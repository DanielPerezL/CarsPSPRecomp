// Intérprete de la VFPU (unidad vectorial de la PSP), una función por operación (las llama el código generado).
// Semántica según el intérprete de PPSSPP (Core/MIPS/InterpreterVFPU.cpp, GPL-2.0+): mapeo de registros,
// prefijos S/T (swizzle, abs, constantes, negación) y D (saturación y máscara de escritura).
// Registros: c->v[i] con la numeración canónica i = fila*32 + matriz*4 + columna.
#include <cmath>

#include "core.h"

namespace {

constexpr int CTRL_SPREFIX = 0, CTRL_TPREFIX = 1, CTRL_DPREFIX = 2, CTRL_CC = 3;

inline int VD(uint32_t op) { return op & 0x7F; }
inline int VS(uint32_t op) { return op >> 8 & 0x7F; }
inline int VT(uint32_t op) { return op >> 16 & 0x7F; }
inline int vec_size(uint32_t op) { return (op >> 7 & 1) + (op >> 14 & 2) + 1; }  // 1..4

void vector_regs(uint8_t regs[4], int n, int reg) {
    int mtx = reg >> 2 & 7, col = reg & 3, row = 0, transpose = reg >> 5 & 1;
    switch (n) {
    case 1: transpose = 0; row = reg >> 5 & 3; break;
    case 2: row = reg >> 5 & 2; break;
    case 3: row = reg >> 6 & 1; break;
    default: row = reg >> 5 & 2; break;
    }
    for (int i = 0; i < n; i++)
        regs[i] = mtx * 4 + (transpose ? ((row + i) & 3) + col * 32 : col + ((row + i) & 3) * 32);
}

void matrix_regs(uint8_t regs[16], int n, int reg) {
    int mtx = reg >> 2 & 7, col = reg & 3, row = 0, transpose = reg >> 5 & 1;
    switch (n) {
    case 1: transpose = 0; row = reg >> 5 & 3; break;
    case 2: row = reg >> 5 & 2; break;
    case 3: row = reg >> 6 & 1; break;
    default: row = reg >> 5 & 2; break;
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            regs[j * 4 + i] = mtx * 4 + (transpose ? ((row + i) & 3) + ((col + j) & 3) * 32
                                                   : ((col + j) & 3) + ((row + i) & 3) * 32);
}

void read_vec(Ctx *c, float *d, int n, int reg) {
    uint8_t r[4];
    vector_regs(r, n, reg);
    for (int i = 0; i < n; i++) d[i] = c->v[r[i]];
}

void write_vec(Ctx *c, const float *s, int n, int reg) {
    uint8_t r[4];
    vector_regs(r, n, reg);
    uint32_t mask = c->vfpu_ctrl[CTRL_DPREFIX] >> 8 & 0xF;
    for (int i = 0; i < n; i++)
        if (!(mask >> i & 1)) c->v[r[i]] = s[i];
}

void read_mtx(Ctx *c, float *d, int n, int reg) {
    uint8_t r[16];
    matrix_regs(r, n, reg);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) d[j * 4 + i] = c->v[r[j * 4 + i]];
}

void write_mtx(Ctx *c, const float *s, int n, int reg) {
    uint8_t r[16];
    matrix_regs(r, n, reg);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) c->v[r[j * 4 + i]] = s[j * 4 + i];
}

void apply_st(float *r, uint32_t data, int n) {
    if (data == 0xE4) return;
    static const float K[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};
    float orig[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) orig[i] = r[i];
    for (int i = 0; i < n; i++) {
        int regnum = data >> (i * 2) & 3, abs = data >> (8 + i) & 1;
        int neg = data >> (16 + i) & 1, cst = data >> (12 + i) & 1;
        if (!cst) {
            r[i] = orig[regnum];
            if (abs) r[i] = fabsf(r[i]);
        } else {
            r[i] = K[regnum + (abs << 2)];
        }
        if (neg) r[i] = -r[i];
    }
}

