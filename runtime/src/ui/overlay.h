// Interfaz propia dibujada sobre la imagen del juego (Dear ImGui): diálogos del sistema.
#pragma once
#include <string>

// Tras crear el contexto OpenGL de la ventana. font: TTF a usar (vacío = buscar una del sistema).
bool overlay_init(void *sdl_window, void *gl_context, const char *font);
void overlay_shutdown();
// Eventos de SDL de la ventana (ratón y teclado mientras hay algo en pantalla).
void overlay_event(const void *sdl_event);
// Aviso breve en una esquina (p. ej. "Texturas recargadas").
void overlay_toast(const std::string &text);
// Dibuja sobre el framebuffer de la ventana, justo antes de presentarlo.
void overlay_render();
