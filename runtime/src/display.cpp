#include "display.h"

#include <cstdlib>
#include <vector>

#include "gpu/renderer.h"
#include "platform/window.h"
#include "threads.h"

static uint32_t g_fb_addr = 0, g_fb_stride = 512, g_fb_fmt = 3;
static bool g_windowed = false;

// Pulsaciones programadas para pruebas: CARS_PRESS="2.5:start,4:cross,10:cross:20" (segundos:botón[:duración]);
// CARS_PRESS_LEN fija la duración por defecto (0,15 s)
struct Press {
    double t;
    uint32_t mask;
    double len;
};
static std::vector<Press> g_presses;
struct Shot {
    double t;
    std::string path;
    bool done = false;
};
static std::vector<Shot> g_shots;
static double g_exit_after = 0;
static double g_press_len = 0.15;

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t fmt) {
    if (addr != g_fb_addr) LOGT("display", "framebuffer %08x stride=%u fmt=%u", addr, stride, fmt);
    if (addr) {  // addr = 0 desactiva la salida de vídeo: se mantiene el último
        g_fb_addr = addr;
        g_fb_stride = stride;
        g_fb_fmt = fmt;
        if (g_windowed) renderer_flip(addr, stride, fmt);
    }
}

void display_get_framebuf(uint32_t *addr, uint32_t *stride, uint32_t *fmt) {
    *addr = g_fb_addr;
    *stride = g_fb_stride;
    *fmt = g_fb_fmt;
}

static uint32_t button_mask(const std::string &name) {
    static const struct { const char *n; uint32_t m; } map[] = {
        {"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
        {"ltrigger", 0x100}, {"rtrigger", 0x200}, {"triangle", 0x1000}, {"circle", 0x2000},
        {"cross", 0x4000}, {"square", 0x8000}};
    for (auto &e : map)
        if (name == e.n) return e.m;
    return 0;
}

void display_init(bool windowed) {
    g_windowed = windowed;
    if (const char *l = getenv("CARS_PRESS_LEN")) g_press_len = atof(l);
    if (const char *p = getenv("CARS_PRESS")) {
        std::string s = p;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            std::string item = s.substr(i, j - i);
            size_t c1 = item.find(':');
            if (c1 != std::string::npos) {
                size_t c2 = item.find(':', c1 + 1);
                std::string btn = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                double len = c2 == std::string::npos ? g_press_len : atof(item.substr(c2 + 1).c_str());
                g_presses.push_back({atof(item.substr(0, c1).c_str()), button_mask(btn), len});
            }
            i = j + 1;
        }
    }
}

void display_add_screenshot(double t, const std::string &path) { g_shots.push_back({t, path}); }
void display_set_exit_after(double t) { g_exit_after = t; }

uint32_t display_buttons() {
    double now = time_us() / 1e6;
    uint32_t mask = g_windowed ? window_pad().buttons : 0;
    for (auto &p : g_presses)
        if (now >= p.t && now < p.t + p.len) mask |= p.mask;
    return mask;
}

void display_analog(uint8_t *lx, uint8_t *ly) {
    *lx = *ly = 128;
    if (g_windowed) {
        PadState p = window_pad();
        *lx = p.lx;
        *ly = p.ly;
    }
}

void display_vblank() {
    if (!g_windowed) return;
    if (!window_poll()) {
        LOGI("display", "ventana cerrada");
        exit(0);
    }
    if (!renderer_interpolating()) renderer_present(g_fb_addr, g_fb_stride, g_fb_fmt);
    double now = time_us() / 1e6;
    for (auto &s : g_shots)
        if (!s.done && now >= s.t) {
            renderer_screenshot(s.path);
            s.done = true;
        }
    if (g_exit_after > 0 && now >= g_exit_after) {
        LOGI("display", "fin de la prueba (%.1f s)", g_exit_after);
        exit(0);
    }
}

uint64_t display_idle() {
    if (!g_windowed || !renderer_interpolating()) return UINT64_MAX;
    if (renderer_present(g_fb_addr, g_fb_stride, g_fb_fmt) && !window_poll()) {
        LOGI("display", "ventana cerrada");
        exit(0);
    }
    return renderer_next_present_in();
}
