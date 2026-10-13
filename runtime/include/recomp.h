// Interfaz entre el código recompilado (build/gen/*.c) y el runtime.
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ctx {
    uint32_t r[32];
    uint32_t hi, lo;
    union {
        float f[32];
        uint32_t fi[32];
    };
    uint32_t fcr31;
    uint32_t fpcond;
    // VFPU: 128 registros (8 matrices de 4x4) y registros de control (prefijos, cc...)
    union {
        float v[128];
        uint32_t vi[128];
    };
    uint32_t vfpu_ctrl[16];
    uint32_t pc;  // última posición conocida (para diagnósticos)
} Ctx;

typedef void (*RecompFunc)(Ctx *c);

// Memoria de la PSP: un bloque de 512 MB direccionado con addr & 0x1FFFFFFF
// (cubre los espejos sin caché 0x4xxxxxxx y el kernel 0x8xxxxxxx).
extern uint8_t *g_mem;
#define MEMP(a) (g_mem + ((uint32_t)(a) & 0x1FFFFFFFu))

static inline uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, MEMP(a), 4); return v; }
static inline uint16_t R16(uint32_t a) { uint16_t v; memcpy(&v, MEMP(a), 2); return v; }
static inline uint8_t R8(uint32_t a) { return *MEMP(a); }
static inline void W32(uint32_t a, uint32_t v) { memcpy(MEMP(a), &v, 4); }
static inline void W16(uint32_t a, uint16_t v) { memcpy(MEMP(a), &v, 2); }
static inline void W8(uint32_t a, uint8_t v) { *MEMP(a) = v; }

static inline uint32_t op_lwl(uint32_t addr, uint32_t rt) {
    uint32_t shift = (addr & 3) * 8, mem = R32(addr & ~3u);
    return (rt & (0x00FFFFFFu >> shift)) | (mem << (24 - shift));
}
static inline uint32_t op_lwr(uint32_t addr, uint32_t rt) {
    uint32_t shift = (addr & 3) * 8, mem = R32(addr & ~3u);
    return (rt & (0xFFFFFF00u << (24 - shift))) | (mem >> shift);
}
static inline void op_div(Ctx *c, int32_t a, int32_t b) {
    if (b == 0) { c->lo = a < 0 ? 1 : 0xFFFFFFFFu; c->hi = (uint32_t)a; }
    else if (a == INT32_MIN && b == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
static inline void op_divu(Ctx *c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = a > 0xFFFF ? 0xFFFFFFFFu : 0xFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}
static inline void op_mult(Ctx *c, int64_t r) { c->lo = (uint32_t)r; c->hi = (uint32_t)((uint64_t)r >> 32); }
static inline int64_t hilo(Ctx *c) { return (int64_t)(((uint64_t)c->hi << 32) | c->lo); }
static inline uint32_t op_clz(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t op_bitrev(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
static inline uint32_t op_ext(uint32_t rs, uint32_t pos, uint32_t size) {
    return (rs >> pos) & (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1));
}
static inline uint32_t op_ins(uint32_t rt, uint32_t rs, uint32_t pos, uint32_t size) {
    uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
    return (rt & ~mask) | ((rs << pos) & mask);
}
// float -> int32 al estilo PSP: NaN y desbordes saturan a INT32_MAX/INT32_MIN
static inline uint32_t op_f2i(double v) {
    if (v != v || v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t op_cvt_w_s(Ctx *c, float v) {
    switch (c->fcr31 & 3) {
    case 0: return op_f2i(rint(v));
    case 1: return op_f2i(truncf(v));
    case 2: return op_f2i(ceilf(v));
    default: return op_f2i(floorf(v));
    }
}

// Implementados por el runtime
void recomp_call(Ctx *c, uint32_t addr);            // jalr / jr a función
void recomp_bad_jump(Ctx *c, uint32_t from, uint32_t to);
void recomp_unimplemented(Ctx *c, uint32_t pc, uint32_t word);
void recomp_break(Ctx *c, uint32_t pc, uint32_t code);
void recomp_bad_sp(Ctx *c, uint32_t pc);
// setjmp/longjmp del juego: jmp_buf del host asociado a la dirección del jmp_buf MIPS
jmp_buf *recomp_jmpbuf(uint32_t mips_jmpbuf);
void recomp_longjmp(Ctx *c, uint32_t mips_jmpbuf);
void hle_call(Ctx *c, uint32_t nid);

typedef struct RecompImport {
    uint32_t nid;
    const char *name;
} RecompImport;

// Generados (build/gen/table.c, build/gen/imports.c)
extern const uint32_t g_recomp_gp;
extern const uint32_t g_recomp_import_count;
extern const RecompImport g_recomp_imports[];
extern const uint32_t g_recomp_func_addrs[];
extern const RecompFunc g_recomp_funcs[];
extern const uint32_t g_recomp_func_count;
extern const uint32_t g_recomp_entry;
extern const uint32_t g_recomp_image_base;
extern const uint32_t g_recomp_image_size;
extern const uint32_t g_recomp_bss_end;

#ifdef __cplusplus
}
#endif
