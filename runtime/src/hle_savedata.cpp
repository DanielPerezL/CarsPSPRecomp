// sceUtility: partidas guardadas (Savedata). Los diálogos de mensaje están en hle_dialog.cpp.
// Las partidas se guardan sin cifrar en <saves>/PSP/SAVEDATA/<juego><partida>/, con la misma
// estructura que en el Memory Stick: el fichero de datos, PARAM.BIN (parámetros SFO) e ICON0.PNG.
// Los modos sin interfaz (autoguardado, autocarga, tamaños...) terminan al momento; Guardar y Cargar
// muestran la pantalla del sistema (confirmación, "Guardando...", resultado) sobre el juego, como la PSP.
// Estructuras, códigos y pantallas según PPSSPP (Core/Dialog/SavedataParam.*, PSPSaveDialog.cpp).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "display.h"
#include "threads.h"
#include "ui/dialog.h"

namespace fs = std::filesystem;

namespace {

// SceUtilitySavedataParam
constexpr uint32_t P_RESULT = 0x1C, P_MODE = 0x30, P_GAME = 0x3C, P_SAVE = 0x4C, P_SAVELIST = 0x60, P_FILE = 0x64,
                   P_BUF = 0x74, P_BUFSIZE = 0x78, P_DATASIZE = 0x7C, P_SFO = 0x80, SFO_SIZE = 0x504, P_ICON0 = 0x584,
                   P_MSFREE = 0x5D0, P_MSDATA = 0x5D4, P_UTILDATA = 0x5D8;
constexpr uint32_t SFO_SAVE_TITLE = 0x80;  // SceUtilitySavedataSFOParam: title, savedataTitle, detail...
constexpr uint32_t ERR_LOAD_NO_DATA = 0x80110307, ERR_SIZES_NO_DATA = 0x801103C7, ERR_DELETE_NO_DATA = 0x80110347;
constexpr uint32_t ERR_INVALID_STATUS = 0x80110001, RESULT_CANCEL = 1;
constexpr uint32_t CLUSTER = 0x8000;
constexpr uint32_t BTN_RIGHT = 0x20, BTN_LEFT = 0x80, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000;

enum Status { NONE = 0, INIT = 1, RUNNING = 2, FINISHED = 3, SHUTDOWN = 4 };

// Pantallas de Guardar/Cargar
enum Screen {
    HIDDEN, SAVE_CONFIRM, SAVE_OVERWRITE, SAVING, SAVE_DONE, SAVE_FAILED,
    LOAD_CONFIRM, LOADING, LOAD_DONE, LOAD_FAILED, NO_DATA
};

int g_save_status = NONE;
Screen g_screen = HIDDEN;
uint32_t g_param = 0, g_prev_buttons = 0, g_error = 0;
std::string g_dir;
std::chrono::steady_clock::time_point g_busy_until;
int g_icon_serial = 0;

std::string str(uint32_t addr, size_t max) { return std::string(mem_str(addr), strnlen(mem_str(addr), max)); }

std::string save_dir(const std::string &game, const std::string &save) {
    return g_save_root + "/PSP/SAVEDATA/" + game + save;
}

void mkdirs(const std::string &path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t n = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(n);
    return true;
}

void write_file(const std::string &path, const uint8_t *data, size_t n) {
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}

uint64_t dir_size(const std::string &dir, bool *exists) {
    uint64_t total = 0;
    std::error_code ec;
    *exists = fs::is_directory(dir, ec);
    if (*exists)
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) total += (e.file_size(ec) + CLUSTER - 1) / CLUSTER * CLUSTER;
    return total;
}

void size_text(uint64_t bytes, uint32_t addr) {
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(bytes / 1024));
    memcpy(MEMP(addr), buf, 8);  // el campo de la PSP es de 8 caracteres
}

// Primera partida de saveNameList que exista (o la primera de la lista si ninguna existe).
std::string pick_from_list(uint32_t p, const std::string &game, bool must_exist) {
    uint32_t list = R32(p + P_SAVELIST);
    std::string first;
    for (int i = 0; list && i < 128; i++) {
        std::string name = str(list + i * 20, 20);
        if (name.empty()) break;
        if (first.empty()) first = name;
        bool exists;
        dir_size(save_dir(game, name), &exists);
        if (exists) return name;
    }
    return must_exist ? std::string() : first;
}