void apply_d(Ctx *c, float *v, int n) {
    uint32_t data = c->vfpu_ctrl[CTRL_DPREFIX];
    if (!data) return;
    for (int i = 0; i < n; i++) {
        int sat = data >> (i * 2) & 3;
        if (sat == 1) v[i] = v[i] != v[i] ? v[i] : fminf(fmaxf(v[i], 0.f), 1.f);
        else if (sat == 3) v[i] = v[i] != v[i] ? v[i] : fminf(fmaxf(v[i], -1.f), 1.f);
    }
}

inline void eat_prefixes(Ctx *c) {
    c->vfpu_ctrl[CTRL_SPREFIX] = 0xE4;
    c->vfpu_ctrl[CTRL_TPREFIX] = 0xE4;
    c->vfpu_ctrl[CTRL_DPREFIX] = 0;
}

inline void S(Ctx *c, float *s, int n) { apply_st(s, c->vfpu_ctrl[CTRL_SPREFIX], n); }
inline void T(Ctx *c, float *t, int n) { apply_st(t, c->vfpu_ctrl[CTRL_TPREFIX], n); }

inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// Operación binaria por componente: d = s op t
template <typename F>
void binop(Ctx *c, uint32_t op, F f) {
    int n = vec_size(op);
    float s[4], t[4], d[4];
    read_vec(c, s, n, VS(op));
    read_vec(c, t, n, VT(op));
    S(c, s, n);
    T(c, t, n);
    for (int i = 0; i < n; i++) d[i] = f(s[i], t[i]);
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

// Operación unaria por componente: d = f(s)
template <typename F>
void unop(Ctx *c, uint32_t op, F f) {
    int n = vec_size(op);
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    S(c, s, n);
    for (int i = 0; i < n; i++) d[i] = f(s[i]);
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

void fill(Ctx *c, uint32_t op, float v) {
    int n = vec_size(op);
    float d[4] = {v, v, v, v};
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

inline uint32_t mem_addr(Ctx *c, uint32_t op) { return c->r[op >> 21 & 31] + (int32_t)(int16_t)(op & 0xFFFC); }

}  // namespace

extern "C" {

// ---------------------------------------------------------------- carga/almacenamiento y movimientos
void vfpu_lv_s(Ctx *c, uint32_t op) { c->vi[(op >> 16 & 0x1F) | (op & 3) << 5] = R32(mem_addr(c, op)); }
void vfpu_sv_s(Ctx *c, uint32_t op) { W32(mem_addr(c, op), c->vi[(op >> 16 & 0x1F) | (op & 3) << 5]); }
void vfpu_lv_q(Ctx *c, uint32_t op) {
    uint32_t a = mem_addr(c, op);
    float d[4];
    memcpy(d, MEMP(a), 16);
    uint32_t saved = c->vfpu_ctrl[CTRL_DPREFIX];
    c->vfpu_ctrl[CTRL_DPREFIX] = 0;  // las cargas no usan la máscara de escritura
    write_vec(c, d, 4, (op >> 16 & 0x1F) | (op & 1) << 5);
    c->vfpu_ctrl[CTRL_DPREFIX] = saved;
}
void vfpu_sv_q(Ctx *c, uint32_t op) {
    float d[4];
    read_vec(c, d, 4, (op >> 16 & 0x1F) | (op & 1) << 5);
    memcpy(MEMP(mem_addr(c, op)), d, 16);
}
void vfpu_mfv(Ctx *c, uint32_t op) {
    int rt = op >> 16 & 31, imm = op & 0xFF;
    if (!rt) return;
    if (imm < 128) c->r[rt] = c->vi[imm];
    else if (imm < 144) c->r[rt] = c->vfpu_ctrl[imm - 128];
}
void vfpu_mtv(Ctx *c, uint32_t op) {
    int rt = op >> 16 & 31, imm = op & 0xFF;
    if (imm < 128) c->vi[imm] = c->r[rt];
    else if (imm < 144) c->vfpu_ctrl[imm - 128] = c->r[rt];
}
void vfpu_viim(Ctx *c, uint32_t op) {
    float f = (float)(int16_t)(op & 0xFFFF);
    apply_d(c, &f, 1);
    write_vec(c, &f, 1, VT(op));
    eat_prefixes(c);
}

// ---------------------------------------------------------------- prefijos y control
void vfpu_vpfxs(Ctx *c, uint32_t op) { c->vfpu_ctrl[CTRL_SPREFIX] = op & 0xFFFFF; }
void vfpu_vpfxt(Ctx *c, uint32_t op) { c->vfpu_ctrl[CTRL_TPREFIX] = op & 0xFFFFF; }
void vfpu_vpfxd(Ctx *c, uint32_t op) { c->vfpu_ctrl[CTRL_DPREFIX] = op & 0xFFF; }
void vfpu_vflush(Ctx *, uint32_t) {}
void vfpu_vnop(Ctx *c, uint32_t) { eat_prefixes(c); }

// ---------------------------------------------------------------- inicialización
void vfpu_vzero(Ctx *c, uint32_t op) { fill(c, op, 0.f); }
void vfpu_vone(Ctx *c, uint32_t op) { fill(c, op, 1.f); }
void vfpu_vidt(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    int offmask = (n == 4 || n == 3) ? 3 : 1;
    float d[4];
    for (int i = 0; i < n; i++) d[i] = (VD(op) & offmask) == (i & offmask) ? 1.f : 0.f;
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}
void vfpu_vmzero(Ctx *c, uint32_t op) {
    float m[16] = {};
    write_mtx(c, m, vec_size(op), VD(op));
    eat_prefixes(c);
}

// ---------------------------------------------------------------- aritmética por componente
void vfpu_vadd(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return a + b; }); }
void vfpu_vsub(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return a - b; }); }
void vfpu_vmul(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return a * b; }); }
void vfpu_vdiv(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return a / b; }); }
void vfpu_vmin(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return fminf(a, b); }); }
void vfpu_vmax(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return fmaxf(a, b); }); }
void vfpu_vslt(Ctx *c, uint32_t op) { binop(c, op, [](float a, float b) { return a < b ? 1.f : 0.f; }); }
void vfpu_vmov(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return a; }); }
void vfpu_vabs(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return fabsf(a); }); }
void vfpu_vneg(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return -a; }); }
void vfpu_vsat0(Ctx *c, uint32_t op) {
    unop(c, op, [](float a) { return a <= 0.f ? 0.f : a > 1.f ? 1.f : a; });
}
void vfpu_vrcp(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return 1.f / a; }); }
void vfpu_vrsq(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return 1.f / sqrtf(a); }); }
void vfpu_vsqrt(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return sqrtf(fabsf(a)); }); }
// vsin/vcos trabajan en cuartos de vuelta: vsin(x) = sin(x * pi/2)
void vfpu_vsin(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return (float)sin(fmod(a, 4.0) * M_PI_2); }); }
void vfpu_vcos(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return (float)cos(fmod(a, 4.0) * M_PI_2); }); }
void vfpu_vexp2(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return exp2f(a); }); }
void vfpu_vlog2(Ctx *c, uint32_t op) { unop(c, op, [](float a) { return log2f(a); }); }

