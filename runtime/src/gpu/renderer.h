// Interfaz del renderizador (backend OpenGL en renderer_gl.cpp).
//
// Sin interpolación cada dibujo se ejecuta al momento y se presenta en el vblank de la PSP.
// Con interpolación los dibujos de cada fotograma lógico se graban (estado + vértices) y se cierran al
// cambiar de framebuffer (flip); el presentador vuelve a dibujar el último fotograma interpolando desde
// el anterior tantas veces por segundo como permita el monitor, con un fotograma lógico de retraso.
#pragma once
#include <cstdint>
#include <string>

#include "ge.h"

struct RendererConfig {
    int scale = 4;            // resolución interna = 480x272 * scale (4 -> 1920x1088)
    bool interpolate = true;  // fotogramas intermedios entre los fotogramas lógicos del juego
    int max_fps = 0;          // límite de presentación con interpolación (0 = frecuencia del monitor)
    std::string textures_dir = "textures";  // sustituciones <hash>.png
    std::string dump_dir;                   // si no está vacío, vuelca aquí las texturas originales
};

bool renderer_init(const RendererConfig &cfg);
bool renderer_interpolating();
// Dibuja (o graba) vértices ya ensamblados con el estado actual de la GE.
// match_key identifica el dibujo entre fotogramas (origen de vértices, textura, formato...).
void renderer_draw(GpuPrim prim, const GpuVertex *v, int count, uint64_t match_key);
// Copia por bloques de la GE (TRANSFER): invalida lo que haya en caché en el destino.
void renderer_invalidate(uint32_t addr, uint32_t size);
// El juego cambia el framebuffer visible (sceDisplaySetFrameBuf): cierra el fotograma lógico.
void renderer_flip(uint32_t fb_addr, uint32_t stride, uint32_t fmt);
// Presenta en la ventana: sin interpolación, el framebuffer visible; con interpolación, el
// fotograma interpolado para el instante actual. Devuelve false si aún no toca presentar.
bool renderer_present(uint32_t fb_addr, uint32_t stride, uint32_t fmt);
// Microsegundos hasta la próxima presentación interpolada (para el planificador).
uint64_t renderer_next_present_in();
// Captura el último fotograma presentado a un PPM.
bool renderer_screenshot(const std::string &path);
