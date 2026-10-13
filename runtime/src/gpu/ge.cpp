// sceGe_user y ejecución de display lists de la GE: estado, matrices, decodificación de vértices
// y ensamblado de primitivas. El dibujo lo hace el renderizador (renderer.h).
#include "ge.h"

#include <csignal>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>

#include "../threads.h"
#include "renderer.h"

GeState g_ge;

float f24_to_float(uint32_t arg) {
    uint32_t v = arg << 8;
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// ---------------------------------------------------------------- listas
struct GeList {
    int id;
    uint32_t start, pc, stall;
    uint32_t arg;
    bool done = false;
    uint32_t stack[32];
    int sp = 0;
};

static std::map<int, GeList> g_lists;
static std::deque<int> g_queue;
static int g_next_list = 1;
static uint32_t g_cb_signal = 0, g_cb_signal_arg = 0, g_cb_finish = 0, g_cb_finish_arg = 0;
struct PendingCb {
    bool finish;
    uint32_t arg;
};
static std::deque<PendingCb> g_pending_cbs;
static uint64_t g_prims = 0, g_lists_done = 0, g_last_report = 0;

static uint32_t rel_addr(uint32_t arg) {
    return ((((g_ge.cmd[0x10] & 0x0F0000) << 8) | (arg & 0xFFFFFF)) + g_ge.offset) & 0x0FFFFFFF;
}

// ---------------------------------------------------------------- vértices
namespace {

struct Layout {
    int w_off = -1, t_off = -1, c_off = -1, n_off = -1, p_off = -1;
    int wt, tc, col, nrm, pos, wcount, morphs, stride, onesize;
};

Layout layout(uint32_t vtype) {
    static const int SIZE[4] = {0, 1, 2, 4};
    Layout l;
    l.tc = vtype & 3;
    l.col = vtype >> 2 & 7;
    l.nrm = vtype >> 5 & 3;
    l.pos = vtype >> 7 & 3;
    l.wt = vtype >> 9 & 3;
    l.wcount = (vtype >> 14 & 7) + 1;
    l.morphs = (vtype >> 18 & 7) + 1;
    int off = 0, align = 1;
    auto add = [&](int esize, int n) {
        off = (off + esize - 1) / esize * esize;
        int r = off;
        off += esize * n;
        if (esize > align) align = esize;
        return r;
    };
    if (l.wt) l.w_off = add(SIZE[l.wt], l.wcount);
    if (l.tc) l.t_off = add(SIZE[l.tc], 2);
    if (l.col) l.c_off = add(l.col == 7 ? 4 : 2, 1);
    if (l.nrm) l.n_off = add(SIZE[l.nrm], 3);
    if (l.pos) l.p_off = add(SIZE[l.pos], 3);
    l.onesize = (off + align - 1) / align * align;
    l.stride = l.onesize * l.morphs;
    return l;
}

// Componente con signo: s8/s16 normalizados (o crudos en through) o float.
inline float rd(const uint8_t *p, int fmt, bool through_pos) {
    switch (fmt) {
    case 1: return through_pos ? (float)(int8_t)*p : (int8_t)*p / 128.f;
    case 2: {
        uint16_t u;
        memcpy(&u, p, 2);
        return through_pos ? (float)(int16_t)u : (int16_t)u / 32768.f;
    }
    default: {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    }
}

void read_color(const uint8_t *p, int fmt, uint8_t out[4]) {
    uint16_t v;
    switch (fmt) {
    case 7: memcpy(out, p, 4); return;
    case 4:  // 565
        memcpy(&v, p, 2);
        out[0] = (v & 0x1F) * 255 / 31; out[1] = (v >> 5 & 0x3F) * 255 / 63; out[2] = (v >> 11) * 255 / 31; out[3] = 255;
        return;
    case 5:  // 5551
        memcpy(&v, p, 2);
        out[0] = (v & 0x1F) * 255 / 31; out[1] = (v >> 5 & 0x1F) * 255 / 31; out[2] = (v >> 10 & 0x1F) * 255 / 31;
        out[3] = v >> 15 ? 255 : 0;
        return;
    default:  // 4444
        memcpy(&v, p, 2);
        out[0] = (v & 0xF) * 17; out[1] = (v >> 4 & 0xF) * 17; out[2] = (v >> 8 & 0xF) * 17; out[3] = (v >> 12) * 17;
        return;
    }
}

// Decodifica un vértice (aplicando morph y skinning si los hay) en espacio de modelo.
void decode_vertex(const Layout &l, const uint8_t *base, bool through, GpuVertex &out) {
    float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, uv[2] = {0, 0}, col[4] = {0, 0, 0, 0};
    for (int m = 0; m < l.morphs; m++) {
        const uint8_t *v = base + m * l.onesize;
        float w = l.morphs > 1 ? g_ge.morph[m] : 1.f;
        if (l.tc) {
            const uint8_t *t = v + l.t_off;
            float tu, tv;
            if (l.tc == 1) {
                tu = t[0]; tv = t[1];
                if (!through) { tu /= 128.f; tv /= 128.f; }
            } else if (l.tc == 2) {
                uint16_t a, b;
                memcpy(&a, t, 2); memcpy(&b, t + 2, 2);
                tu = a; tv = b;
                if (!through) { tu /= 32768.f; tv /= 32768.f; }
            } else {
                memcpy(&tu, t, 4); memcpy(&tv, t + 4, 4);
            }
            uv[0] += w * tu;
            uv[1] += w * tv;
        }
        if (l.col) {
            uint8_t c[4];
            read_color(v + l.c_off, l.col, c);
            for (int k = 0; k < 4; k++) col[k] += w * c[k];
        }
        if (l.nrm) {
            int es = l.nrm == 1 ? 1 : l.nrm == 2 ? 2 : 4;
            for (int k = 0; k < 3; k++) nrm[k] = (m ? nrm[k] : 0) + w * rd(v + l.n_off + k * es, l.nrm, false);
        }
        if (l.pos) {
            int es = l.pos == 1 ? 1 : l.pos == 2 ? 2 : 4;
            for (int k = 0; k < 3; k++) {
                float p = rd(v + l.p_off + k * es, l.pos, through);
                if (through && k == 2 && l.pos == 2) p = (float)(uint16_t)(int16_t)p;  // z sin signo
                pos[k] += w * p;
            }
        }
    }
    if (l.wt && !through) {  // skinning: pos' = sum(w_i * Bone_i * pos)
        float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
        int es = l.wt == 1 ? 1 : l.wt == 2 ? 2 : 4;
        for (int i = 0; i < l.wcount; i++) {
            const uint8_t *wp = base + l.w_off + i * es;
            float w;
            if (l.wt == 1) w = *wp / 128.f;
            else if (l.wt == 2) { uint16_t u; memcpy(&u, wp, 2); w = u / 32768.f; }
            else memcpy(&w, wp, 4);
            if (w == 0.f) continue;
            const float *b = &g_ge.bone[i * 12];
            for (int k = 0; k < 3; k++) {
                sp[k] += w * (b[k] * pos[0] + b[3 + k] * pos[1] + b[6 + k] * pos[2] + b[9 + k]);
                sn[k] += w * (b[k] * nrm[0] + b[3 + k] * nrm[1] + b[6 + k] * nrm[2]);
            }
        }
        memcpy(pos, sp, sizeof(pos));
        memcpy(nrm, sn, sizeof(nrm));
    }
    memcpy(out.pos, pos, sizeof(pos));
    memcpy(out.nrm, nrm, sizeof(nrm));
    memcpy(out.uv, uv, sizeof(uv));
    if (l.col) {
        for (int k = 0; k < 4; k++) out.col[k] = (uint8_t)(col[k] > 255.f ? 255 : col[k] < 0 ? 0 : col[k]);
    } else {
        // sin color de vértice: color ambiente del material
        uint32_t a = g_ge.arg(0x55);
        out.col[0] = a & 0xFF; out.col[1] = a >> 8 & 0xFF; out.col[2] = a >> 16 & 0xFF; out.col[3] = g_ge.arg(0x58) & 0xFF;
    }
}

std::vector<GpuVertex> g_tmp, g_out;

void draw_prim(uint32_t arg) {
    int prim = arg >> 16 & 7, count = arg & 0xFFFF;
    if (!count) return;
    uint32_t vtype = g_ge.cmd[0x12] & 0xFFFFFF;
    bool through = vtype >> 23 & 1;
    Layout l = layout(vtype);
    int idx_fmt = vtype >> 11 & 3;

    // índices -> vértices
    g_tmp.resize(count);
    std::map<int, int> cache;
    for (int i = 0; i < count; i++) {
        int index = i;
        if (idx_fmt == 1) index = R8(g_ge.iaddr + i);
        else if (idx_fmt == 2) index = R16(g_ge.iaddr + i * 2);
        else if (idx_fmt == 3) index = (int)R32(g_ge.iaddr + i * 4);
        decode_vertex(l, MEMP(g_ge.vaddr + index * l.stride), through, g_tmp[i]);
    }
    if (idx_fmt) g_ge.iaddr += count * (idx_fmt == 1 ? 1 : idx_fmt == 2 ? 2 : 4);
    else g_ge.vaddr += count * l.stride;

    // primitiva -> lista
    g_out.clear();
    GpuPrim gp = GpuPrim::Triangles;
    switch (prim) {
    case 0: gp = GpuPrim::Points; g_out = g_tmp; break;
    case 1: gp = GpuPrim::Lines; g_out = g_tmp; break;
    case 2:
        gp = GpuPrim::Lines;
        for (int i = 0; i + 1 < count; i++) { g_out.push_back(g_tmp[i]); g_out.push_back(g_tmp[i + 1]); }
        break;
    case 3: g_out.assign(g_tmp.begin(), g_tmp.begin() + count / 3 * 3); break;
    case 4:
        for (int i = 0; i + 2 < count; i++) {
            if (i & 1) { g_out.push_back(g_tmp[i + 1]); g_out.push_back(g_tmp[i]); }
            else { g_out.push_back(g_tmp[i]); g_out.push_back(g_tmp[i + 1]); }
            g_out.push_back(g_tmp[i + 2]);
        }
        break;
    case 5:
        for (int i = 1; i + 1 < count; i++) { g_out.push_back(g_tmp[0]); g_out.push_back(g_tmp[i]); g_out.push_back(g_tmp[i + 1]); }
        break;
    case 6:  // rectángulos: pares (esquina superior izquierda, inferior derecha); color y z del segundo
        if (!through) {
            static bool warned = false;
            if (!warned) { LOGW("ge", "sprites transformados sin implementar"); warned = true; }
            return;
        }
        for (int i = 0; i + 1 < count; i += 2) {
            GpuVertex a = g_tmp[i], b = g_tmp[i + 1];
            GpuVertex q[4] = {b, b, b, b};
            q[0].pos[0] = a.pos[0]; q[0].pos[1] = a.pos[1]; q[0].uv[0] = a.uv[0]; q[0].uv[1] = a.uv[1];
            q[1].pos[1] = a.pos[1]; q[1].uv[1] = a.uv[1];
            q[2].pos[0] = a.pos[0]; q[2].uv[0] = a.uv[0];
            g_out.push_back(q[0]); g_out.push_back(q[1]); g_out.push_back(q[2]);
            g_out.push_back(q[1]); g_out.push_back(q[3]); g_out.push_back(q[2]);
        }
        break;
    default: return;  // bezier/spline: no se usan en Cars
    }
    g_prims++;
    if (!g_out.empty()) renderer_draw(gp, g_out.data(), (int)g_out.size());
}

void block_transfer() {
    uint32_t src = (g_ge.arg(0xB2) & 0xFFFFF0) | ((g_ge.cmd[0xB3] & 0xFF0000) << 8);
    uint32_t dst = (g_ge.arg(0xB4) & 0xFFFFF0) | ((g_ge.cmd[0xB5] & 0xFF0000) << 8);
    uint32_t sw = g_ge.cmd[0xB3] & 0x7FF, dw = g_ge.cmd[0xB5] & 0x7FF;
    uint32_t sx = g_ge.arg(0xEB) & 0x3FF, sy = g_ge.arg(0xEB) >> 10 & 0x3FF;
    uint32_t dx = g_ge.arg(0xEC) & 0x3FF, dy = g_ge.arg(0xEC) >> 10 & 0x3FF;
    uint32_t w = (g_ge.arg(0xEE) & 0x3FF) + 1, h = (g_ge.arg(0xEE) >> 10 & 0x3FF) + 1;
    uint32_t bpp = (g_ge.arg(0xEA) & 1) ? 4 : 2;
    for (uint32_t y = 0; y < h; y++)
        memmove(MEMP(dst + ((dy + y) * dw + dx) * bpp), MEMP(src + ((sy + y) * sw + sx) * bpp), w * bpp);
    renderer_invalidate(dst + dy * dw * bpp, h * dw * bpp);
}

}  // namespace

// ---------------------------------------------------------------- ejecución
static void ge_execute(GeList &l) {
    for (int guard = 0; guard < 1 << 24; guard++) {
        if (l.stall && (l.pc & 0x0FFFFFFF) == (l.stall & 0x0FFFFFFF)) return;
        uint32_t op = R32(l.pc);
        uint32_t cmd = op >> 24, arg = op & 0xFFFFFF;
        uint32_t cur = l.pc;
        l.pc += 4;
        switch (cmd) {
        case 0x01: g_ge.vaddr = rel_addr(arg); break;
        case 0x02: g_ge.iaddr = rel_addr(arg); break;
        case 0x04: draw_prim(arg); break;
        case 0x08: l.pc = rel_addr(arg) & ~3u; break;  // JUMP
        case 0x09: break;  // BJUMP: salta si la caja envolvente es invisible; se asume visible
        case 0x0A:
            l.stack[l.sp++ & 31] = l.pc;
            l.pc = rel_addr(arg) & ~3u;
            break;
        case 0x0B: l.pc = l.stack[--l.sp & 31]; break;
        case 0x0C: {
            // END: cierra el par que forma con el comando anterior. FINISH+END termina la lista;
            // SIGNAL+END es una señal (callback o salto) y la lista continúa.
            uint32_t prev = R32(cur - 4);
            if (prev >> 24 == 0x0E) {
                uint32_t behaviour = prev >> 16 & 0xFF, data = prev & 0xFFFF;
                uint32_t target = ((data << 16) | (arg & 0xFFFF)) & 0x0FFFFFFC;
                switch (behaviour) {
                case 0x10: l.pc = target; break;                                 // JUMP
                case 0x11: l.stack[l.sp++ & 31] = l.pc; l.pc = target; break;    // CALL
                case 0x12: l.pc = l.stack[--l.sp & 31]; break;                   // RET
                case 0x13: l.pc = (cur - 4 + target) & 0x0FFFFFFC; break;         // RJUMP
                case 0xF0: case 0xFF: break;                                      // SYNC / sin efecto
                default: g_pending_cbs.push_back({false, data}); break;           // SUSPEND/CONTINUE/PAUSE
                }
                break;
            }
            if (prev >> 24 == 0x0F) g_pending_cbs.push_back({true, prev & 0xFFFF});
            l.done = true;
            g_lists_done++;
            LOGT("ge", "lista %d terminada en %08x", l.id, cur);
            return;
        }
        case 0x0E: case 0x0F: break;  // SIGNAL/FINISH: se procesan con el END que les sigue
        case 0x13: g_ge.offset = arg << 8; break;
        case 0x14: g_ge.offset = cur; break;  // ORIGIN
        case 0x2A: g_ge.bone_i = arg & 0x7F; break;
        case 0x2B: if (g_ge.bone_i < 96) g_ge.bone[g_ge.bone_i++] = f24_to_float(arg); break;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33:
            g_ge.morph[cmd - 0x2C] = f24_to_float(arg);
            break;
        case 0x3A: g_ge.world_i = arg & 0xF; break;
        case 0x3B: if (g_ge.world_i < 12) g_ge.world[g_ge.world_i++] = f24_to_float(arg); break;
        case 0x3C: g_ge.view_i = arg & 0xF; break;
        case 0x3D: if (g_ge.view_i < 12) g_ge.view[g_ge.view_i++] = f24_to_float(arg); break;
        case 0x3E: g_ge.proj_i = arg & 0xF; break;
        case 0x3F: if (g_ge.proj_i < 16) g_ge.proj[g_ge.proj_i++] = f24_to_float(arg); break;
        case 0x40: g_ge.tgen_i = arg & 0xF; break;
        case 0x41: if (g_ge.tgen_i < 12) g_ge.tgen[g_ge.tgen_i++] = f24_to_float(arg); break;
        case 0xC4: {  // LOAD CLUT
            uint32_t addr = (g_ge.arg(0xB0) & 0xFFFFF0) | ((g_ge.cmd[0xB1] & 0x0F0000) << 8);
            uint32_t bytes = (arg & 0x3F) * 32;
            if (bytes > sizeof(g_ge.clut)) bytes = sizeof(g_ge.clut);
            if (addr) memcpy(g_ge.clut, MEMP(addr), bytes);
            break;
        }
        case 0xEA: g_ge.cmd[0xEA] = op; block_transfer(); break;
        default: break;
        }
        if (cmd != 0x2B && cmd != 0x3B && cmd != 0x3D && cmd != 0x3F && cmd != 0x41) g_ge.cmd[cmd] = op;
    }
    LOGW("ge", "lista %d: demasiados comandos sin END (pc=%08x)", l.id, l.pc);
    l.done = true;
}

static void ge_run_queue() {
    while (!g_queue.empty()) {
        GeList &l = g_lists[g_queue.front()];
        ge_execute(l);
        if (!l.done) return;  // bloqueada en stall
        g_queue.pop_front();
    }
}

// Los callbacks de la GE se ejecutan en contexto de interrupción, desde el planificador.
static void ge_hook(uint64_t now) {
    while (!g_pending_cbs.empty()) {
        PendingCb cb = g_pending_cbs.front();
        g_pending_cbs.pop_front();
        uint32_t fn = cb.finish ? g_cb_finish : g_cb_signal;
        uint32_t arg = cb.finish ? g_cb_finish_arg : g_cb_signal_arg;
        if (fn) intr_call(fn, {cb.arg, arg, 0});
    }
    if (now - g_last_report > 1000000) {
        LOGD("ge", "listas completadas: %llu, primitivas: %llu", (unsigned long long)g_lists_done,
             (unsigned long long)g_prims);
        g_last_report = now;
        if (g_log_level >= LOG_DEBUG) threads_dump();
    }
}

static void enqueue(Ctx *c, bool head) {
    static bool hooked = false;
    if (!hooked) {
        sched_add_hook(ge_hook);
        hooked = true;
    }
    GeList l{};
    l.id = g_next_list++;
    l.start = l.pc = A0 & 0x0FFFFFFF;
    l.stall = A1 & 0x0FFFFFFF;
    l.arg = A3 ? R32(A3 + 4) : 0;
    g_lists[l.id] = l;
    LOGT("ge", "enqueue %d %08x stall=%08x", l.id, l.start, l.stall);
    if (head) g_queue.push_front(l.id);
    else g_queue.push_back(l.id);
    // limpia listas antiguas terminadas
    while (g_lists.size() > 64 && g_lists.begin()->second.done) g_lists.erase(g_lists.begin());
    ge_run_queue();
    c->r[2] = l.id;
}

HLE(sceGeListEnQueue) { enqueue(c, false); }
HLE(sceGeListEnQueueHead) { enqueue(c, true); }
HLE(sceGeListUpdateStallAddr) {
    auto it = g_lists.find((int)A0);
    if (it == g_lists.end()) RET(0x80000100);
    it->second.stall = A1 & 0x0FFFFFFF;
    ge_run_queue();
    LOGT("ge", "stall lista %d -> %08x (pc %08x, %s)", (int)A0, it->second.stall, it->second.pc,
         it->second.done ? "terminada" : "pendiente");
    RET(0);
}
HLE(sceGeListSync) {
    auto it = g_lists.find((int)A0);
    if (it == g_lists.end()) RET(0);
    RET(it->second.done ? 0 : 2);
}
HLE(sceGeDrawSync) {
    LOGT("ge", "DrawSync(%u): cola %zu", A0, g_queue.size());
    RET(g_queue.empty() ? 0 : 2);
}
HLE(sceGeBreak) { RET(0); }
HLE(sceGeContinue) { RET(0); }
HLE(sceGeEdramGetAddr) { RET(0x04000000); }
HLE(sceGeEdramGetSize) { RET(0x00200000); }
HLE(sceGeSetCallback) {
    g_cb_signal = R32(A0);
    g_cb_signal_arg = R32(A0 + 4);
    g_cb_finish = R32(A0 + 8);
    g_cb_finish_arg = R32(A0 + 12);
    LOGD("ge", "callbacks signal=%08x finish=%08x", g_cb_signal, g_cb_finish);
    RET(1);
}
HLE(sceGeUnsetCallback) { RET(0); }