uint32_t do_load(uint32_t p, const std::string &dir) {
    std::vector<uint8_t> data;
    std::string file = str(p + P_FILE, 13);
    if (!read_file(dir + "/" + file, data)) return ERR_LOAD_NO_DATA;
    uint32_t bufsize = R32(p + P_BUFSIZE), n = (uint32_t)std::min<size_t>(data.size(), bufsize);
    memcpy(MEMP(R32(p + P_BUF)), data.data(), n);
    W32(p + P_DATASIZE, n);
    std::vector<uint8_t> sfo;
    if (read_file(dir + "/PARAM.BIN", sfo) && sfo.size() == SFO_SIZE) memcpy(MEMP(p + P_SFO), sfo.data(), SFO_SIZE);
    LOGI("savedata", "cargada %s/%s (%u bytes)", dir.c_str(), file.c_str(), n);
    return 0;
}

uint32_t do_save(uint32_t p, const std::string &dir) {
    mkdirs(dir);
    std::string file = str(p + P_FILE, 13);
    if (!file.empty()) write_file(dir + "/" + file, MEMP(R32(p + P_BUF)), R32(p + P_DATASIZE));
    write_file(dir + "/PARAM.BIN", MEMP(p + P_SFO), SFO_SIZE);
    uint32_t icon = R32(p + P_ICON0), icon_size = R32(p + P_ICON0 + 8);
    if (icon && icon_size) write_file(dir + "/ICON0.PNG", MEMP(icon), icon_size);
    LOGI("savedata", "guardada %s/%s (%u bytes)", dir.c_str(), file.c_str(), R32(p + P_DATASIZE));
    return 0;
}

uint32_t do_sizes(uint32_t p) {
    uint32_t ret = 0;
    if (uint32_t f = R32(p + P_MSFREE)) {  // 1 GB libre
        W32(f, CLUSTER);
        W32(f + 4, 0x8000);
        W32(f + 8, 0x100000);
        size_text(1ULL << 30, f + 12);
    }
    if (uint32_t d = R32(p + P_MSDATA)) {
        bool exists;
        uint64_t used = dir_size(save_dir(str(d, 13), str(d + 16, 20)), &exists);
        W32(d + 36, (uint32_t)(used / CLUSTER));
        W32(d + 40, (uint32_t)(used / 1024));
        size_text(used, d + 44);
        W32(d + 52, (uint32_t)(used / 1024));
        size_text(used, d + 56);
        if (!exists) ret = ERR_SIZES_NO_DATA;
    }
    if (uint32_t u = R32(p + P_UTILDATA)) {  // espacio que ocuparía esta partida
        uint64_t total = 2 * CLUSTER + (R32(p + P_DATASIZE) + CLUSTER - 1) / CLUSTER * CLUSTER +
                         (R32(p + P_ICON0 + 8) + CLUSTER - 1) / CLUSTER * CLUSTER;
        W32(u, (uint32_t)(total / CLUSTER));
        W32(u + 4, (uint32_t)(total / 1024));
        size_text(total, u + 8);
        W32(u + 16, (uint32_t)(total / 1024));
        size_text(total, u + 20);
    }
    return ret;
}

// Datos de la partida que se muestran bajo el icono: título, fecha y tamaño.
std::string save_info(const std::string &title, time_t when, uint64_t bytes) {
    char date[64];
    tm t{};
#ifdef _WIN32
    localtime_s(&t, &when);
#else
    localtime_r(&when, &t);
#endif
    strftime(date, sizeof(date), "%d/%m/%Y  %H:%M", &t);
    return title + "\n" + date + "\n" + std::to_string((unsigned long long)((bytes + 1023) / 1024)) + " KB";
}

