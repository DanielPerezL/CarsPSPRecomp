// Estado de pantalla compartido entre sceDisplay, la GE, la ventana y los mandos.
#pragma once
#include <cstdint>
#include <string>

void display_set_framebuf(uint32_t addr, uint32_t stride, uint32_t fmt);
void display_get_framebuf(uint32_t *addr, uint32_t *stride, uint32_t *fmt);

// Mandos: ventana (teclado/mando) + pulsaciones programadas para pruebas (CARS_PRESS).
// display_buttons/display_analog son lo que ve el juego: vacíos mientras un diálogo del sistema tiene el
// control y hasta que se sueltan los botones con que se cerró. display_buttons_raw no se bloquea nunca.
uint32_t display_buttons();
uint32_t display_buttons_raw();
void display_analog(uint8_t *lx, uint8_t *ly);
void display_block_input(bool block);

// Opciones de prueba: capturas en instantes concretos y salida automática.
void display_add_screenshot(double t, const std::string &path);
void display_set_exit_after(double t);

void display_init(bool windowed);
bool display_windowed();
// Se llama en cada vblank: eventos de ventana, presentación y capturas.
void display_vblank();
// Se llama cuando todos los hilos del juego esperan: presenta fotogramas interpolados.
// Devuelve los microsegundos hasta la próxima presentación deseada.
uint64_t display_idle();
