// sceUtility: partidas guardadas (Savedata) y diálogos de mensaje del sistema.
// Las partidas se guardan sin cifrar en <saves>/PSP/SAVEDATA/<juego><partida>/, con la misma
// estructura que en el Memory Stick: el fichero de datos, PARAM.BIN (parámetros SFO) e ICON0.PNG.
// Estructuras y códigos según PPSSPP (Core/Dialog/SavedataParam.*).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "threads.h"

namespace fs = std::filesystem;

namespace {

// SceUtilitySavedataParam
constexpr uint32_t P_RESULT = 0x1C, P_MODE = 0x30, P_GAME = 0x3C, P_SAVE = 0x4C, P_SAVELIST = 0x60, P_FILE = 0x64,
                   P_BUF = 0x74, P_BUFSIZE = 0x78, P_DATASIZE = 0x7C, P_SFO = 0x80, SFO_SIZE = 0x504, P_ICON0 = 0x584,
                   P_MSFREE = 0x5D0, P_MSDATA = 0x5D4, P_UTILDATA = 0x5D8;
constexpr uint32_t ERR_LOAD_NO_DATA = 0x80110307, ERR_SIZES_NO_DATA = 0x801103C7, ERR_DELETE_NO_DATA = 0x80110347;
constexpr uint32_t CLUSTER = 0x8000;

int g_save_status = 0, g_msg_status = 0;

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

}  // namespace

HLE(sceUtilitySavedataInitStart) {
    uint32_t p = A0, mode = R32(p + P_MODE);
    std::string game = str(p + P_GAME, 13), save = str(p + P_SAVE, 20);
    uint32_t result = 0;
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
    g_save_status = 2;
    RET(0);
}
HLE(sceUtilitySavedataUpdate) {
    if (g_save_status == 2) g_save_status = 3;
    RET(0);
}
HLE(sceUtilitySavedataGetStatus) { RET(g_save_status); }
HLE(sceUtilitySavedataShutdownStart) {
    g_save_status = 0;
    RET(0);
}

// ---------------------------------------------------------------- diálogo de mensaje
// Sin interfaz propia todavía: se registra el texto y se responde automáticamente
// (CARS_DIALOG_ANSWER=yes|no, por defecto sí).
HLE(sceUtilityMsgDialogInitStart) {
    uint32_t p = A0;
    LOGI("utility", "diálogo: %s", mem_str(p + 0x3C));
    const char *ans = getenv("CARS_DIALOG_ANSWER");
    W32(p + 0x1C, 0);                                       // resultado
    W32(p + 0x240, ans && !strcmp(ans, "no") ? 2 : 1);      // botón pulsado: 1 = sí, 2 = no
    g_msg_status = 2;
    RET(0);
}
HLE(sceUtilityMsgDialogUpdate) {
    if (g_msg_status == 2) g_msg_status = 3;
    RET(0);
}
HLE(sceUtilityMsgDialogGetStatus) { RET(g_msg_status); }
HLE(sceUtilityMsgDialogShutdownStart) {
    g_msg_status = 0;
    RET(0);
}