// Muestra los datos de la partida nueva (la que el juego quiere guardar) o de la que ya existe en disco.
void show_save(bool existing) {
    SystemDialog &d = g_dialog;
    uint32_t p = g_param;
    d.icon_png.clear();
    if (existing) {
        std::vector<uint8_t> sfo;
        std::string title;
        if (read_file(g_dir + "/PARAM.BIN", sfo) && sfo.size() == SFO_SIZE)
            title.assign((const char *)sfo.data() + SFO_SAVE_TITLE, strnlen((const char *)sfo.data() + SFO_SAVE_TITLE, 0x80));
        std::error_code ec;
        auto mtime = fs::last_write_time(g_dir + "/" + str(p + P_FILE, 13), ec);
        time_t when = ec ? time(nullptr)
                         : std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                               mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()));
        uint64_t bytes = 0;  // tamaño real de los ficheros (sin redondear a clústeres)
        for (const auto &e : fs::directory_iterator(g_dir, ec))
            if (e.is_regular_file(ec)) bytes += e.file_size(ec);
        d.info = save_info(title, when, bytes);
        read_file(g_dir + "/ICON0.PNG", d.icon_png);
    } else {
        uint32_t icon = R32(p + P_ICON0), icon_size = icon ? R32(p + P_ICON0 + 8) : 0;
        d.info = save_info(str(p + P_SFO + SFO_SAVE_TITLE, 0x80), time(nullptr),
                           (uint64_t)R32(p + P_DATASIZE) + SFO_SIZE + icon_size);
        if (icon && icon_size) d.icon_png.assign(MEMP(icon), MEMP(icon) + icon_size);
    }
    d.icon_dim = false;
    d.icon_serial = ++g_icon_serial;
}

void set_screen(Screen s) {
    SystemDialog &d = g_dialog;
    g_screen = s;
    d.yesno = s == SAVE_CONFIRM || s == SAVE_OVERWRITE || s == LOAD_CONFIRM;
    d.busy = s == SAVING || s == LOADING;
    d.ok = !d.busy;
    d.cancel = !d.busy;
    d.choice = s == SAVE_OVERWRITE ? 0 : 1;  // sobrescribir: "No" por defecto, como la PSP
    d.clicked = -1;
    static const char *const texts[] = {
        "", "¿Quieres guardar los datos?", "¿Quieres sobrescribir los datos?", "Guardando…\nNo cierres el juego.",
        "Se han guardado los datos.", "No se han podido guardar los datos.", "¿Quieres cargar estos datos?",
        "Cargando…", "Se han cargado los datos.", "No se han podido cargar los datos.", "No hay datos."};
    d.text = texts[s];
    if (d.busy) g_busy_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    LOGD("savedata", "pantalla: %s", d.text.c_str());
}

void close_screen(uint32_t result) {
    W32(g_param + P_RESULT, result);
    g_dialog.visible = false;
    display_block_input(false);
    g_screen = HIDDEN;
    g_save_status = FINISHED;
    LOGI("savedata", "pantalla cerrada (resultado %08x)", result);
}

// Abre Guardar (modo 3) o Cargar (modo 2) con su pantalla. Devuelve false si se resuelve sin mostrarla.
bool open_screen(uint32_t p, uint32_t mode, const std::string &dir) {
    int ans = dialog_auto_answer();
    if (ans >= 0) return false;
    g_param = p;
    g_dir = dir;
    SystemDialog &d = g_dialog;
    d = SystemDialog{};
    bool exists = fs::exists(dir + "/" + str(p + P_FILE, 13));
    if (mode == 3) {
        d.banner = "Guardar";
        show_save(exists);
        set_screen(exists ? SAVE_OVERWRITE : SAVE_CONFIRM);
    } else {
        d.banner = "Cargar";
        if (exists) show_save(true);
        set_screen(exists ? LOAD_CONFIRM : NO_DATA);
    }
    d.visible = true;
    display_block_input(true);
    g_prev_buttons = display_buttons_raw();  // un botón ya pulsado no cuenta hasta soltarlo
    g_save_status = INIT;
    return true;
}

