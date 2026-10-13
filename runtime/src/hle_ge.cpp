// sceGe_user: colas de display lists de la GE.
// F0: intérprete de control de flujo sin rasterizar (cuenta primitivas y gestiona stall/callbacks).
// El renderizador real se engancha en ge_execute() (comandos PRIM/estado).
#include <deque>
#include <map>

#include "threads.h"

struct GeList {
    int id;
    uint32_t start, pc, stall;
    int cbid;
    uint32_t arg;
    bool done = false;
    uint32_t stack[32];
    int sp = 0;
    uint32_t base = 0, offset = 0;
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
static uint32_t g_regs[256];

static void ge_execute(GeList &l) {
    for (int guard = 0; guard < 1 << 22; guard++) {
        if (l.stall && (l.pc & 0x0FFFFFFF) == (l.stall & 0x0FFFFFFF)) return;  // espera a más comandos
        uint32_t op = R32(l.pc);
        uint32_t cmd = op >> 24, arg = op & 0xFFFFFF;
        l.pc += 4;
        g_regs[cmd] = op;
        switch (cmd) {
        case 0x04: g_prims++; break;  // PRIM
        case 0x08: l.pc = ((l.base | arg) + l.offset) & 0x0FFFFFFC; break;  // JUMP
        case 0x0A:  // CALL
            l.stack[l.sp++ & 31] = l.pc;
            l.pc = ((l.base | arg) + l.offset) & 0x0FFFFFFC;
            break;
        case 0x0B: l.pc = l.stack[--l.sp & 31]; break;  // RET
        case 0x10: l.base = (arg << 8) & 0x0F000000; break;  // BASE
        case 0x13: l.offset = arg << 8; break;  // OFFSET ADDR
        case 0x0E: {  // SIGNAL
            uint32_t behaviour = arg >> 16 & 0xFF;
            g_pending_cbs.push_back({false, arg & 0xFFFF});
            if (behaviour == 0x10) {  // salto (signal de tipo jump): el siguiente END lo resuelve
            }
            break;
        }
        case 0x0F:  // FINISH
            g_pending_cbs.push_back({true, l.arg});
            break;
        case 0x0C:  // END
            l.done = true;
            g_lists_done++;
            return;
        default: break;
        }
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
        LOGI("ge", "listas completadas: %llu, primitivas: %llu", (unsigned long long)g_lists_done,
             (unsigned long long)g_prims);
        g_last_report = now;
        if (g_log_level >= LOG_DEBUG) threads_dump();
    }
}

static void enqueue(Ctx *c, bool head) {
    static bool hooked = false;
    if (!hooked) { sched_add_hook(ge_hook); hooked = true; }
    GeList l{};
    l.id = g_next_list++;
    l.start = l.pc = A0 & 0x0FFFFFFF;
    l.stall = A1 & 0x0FFFFFFF;
    l.cbid = (int)A2;
    l.arg = A3 ? R32(A3 + 4) : 0;
    g_lists[l.id] = l;
    if (head) g_queue.push_front(l.id);
    else g_queue.push_back(l.id);
    LOGT("ge", "enqueue %d %08x stall=%08x", l.id, l.start, l.stall);
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
    RET(0);
}
HLE(sceGeListSync) {
    auto it = g_lists.find((int)A0);
    if (it == g_lists.end()) RET(0x80000100);
    RET(it->second.done ? 0 : 2);
}
HLE(sceGeDrawSync) { RET(g_queue.empty() ? 0 : 2); }
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
