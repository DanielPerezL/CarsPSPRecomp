// Estado de la GE (GPU de la PSP) compartido entre el intérprete de display lists y el renderizador.
#pragma once
#include <cstdint>
#include <cstring>

// Vértice ya decodificado y listo para el renderizador (posición en espacio de modelo, o de pantalla en "through").
struct GpuVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint8_t col[4];
};

enum class GpuPrim { Triangles, Lines, Points };

struct GeState {
    uint32_t cmd[256] = {};
    float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float view[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float tgen[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float bone[8 * 12] = {};
    float morph[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    int world_i = 0, view_i = 0, proj_i = 0, tgen_i = 0, bone_i = 0;
    uint32_t vaddr = 0, iaddr = 0, offset = 0;
    uint8_t clut[2048] = {};

    uint32_t arg(int c) const { return cmd[c] & 0xFFFFFF; }
    bool through() const { return cmd[0x12] >> 23 & 1; }
    float f24(int c) const {
        uint32_t v = cmd[c] << 8;
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    // Color de 24 bits (0xBBGGRR) normalizado
    void color(int c, float out[3]) const {
        uint32_t v = arg(c);
        out[0] = (v & 0xFF) / 255.f;
        out[1] = (v >> 8 & 0xFF) / 255.f;
        out[2] = (v >> 16 & 0xFF) / 255.f;
    }
    uint32_t fb_addr() const { return arg(0x9C) & 0x1FFFF0; }  // desplazamiento dentro de la VRAM
    uint32_t tex_addr(int level = 0) const {
        return (arg(0xA0 + level) & 0xFFFFF0) | ((cmd[0xA8 + level] & 0x0F0000) << 8);
    }
    uint32_t tex_bufw(int level = 0) const { return cmd[0xA8 + level] & 0x7FF; }
    int tex_w(int level = 0) const { return 1 << (cmd[0xB8 + level] & 0xF); }
    int tex_h(int level = 0) const { return 1 << (cmd[0xB8 + level] >> 8 & 0xF); }
};

extern GeState g_ge;

float f24_to_float(uint32_t arg);
