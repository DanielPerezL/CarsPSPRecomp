// Interfaz del renderizador (backend OpenGL en renderer_gl.cpp). Lee el estado de g_ge en cada dibujo.
#pragma once
#include <cstdint>
#include <string>

#include "ge.h"

struct RendererConfig {
    int scale = 4;  // resolución interna = 480x272 * scale (4 -> 1920x1088)
};

bool renderer_init(const RendererConfig &cfg);
// Dibuja vértices ya ensamblados (listas de triángulos/líneas/puntos) con el estado actual de la GE.
void renderer_draw(GpuPrim prim, const GpuVertex *v, int count);
// Copia por bloques de la GE (TRANSFER): invalida lo que haya en caché en el destino.
void renderer_invalidate(uint32_t addr, uint32_t size);
// Muestra en la ventana el framebuffer de la PSP en la dirección dada (sceDisplaySetFrameBuf).
void renderer_present(uint32_t fb_addr, uint32_t stride, uint32_t fmt);
// Captura el último fotograma presentado a un PPM.
bool renderer_screenshot(const std::string &path);
