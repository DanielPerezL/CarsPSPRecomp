// sceUtilityMsgDialog: mensajes del sistema (avisos, errores y preguntas Sí/No) con interfaz propia.
// El juego sigue dibujando su escena y llama a Update en cada fotograma; el diálogo se pinta encima
// (ui/overlay.cpp) y se contesta con el mando, el teclado o el ratón. Comportamiento según PPSSPP
// (Core/Dialog/PSPMsgDialog.cpp): opciones, botón por defecto, ○ = cancelar y máquina de estados.
#include <cstdlib>
#include <string>

#include "display.h"
#include "threads.h"
#include "ui/dialog.h"

SystemDialog g_dialog;

namespace {

// pspMessageDialog
constexpr uint32_t P_SIZE = 0x00, P_RESULT = 0x1C, P_TYPE = 0x34, P_ERROR = 0x38, P_TEXT = 0x3C,
                   P_OPTIONS = 0x23C, P_PRESSED = 0x240, P_OK_LABEL = 0x244, P_CANCEL_LABEL = 0x284;
constexpr uint32_t SIZE_V2 = 580, SIZE_V3 = 708;
constexpr uint32_t OPT_YESNO = 0x10, OPT_OK = 0x20, OPT_NOCANCEL = 0x80, OPT_DEFAULT_NO = 0x100;
constexpr uint32_t ERR_INVALID_STATUS = 0x80110001, ERR_ERRORCODE_INVALID = 0x80110501;
constexpr uint32_t BTN_RIGHT = 0x20, BTN_LEFT = 0x80, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000;

enum Status { NONE = 0, INIT = 1, RUNNING = 2, FINISHED = 3, SHUTDOWN = 4 };

Status g_status = NONE;
uint32_t g_param = 0, g_size = 0, g_prev_buttons = 0;

// Los textos del juego deberían venir en UTF-8; si no lo son, se interpretan como Latin-1.
std::string to_utf8(const std::string &s) {
    size_t i = 0;
    bool valid = true;
    while (i < s.size() && valid) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || i + n >= s.size()) valid = false;
        for (int k = 1; valid && k <= n; k++) valid = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        i += n + 1;
    }
    if (valid) return s;
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string field(uint32_t addr, size_t max) { return to_utf8(std::string(mem_str(addr), strnlen(mem_str(addr), max))); }

void finish(int pressed) {
    if (g_size >= SIZE_V2) W32(g_param + P_PRESSED, pressed);
    W32(g_param + P_RESULT, 0);
    g_dialog.visible = false;
    display_block_input(false);  // el juego no recibe la pulsación que cierra el diálogo
    g_status = FINISHED;
    LOGI("utility", "diálogo cerrado: botón %d", pressed);
}

}  // namespace

int dialog_auto_answer() {
    const char *ans = getenv("CARS_DIALOG_ANSWER");
    if (ans) return !strcmp(ans, "no") ? 2 : 1;
    return display_windowed() ? -1 : 1;
}

HLE(sceUtilityMsgDialogInitStart) {
    if (g_status != NONE) RET(ERR_INVALID_STATUS);
    uint32_t p = g_param = A0;
    g_size = R32(p + P_SIZE);
    uint32_t type = R32(p + P_TYPE), options = g_size >= SIZE_V2 ? R32(p + P_OPTIONS) : 0;
    bool v3 = g_size >= SIZE_V3;
    g_status = INIT;

    if (type == 0 && !(R32(p + P_ERROR) & 0x80000000)) {  // código de error inválido: termina sin mostrarse
        W32(p + P_RESULT, ERR_ERRORCODE_INVALID);
        g_status = FINISHED;
        RET(0);
    }
    SystemDialog &d = g_dialog;
    d = SystemDialog{};
    if (type == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Se ha producido un error (%08X).", R32(p + P_ERROR));
        d.text = buf;
    } else {
        d.text = field(p + P_TEXT, 512);
    }
    d.yesno = (options & OPT_YESNO) && (v3 || type == 1);
    d.ok = v3 && (options & OPT_OK);
    d.cancel = !(v3 && (options & OPT_NOCANCEL));
    d.choice = (options & OPT_DEFAULT_NO) && !d.ok ? 0 : 1;
    if (v3) {
        d.ok_label = field(p + P_OK_LABEL, 64);
        d.cancel_label = field(p + P_CANCEL_LABEL, 64);
    }
    if (g_log_level >= LOG_TRACE) {  // bytes del texto, para depurar codificaciones
        std::string hex;
        for (unsigned char ch : d.text) { char b[4]; snprintf(b, sizeof(b), "%02x ", ch); hex += b; }
        LOGT("utility", "texto: %s", hex.c_str());
    }
    LOGI("utility", "diálogo (tipo %u, opciones %03x, v%d): %s", type, options, v3 ? 3 : g_size >= SIZE_V2 ? 2 : 1,
         d.text.c_str());

    int ans = dialog_auto_answer();
    if (ans >= 0) {
        finish(d.yesno || d.ok ? ans : (v3 || d.yesno ? 3 : 0));
        RET(0);
    }
    d.visible = true;
    display_block_input(true);
    g_prev_buttons = display_buttons_raw();  // un botón ya pulsado no cuenta hasta soltarlo
    RET(0);
}

HLE(sceUtilityMsgDialogUpdate) {
    if (g_status != RUNNING) RET(ERR_INVALID_STATUS);
    SystemDialog &d = g_dialog;
    uint32_t now = display_buttons_raw(), pressed = now & ~g_prev_buttons;
    g_prev_buttons = now;
    bool v3 = g_size >= SIZE_V3;

    if (d.yesno) {
        if (pressed & BTN_LEFT) d.choice = 1;
        if (pressed & BTN_RIGHT) d.choice = 0;
    }
    int click = d.clicked;
    d.clicked = -1;
    if (click == 3 || (click < 0 && (pressed & BTN_CIRCLE))) {
        if (d.cancel) finish(v3 || d.yesno ? 3 : 0);
    } else if (click == 1 || click == 2 || (pressed & BTN_CROSS)) {
        if (click > 0 && d.yesno) d.choice = click == 1 ? 1 : 0;
        if (d.yesno || d.ok) finish(d.yesno && d.choice == 0 ? 2 : 1);
    }
    if (g_status == RUNNING) W32(g_param + P_RESULT, 0);
    RET(0);
}

HLE(sceUtilityMsgDialogGetStatus) {
    Status s = g_status;
    if (g_status == INIT) g_status = RUNNING;
    else if (g_status == SHUTDOWN) g_status = NONE;
    RET(s);
}

HLE(sceUtilityMsgDialogShutdownStart) {
    if (g_status != FINISHED) RET(ERR_INVALID_STATUS);
    g_status = SHUTDOWN;
    RET(0);
}

HLE(sceUtilityMsgDialogAbort) {
    if (g_status != RUNNING) RET(ERR_INVALID_STATUS);
    finish(0);
    RET(0);
}
