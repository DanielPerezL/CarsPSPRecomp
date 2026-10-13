// Estado de pantalla compartido entre sceDisplay, la GE, la ventana y los mandos.
#pragma once
#include <cstdint>
#include <string>

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t fmt);
void display_get_framebuf(uint32_t *addr, uint32_t *stride, uint32_t *fmt);

// Mandos: ventana (teclado/mando) + pulsaciones programadas para pruebas (CARS_PRESS).
uint32_t display_buttons();
void display_analog(uint8_t *lx, uint8_t *ly);

// Opciones de prueba: capturas en instantes concretos y salida automática.
void display_add_screenshot(double t, const std::string &path);
void display_set_exit_after(double t);

void display_init(bool windowed);
// Se llama en cada vblank: eventos de ventana, presentación y capturas.
void display_vblank();
