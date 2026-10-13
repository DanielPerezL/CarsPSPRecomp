#include "texture.h"

#include <cstring>

#include "../core.h"

namespace {

const int BPP[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};  // bits por texel (DXT: por bloque/16)

inline void c565(uint16_t v, uint8_t *o) {
    o[0] = (v & 0x1F) * 255 / 31; o[1] = (v >> 5 & 0x3F) * 255 / 63; o[2] = (v >> 11) * 255 / 31; o[3] = 255;
}
inline void c5551(uint16_t v, uint8_t *o) {
    o[0] = (v & 0x1F) * 255 / 31; o[1] = (v >> 5 & 0x1F) * 255 / 31; o[2] = (v >> 10 & 0x1F) * 255 / 31;
    o[3] = v >> 15 ? 255 : 0;
}
inline void c4444(uint16_t v, uint8_t *o) {
    o[0] = (v & 0xF) * 17; o[1] = (v >> 4 & 0xF) * 17; o[2] = (v >> 8 & 0xF) * 17; o[3] = (v >> 12) * 17;
}

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ULL) {
    // FNV-1a en palabras de 8 bytes (suficiente para detectar cambios de contenido)
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

size_t row_bytes(const TexParams &p) {
    size_t rb = (size_t)p.bufw * BPP[p.format] / 8;
    return p.swizzled && rb < 16 ? 16 : rb;
}

size_t data_bytes(const TexParams &p) {
    if (p.format >= 8) return (size_t)(p.bufw / 4) * (p.h / 4) * (p.format == 8 ? 8 : 16);
    int rows = p.swizzled ? (p.h + 7) / 8 * 8 : p.h;
    return row_bytes(p) * rows;
}

// Lee los datos de textura en orden lineal (deshaciendo el swizzle de bloques 16 bytes x 8 filas).
void linear_data(const TexParams &p, std::vector<uint8_t> &out) {
    size_t n = data_bytes(p);
    out.resize(n);
    const uint8_t *src = MEMP(p.addr);
    if (!p.swizzled || p.format >= 8) {
        memcpy(out.data(), src, n);
        return;
    }
    size_t rb = row_bytes(p), bx = rb / 16;
    int rows = (p.h + 7) / 8 * 8;
    size_t i = 0;
    for (int by = 0; by < rows; by += 8)
        for (size_t x = 0; x < bx; x++)
            for (int r = 0; r < 8; r++, i += 16) memcpy(&out[(by + r) * rb + x * 16], src + i, 16);
}

void clut_color(const uint8_t *clut, uint32_t clutfmt, uint32_t index, uint8_t *o) {
    uint32_t shift = clutfmt >> 2 & 0x1F, mask = clutfmt >> 8 & 0xFF, start = (clutfmt >> 16 & 0x1F) << 4;
    uint32_t i = ((index >> shift) & mask) | start;
    switch (clutfmt & 3) {
    case 3: memcpy(o, clut + (i & 0x1FF) * 4, 4); break;
    default: {
        uint16_t v;
        memcpy(&v, clut + (i & 0x3FF) * 2, 2);
        if ((clutfmt & 3) == 0) c565(v, o);
        else if ((clutfmt & 3) == 1) c5551(v, o);
        else c4444(v, o);
    }
    }
}

void dxt_block(const uint8_t *b, int format, uint8_t out[16][4]) {
    const uint8_t *cb = format == 8 ? b : b + 8;  // en DXT3/5 el color va después del alfa
    // En la PSP el bloque DXT está reordenado: índices (4 bytes) y luego los dos colores 565.
    uint32_t idx;
    uint16_t c0, c1;
    memcpy(&idx, cb, 4);
    memcpy(&c0, cb + 4, 2);
    memcpy(&c1, cb + 6, 2);
    uint8_t pal[4][4];
    c565(c0, pal[0]);
    c565(c1, pal[1]);
    // PSP: el 565 de DXT tiene el rojo en los bits altos
    std::swap(pal[0][0], pal[0][2]);
    std::swap(pal[1][0], pal[1][2]);
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || format != 8) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || format != 8) ? 255 : 0;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[idx >> (i * 2) & 3], 4);
    if (format == 9) {  // DXT3: alfa explícito de 4 bits
        uint64_t a;
        memcpy(&a, b, 8);
        for (int i = 0; i < 16; i++) out[i][3] = (a >> (i * 4) & 0xF) * 17;
    } else if (format == 10) {  // DXT5: alfa interpolado
        uint8_t a0 = b[6], a1 = b[7];
        uint64_t bits = 0;
        memcpy(&bits, b, 6);
        uint8_t ap[8] = {a0, a1};
        for (int k = 2; k < 8; k++)
            ap[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? 0 : 255);
        for (int i = 0; i < 16; i++) out[i][3] = ap[bits >> (i * 3) & 7];
    }
}

}  // namespace

uint64_t texture_hash(const TexParams &p, const uint8_t *clut) {
    uint64_t h = fnv(MEMP(p.addr), data_bytes(p));
    h ^= (uint64_t)p.format << 56 ^ (uint64_t)p.w << 40 ^ (uint64_t)p.h << 24;
    if (p.format >= 4 && p.format <= 7) h = fnv(clut, (p.clutfmt & 3) == 3 ? 1024 : 512, h ^ p.clutfmt);
    return h;
}

uint64_t texture_decode(const TexParams &p, const uint8_t *clut, std::vector<uint8_t> &rgba) {
    std::vector<uint8_t> lin;
    linear_data(p, lin);
    rgba.assign((size_t)p.w * p.h * 4, 0);
    size_t rb = row_bytes(p);
    if (p.format >= 8) {
        size_t bs = p.format == 8 ? 8 : 16, bpr = p.bufw / 4;
        uint8_t blk[16][4];
        for (int by = 0; by < p.h / 4; by++)
            for (int bx = 0; bx < p.w / 4; bx++) {
                dxt_block(&lin[(by * bpr + bx) * bs], p.format, blk);
                for (int i = 0; i < 16; i++)
                    memcpy(&rgba[(((size_t)by * 4 + i / 4) * p.w + bx * 4 + i % 4) * 4], blk[i], 4);
            }
        return texture_hash(p, clut);
    }
    for (int y = 0; y < p.h; y++) {
        const uint8_t *row = &lin[y * rb];
        uint8_t *o = &rgba[(size_t)y * p.w * 4];
        for (int x = 0; x < p.w; x++, o += 4) {
            uint16_t v16;
            switch (p.format) {
            case 0: memcpy(&v16, row + x * 2, 2); c565(v16, o); break;
            case 1: memcpy(&v16, row + x * 2, 2); c5551(v16, o); break;
            case 2: memcpy(&v16, row + x * 2, 2); c4444(v16, o); break;
            case 3: memcpy(o, row + x * 4, 4); break;
            case 4: clut_color(clut, p.clutfmt, row[x >> 1] >> ((x & 1) * 4) & 0xF, o); break;
            case 5: clut_color(clut, p.clutfmt, row[x], o); break;
            case 6: memcpy(&v16, row + x * 2, 2); clut_color(clut, p.clutfmt, v16, o); break;
            case 7: {
                uint32_t v;
                memcpy(&v, row + x * 4, 4);
                clut_color(clut, p.clutfmt, v, o);
                break;
            }
            }
        }
    }
    return texture_hash(p, clut);
}
