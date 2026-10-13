// Ventana (SDL2 + OpenGL 3.3 core) y entrada (teclado y mando) mapeada a los botones de la PSP.
#pragma once
#include <cstdint>
#include <string>

struct WindowConfig {
    int width = 1280, height = 720;
    bool fullscreen = false;
    bool hidden = false;  // pruebas automáticas: sin ventana visible, se renderiza igual a los FBO
    bool vsync = false;
    std::string ui_font;  // TTF de la interfaz propia (vacío = una del sistema)
};

bool window_init(const WindowConfig &cfg);
void window_shutdown();
// Procesa eventos; devuelve false si el usuario cierra la ventana.
bool window_poll();
void window_swap();
void window_drawable_size(int *w, int *h);

// Estado de mandos en formato sceCtrl: botones PSP_CTRL_* y stick analógico 0..255 (128 = centro).
struct PadState {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128;
};
PadState window_pad();