// ---------------------------------------------------------------- reducciones y productos
void vfpu_vdot(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[4], t[4], d = 0.f;
    read_vec(c, s, n, VS(op));
    read_vec(c, t, n, VT(op));
    S(c, s, n);
    T(c, t, n);
    for (int i = 0; i < n; i++) d += s[i] * t[i];
    apply_d(c, &d, 1);
    write_vec(c, &d, 1, VD(op));
    eat_prefixes(c);
}
void vfpu_vfad(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[4], d = 0.f;
    read_vec(c, s, n, VS(op));
    S(c, s, n);
    for (int i = 0; i < n; i++) d += s[i];
    apply_d(c, &d, 1);
    write_vec(c, &d, 1, VD(op));
    eat_prefixes(c);
}
void vfpu_vscl(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[4], t, d[4];
    read_vec(c, s, n, VS(op));
    read_vec(c, &t, 1, VT(op));
    S(c, s, n);
    for (int i = 0; i < n; i++) d[i] = s[i] * t;
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}
void vfpu_vcrsp(Ctx *c, uint32_t op) {
    float s[4], t[4], d[4];
    read_vec(c, s, 3, VS(op));
    read_vec(c, t, 3, VT(op));
    d[0] = s[1] * t[2] - s[2] * t[1];
    d[1] = s[2] * t[0] - s[0] * t[2];
    d[2] = s[0] * t[1] - s[1] * t[0];
    apply_d(c, d, 3);
    write_vec(c, d, 3, VD(op));
    eat_prefixes(c);
}

