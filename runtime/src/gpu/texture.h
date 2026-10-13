// Decodificación de texturas de la GE a RGBA8 (todos los formatos, swizzle y CLUT).
#pragma once
#include <cstdint>
#include <vector>

struct TexParams {
    uint32_t addr;     // dirección de memoria del nivel 0
    uint32_t bufw;     // ancho del buffer en píxeles
    int w, h;          // tamaño (potencias de 2)
    int format;        // 0..10 (GE_TFMT_*)
    bool swizzled;
    uint32_t clutfmt;  // registro 0xC5 completo
};

// Decodifica el nivel 0; devuelve el hash del contenido (texels + CLUT usada), clave de caché y de sustitución.
uint64_t texture_decode(const TexParams &p, const uint8_t *clut, std::vector<uint8_t> &rgba);
// Hash rápido del contenido sin decodificar (para validar la caché en cada uso).
uint64_t texture_hash(const TexParams &p, const uint8_t *clut);