// Un fotograma de la pantalla de Guardar/Cargar: botones de la PSP, ratón y teclado (overlay).
void update_screen() {
    SystemDialog &d = g_dialog;
    uint32_t now = display_buttons_raw(), pressed = now & ~g_prev_buttons;
    g_prev_buttons = now;
    int click = d.clicked;
    d.clicked = -1;

    if (d.busy) {
        if (std::chrono::steady_clock::now() < g_busy_until) return;
        if (g_screen == SAVING) {
            g_error = do_save(g_param, g_dir);
            show_save(false);
            set_screen(g_error ? SAVE_FAILED : SAVE_DONE);
        } else {
            g_error = do_load(g_param, g_dir);
            set_screen(g_error ? LOAD_FAILED : LOAD_DONE);
        }
        return;
    }
    if (d.yesno) {
        if (pressed & BTN_LEFT) d.choice = 1;
        if (pressed & BTN_RIGHT) d.choice = 0;
    }
    bool back = click == 3 || (click < 0 && (pressed & BTN_CIRCLE));
    bool accept = click == 1 || click == 2 || (click < 0 && (pressed & BTN_CROSS));
    if (click == 1 || click == 2) d.choice = click == 1 ? 1 : 0;
    if (!back && !accept) return;

    switch (g_screen) {
    case SAVE_CONFIRM:
    case SAVE_OVERWRITE:
    case LOAD_CONFIRM:
        if (back || d.choice == 0) close_screen(RESULT_CANCEL);
        else set_screen(g_screen == LOAD_CONFIRM ? LOADING : SAVING);
        break;
    case SAVE_DONE:
    case LOAD_DONE:
        close_screen(0);
        break;
    case SAVE_FAILED:
    case LOAD_FAILED:
        close_screen(g_error);
        break;
    case NO_DATA:
        close_screen(ERR_LOAD_NO_DATA);
        break;
    default:
        break;
    }
}

}  // namespace

HLE(sceUtilitySavedataInitStart) {
    uint32_t p = A0, mode = R32(p + P_MODE);
    std::string game = str(p + P_GAME, 13), save = str(p + P_SAVE, 20);
    uint32_t result = 0;
    if (g_save_status != NONE && g_save_status != SHUTDOWN) RET(ERR_INVALID_STATUS);
    if ((mode == 2 || mode == 3) && open_screen(p, mode, save_dir(game, save))) {
        LOGD("savedata", "modo %u %s%s: pantalla del sistema", mode, game.c_str(), save.c_str());
        W32(p + P_RESULT, 0);
        RET(0);
    }
    switch (mode) {
    case 0: case 2:  // AUTOLOAD / LOAD
        result = do_load(p, save_dir(game, save));
        break;
    case 1: case 3:  // AUTOSAVE / SAVE
        result = do_save(p, save_dir(game, save));
        break;
    case 4: {  // LISTLOAD: la primera partida existente de la lista
        std::string pick = pick_from_list(p, game, true);
        result = pick.empty() ? ERR_LOAD_NO_DATA : do_load(p, save_dir(game, pick));
        if (!pick.empty()) memcpy(MEMP(p + P_SAVE), pick.c_str(), pick.size() + 1);
        break;
    }
    case 5: {  // LISTSAVE
        std::string pick = save.empty() ? pick_from_list(p, game, false) : save;
        result = do_save(p, save_dir(game, pick));
        break;
    }
    case 6: case 7: {  // LISTDELETE / DELETE
        std::string dir = save_dir(game, save);
        bool exists;
        dir_size(dir, &exists);
        if (!exists) result = ERR_DELETE_NO_DATA;
        else {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
        break;
    }
    case 8:  // SIZES
        result = do_sizes(p);
        break;
    default:
        LOGW("savedata", "modo %u sin implementar (%s%s)", mode, game.c_str(), save.c_str());
        break;
    }
    LOGD("savedata", "modo %u %s%s -> %08x", mode, game.c_str(), save.c_str(), result);
    W32(p + P_RESULT, result);
    g_save_status = RUNNING;  // sin pantalla: termina en el primer Update
    RET(0);
}
HLE(sceUtilitySavedataUpdate) {
    if (g_save_status != RUNNING) RET(ERR_INVALID_STATUS);
    if (g_screen == HIDDEN) g_save_status = FINISHED;
    else update_screen();
    RET(0);
}
HLE(sceUtilitySavedataGetStatus) {
    int s = g_save_status;
    if (g_save_status == INIT) g_save_status = RUNNING;
    else if (g_save_status == SHUTDOWN) g_save_status = NONE;
    RET(s);
}
HLE(sceUtilitySavedataShutdownStart) {
    if (g_save_status != FINISHED) RET(ERR_INVALID_STATUS);
    g_save_status = SHUTDOWN;
    RET(0);
}