// ---------------------------------------------------------------- matrices
void vfpu_vmmul(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[16] = {}, t[16] = {}, d[16];
    read_mtx(c, s, n, VS(op));
    read_mtx(c, t, n, VT(op));
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++) {
            float sum = 0.f;
            for (int k = 0; k < n; k++) sum += s[b * 4 + k] * t[a * 4 + k];
            d[a * 4 + b] = sum;
        }
    write_mtx(c, d, n, VD(op));
    eat_prefixes(c);
}
void vfpu_vmscl(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[16] = {}, t, d[16];
    read_mtx(c, s, n, VS(op));
    read_vec(c, &t, 1, VT(op));
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++) d[a * 4 + b] = s[a * 4 + b] * t;
    write_mtx(c, d, n, VD(op));
    eat_prefixes(c);
}
void vfpu_vtfm3(Ctx *c, uint32_t op) {
    int ins = op >> 23 & 3;  // 2 para vtfm3
    int n = vec_size(op);
    int tn = n < ins + 1 ? n : ins + 1;
    float s[16] = {}, t[4] = {}, d[4];
    read_mtx(c, s, ins + 1, VS(op));
    read_vec(c, t, ins + 1, VT(op));
    for (int i = 0; i <= ins; i++) {
        d[i] = 0.f;
        for (int k = 0; k < tn; k++) d[i] += s[i * 4 + k] * t[k];
        if (ins >= n) d[i] += s[i * 4 + ins];
    }
    apply_d(c, d, ins + 1);
    write_vec(c, d, ins + 1, VD(op));
    eat_prefixes(c);
}

// ---------------------------------------------------------------- conversiones
static void f2i(Ctx *c, uint32_t op, int mode) {
    int n = vec_size(op);
    float s[4];
    int32_t d[4];
    double mult = (double)(1u << (op >> 16 & 0x1F));
    read_vec(c, s, n, VS(op));
    S(c, s, n);
    for (int i = 0; i < n; i++) {
        double v = s[i] * mult;
        if (s[i] != s[i] || v > 2147483647.0) d[i] = 0x7FFFFFFF;
        else if (v <= -2147483648.0) d[i] = (int32_t)0x80000000;
        else if (mode == 0) d[i] = (int32_t)nearbyint(v);
        else if (mode == 1) d[i] = (int32_t)trunc(v);
        else if (mode == 2) d[i] = (int32_t)ceil(v);
        else d[i] = (int32_t)floor(v);
    }
    write_vec(c, reinterpret_cast<float *>(d), n, VD(op));
    eat_prefixes(c);
}
void vfpu_vf2iz(Ctx *c, uint32_t op) { f2i(c, op, 1); }
void vfpu_vf2id(Ctx *c, uint32_t op) { f2i(c, op, 3); }
void vfpu_vi2f(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[4], d[4];
    float mult = 1.f / (float)(1u << (op >> 16 & 0x1F));
    read_vec(c, s, n, VS(op));
    S(c, s, n);
    for (int i = 0; i < n; i++) d[i] = (float)(int32_t)fbits(s[i]) * mult;
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}
void vfpu_vi2uc(Ctx *c, uint32_t op) {
    float s[4] = {};
    read_vec(c, s, vec_size(op), VS(op));
    S(c, s, 4);
    uint32_t d = 0;
    for (int i = 0; i < 4; i++) {
        int32_t v = (int32_t)fbits(s[i]);
        if (v < 0) v = 0;
        d |= ((uint32_t)(v >> 23) & 0xFF) << (i * 8);
    }
    float f = bitsf(d);
    write_vec(c, &f, 1, VD(op));
    eat_prefixes(c);
}
void vfpu_vi2c(Ctx *c, uint32_t op) {
    float s[4] = {};
    read_vec(c, s, vec_size(op), VS(op));
    S(c, s, 4);
    uint32_t d = 0;
    for (int i = 0; i < 4; i++) d |= (fbits(s[i]) >> 24) << (i * 8);
    float f = bitsf(d);
    write_vec(c, &f, 1, VD(op));
    eat_prefixes(c);
}
void vfpu_vs2i(Ctx *c, uint32_t op) {
    int n = vec_size(op);
    float s[4] = {};
    uint32_t d[4] = {};
    read_vec(c, s, n, VS(op));
    S(c, s, n);
    int in = n >= 2 ? 2 : 1, out = n >= 2 ? 4 : 2;
    for (int i = 0; i < in; i++) {
        uint32_t v = fbits(s[i]);
        d[i * 2] = (v & 0xFFFF) << 16;
        d[i * 2 + 1] = v & 0xFFFF0000;
    }
    write_vec(c, reinterpret_cast<float *>(d), out, VD(op));
    eat_prefixes(c);
}

