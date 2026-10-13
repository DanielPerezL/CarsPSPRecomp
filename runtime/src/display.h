// Estado de pantalla compartido entre sceDisplay, la GE y el frontend.
#pragma once
#include <cstdint>

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t fmt);
void display_get_framebuf(uint32_t *addr, uint32_t *stride, uint32_t *fmt);
uint32_t display_buttons();
// Vuelca el framebuffer actual a un PPM (diagnóstico de la F0).
void display_dump(const char *path);
void display_init();
