#include "display.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "threads.h"

static uint32_t g_fb_addr = 0, g_fb_stride = 512, g_fb_fmt = 3;

// Pulsaciones programadas para pruebas sin ventana: CARS_PRESS="2.5:start,4:cross" (segundos:botón)
struct Press {
    double t;
    uint32_t mask;
};
static std::vector<Press> g_presses;

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t fmt) {
    if (addr != g_fb_addr) LOGT("display", "framebuffer %08x stride=%u fmt=%u", addr, stride, fmt);
    g_fb_addr = addr;
    g_fb_stride = stride;
    g_fb_fmt = fmt;
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

void display_init() {
    if (const char *p = getenv("CARS_PRESS")) {
        std::string s = p;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            std::string item = s.substr(i, j - i);
            size_t colon = item.find(':');
            if (colon != std::string::npos)
                g_presses.push_back({atof(item.substr(0, colon).c_str()), button_mask(item.substr(colon + 1))});
            i = j + 1;
        }
    }
}

uint32_t display_buttons() {
    double now = time_us() / 1e6;
    uint32_t mask = 0;
    for (auto &p : g_presses)
        if (now >= p.t && now < p.t + 0.15) mask |= p.mask;
    return mask;
}

void display_dump(const char *path) {
    if (!g_fb_addr) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++) {
            uint8_t rgb[3];
            if (g_fb_fmt == 3) {
                uint32_t p = R32(g_fb_addr + (y * g_fb_stride + x) * 4);
                rgb[0] = p & 0xFF; rgb[1] = p >> 8 & 0xFF; rgb[2] = p >> 16 & 0xFF;
            } else {
                uint16_t p = R16(g_fb_addr + (y * g_fb_stride + x) * 2);
                if (g_fb_fmt == 0) { rgb[0] = (p & 0x1F) << 3; rgb[1] = (p >> 5 & 0x3F) << 2; rgb[2] = (p >> 11) << 3; }
                else if (g_fb_fmt == 1) { rgb[0] = (p & 0x1F) << 3; rgb[1] = (p >> 5 & 0x1F) << 3; rgb[2] = (p >> 10 & 0x1F) << 3; }
                else { rgb[0] = (p & 0xF) << 4; rgb[1] = (p >> 4 & 0xF) << 4; rgb[2] = (p >> 8 & 0xF) << 4; }
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}