// ---------------------------------------------------------------- comparación y movimiento condicional
void vfpu_vcmp(Ctx *c, uint32_t op) {
    int n = vec_size(op), cond = op & 0xF;
    float s[4], t[4];
    read_vec(c, s, n, VS(op));
    read_vec(c, t, n, VT(op));
    S(c, s, n);
    T(c, t, n);
    int cc = 0, any = 0, all = 1, affected = 0x30;
    for (int i = 0; i < n; i++) {
        bool r;
        switch (cond) {
        case 0: r = false; break;
        case 1: r = s[i] == t[i]; break;
        case 2: r = s[i] < t[i]; break;
        case 3: r = s[i] <= t[i]; break;
        case 4: r = true; break;
        case 5: r = s[i] != t[i]; break;
        case 6: r = s[i] >= t[i]; break;
        case 7: r = s[i] > t[i]; break;
        case 8: r = s[i] == 0.f; break;
        case 9: r = std::isnan(s[i]); break;
        case 10: r = std::isinf(s[i]); break;
        case 11: r = !std::isfinite(s[i]); break;
        case 12: r = s[i] != 0.f; break;
        case 13: r = !std::isnan(s[i]); break;
        case 14: r = !std::isinf(s[i]); break;
        default: r = std::isfinite(s[i]); break;
        }
        cc |= r << i;
        any |= r;
        all &= r;
        affected |= 1 << i;
    }
    c->vfpu_ctrl[CTRL_CC] = (c->vfpu_ctrl[CTRL_CC] & ~affected) | ((cc | any << 4 | all << 5) & affected);
    eat_prefixes(c);
}
void vfpu_vcmovt(Ctx *c, uint32_t op) {
    int n = vec_size(op), tf = op >> 19 & 1, imm3 = op >> 16 & 7;
    float s[4], d[4];
    read_vec(c, s, n, VS(op));
    read_vec(c, d, n, VD(op));
    S(c, s, n);
    uint32_t cc = c->vfpu_ctrl[CTRL_CC];
    if (imm3 < 6) {
        if ((int)(cc >> imm3 & 1) == !tf)
            for (int i = 0; i < n; i++) d[i] = s[i];
    } else if (imm3 == 6) {
        for (int i = 0; i < n; i++)
            if ((int)(cc >> i & 1) == !tf) d[i] = s[i];
    }
    apply_d(c, d, n);
    write_vec(c, d, n, VD(op));
    eat_prefixes(c);
}

}  // extern "C"
