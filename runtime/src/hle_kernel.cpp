// ThreadManForUser: hilos, semáforos, flags de evento, retardos y callbacks.
#include <map>

#include "threads.h"

int next_uid();

// ---------------------------------------------------------------- hilos
HLE(sceKernelCreateThread) {
    int id = thread_create(mem_str(A0), A1, (int)A2, A3, T0);
    RET(id < 0 ? 0x800200D9 : id);
}

HLE(sceKernelStartThread) {
    Thread *t = thread_get((int)A0);
    if (!t) RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
    if (t->state != ThreadState::Dormant) RET(SCE_KERNEL_ERROR_NOT_DORMANT);
    thread_start(t, A1, A2);
    c->r[2] = 0;
    thread_reschedule();
}

HLE(sceKernelExitThread) { thread_exit_current(A0, false); }
HLE(sceKernelExitDeleteThread) { thread_exit_current(A0, true); }

HLE(sceKernelDeleteThread) {
    Thread *t = thread_get((int)A0);
    if (!t) RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
    if (t->state != ThreadState::Dormant) RET(SCE_KERNEL_ERROR_NOT_DORMANT);
    if (t == thread_current()) RET(SCE_KERNEL_ERROR_NOT_DORMANT);
    t->state = ThreadState::Dead;
    RET(0);
}

HLE(sceKernelGetThreadId) { RET(thread_current()->id); }

// Esperar al hilo 0 o a uno mismo es ilegal: la PSP devuelve el error al instante (el juego lo hace).
constexpr uint32_t SCE_KERNEL_ERROR_ILLEGAL_THID = 0x80020197;

HLE(sceKernelWaitThreadEnd) {
    if (A0 == 0 || (int)A0 == thread_current()->id) RET(SCE_KERNEL_ERROR_ILLEGAL_THID);
    Thread *t = thread_get((int)A0);
    if (!t) RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
    if (t->state == ThreadState::Dormant || t->state == ThreadState::Dead) RET(t->exit_status);
    uint32_t tmo = A1 ? R32(A1) : 0;
    RET(thread_wait(WaitType::ThreadEnd, t->id, tmo, A1, false));
}
HLE(sceKernelWaitThreadEndCB) {
    if (A0 == 0 || (int)A0 == thread_current()->id) RET(SCE_KERNEL_ERROR_ILLEGAL_THID);
    Thread *t = thread_get((int)A0);
    if (!t) RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
    if (t->state == ThreadState::Dormant || t->state == ThreadState::Dead) RET(t->exit_status);
    uint32_t tmo = A1 ? R32(A1) : 0;
    RET(thread_wait(WaitType::ThreadEnd, t->id, tmo, A1, true));
}

static void delay(Ctx *c, uint32_t us, bool cb) {
    c->r[2] = thread_wait(WaitType::Delay, 0, us ? us : 1, 0, cb);
}
HLE(sceKernelDelayThread) { delay(c, A0, false); }
HLE(sceKernelDelayThreadCB) { delay(c, A0, true); }

HLE(sceKernelSleepThread) {
    Thread *t = thread_current();
    if (t->wakeup_count > 0) { t->wakeup_count--; RET(0); }
    RET(thread_wait(WaitType::Sleep, 0, 0, 0, false));
}
HLE(sceKernelWakeupThread) {
    Thread *t = thread_get((int)A0);
    if (!t) RET(SCE_KERNEL_ERROR_UNKNOWN_THID);
    if (t->state == ThreadState::Waiting && t->wait == WaitType::Sleep) {
        thread_wake(t, 0);
        c->r[2] = 0;
        thread_reschedule();
        return;
    }
    t->wakeup_count++;
    RET(0);
}

HLE(sceKernelGetSystemTimeLow) { RET((uint32_t)time_us()); }
HLE(sceKernelReleaseThreadEventHandler) { RET(0); }
HLE(sceKernelCancelAlarm) { RET(0); }

// ---------------------------------------------------------------- semáforos
struct Sema {
    std::string name;
    int count, max;
};
static std::map<int, Sema> g_semas;

HLE(sceKernelCreateSema) {
    int id = next_uid();
    g_semas[id] = {mem_str(A0), (int)A2, (int)A3};
    LOGD("sema", "crear %d '%s' %d/%d", id, mem_str(A0), (int)A2, (int)A3);
    RET(id);
}
HLE(sceKernelDeleteSema) {
    if (!g_semas.erase((int)A0)) RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
    for (Thread *t : threads_waiting_on(WaitType::Sema, (int)A0)) thread_wake(t, SCE_KERNEL_ERROR_WAIT_DELETE);
    RET(0);
}

static void sema_wait(Ctx *c, bool cb) {
    auto it = g_semas.find((int)A0);
    if (it == g_semas.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
    int need = (int)A1;
    if (it->second.count >= need && threads_waiting_on(WaitType::Sema, (int)A0).empty()) {
        it->second.count -= need;
        RET(0);
    }
    thread_current()->wait_a = need;
    uint32_t tmo = A2 ? R32(A2) : 0;
    RET(thread_wait(WaitType::Sema, (int)A0, tmo, A2, cb));
}
HLE(sceKernelWaitSema) { sema_wait(c, false); }
HLE(sceKernelWaitSemaCB) { sema_wait(c, true); }

HLE(sceKernelPollSema) {
    auto it = g_semas.find((int)A0);
    if (it == g_semas.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
    if (it->second.count >= (int)A1) { it->second.count -= (int)A1; RET(0); }
    RET(SCE_KERNEL_ERROR_SEMA_ZERO);
}

HLE(sceKernelSignalSema) {
    auto it = g_semas.find((int)A0);
    if (it == g_semas.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID);
    Sema &s = it->second;
    s.count = std::min(s.count + (int)A1, s.max > 0 ? s.max : 0x7FFFFFFF);
    for (Thread *t : threads_waiting_on(WaitType::Sema, (int)A0)) {
        if (s.count < (int)t->wait_a) break;
        s.count -= (int)t->wait_a;
        thread_wake(t, 0);
    }
    c->r[2] = 0;
    thread_reschedule();
}

// ---------------------------------------------------------------- flags de evento
struct EventFlag {
    std::string name;
    uint32_t bits;
};
static std::map<int, EventFlag> g_evfs;
constexpr uint32_t EVF_OR = 1, EVF_CLEARALL = 0x20, EVF_CLEARPAT = 0x10;

static bool evf_try(EventFlag &e, uint32_t pattern, uint32_t mode, uint32_t out_ptr) {
    bool ok = (mode & EVF_OR) ? (e.bits & pattern) != 0 : (e.bits & pattern) == pattern;
    if (!ok) return false;
    if (out_ptr) W32(out_ptr, e.bits);
    if (mode & EVF_CLEARALL) e.bits = 0;
    if (mode & EVF_CLEARPAT) e.bits &= ~pattern;
    return true;
}

HLE(sceKernelCreateEventFlag) {
    int id = next_uid();
    g_evfs[id] = {mem_str(A0), A2};
    LOGD("evf", "crear %d '%s' bits=%08x", id, mem_str(A0), A2);
    RET(id);
}
HLE(sceKernelDeleteEventFlag) {
    if (!g_evfs.erase((int)A0)) RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
    for (Thread *t : threads_waiting_on(WaitType::EventFlag, (int)A0)) thread_wake(t, SCE_KERNEL_ERROR_WAIT_DELETE);
    RET(0);
}
HLE(sceKernelSetEventFlag) {
    auto it = g_evfs.find((int)A0);
    if (it == g_evfs.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
    it->second.bits |= A1;
    for (Thread *t : threads_waiting_on(WaitType::EventFlag, (int)A0))
        if (evf_try(it->second, t->wait_a, t->wait_b, t->wait_c)) thread_wake(t, 0);
    c->r[2] = 0;
    thread_reschedule();
}
HLE(sceKernelClearEventFlag) {
    auto it = g_evfs.find((int)A0);
    if (it == g_evfs.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
    it->second.bits &= A1;
    RET(0);
}
HLE(sceKernelPollEventFlag) {
    auto it = g_evfs.find((int)A0);
    if (it == g_evfs.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
    if (evf_try(it->second, A1, A2, A3)) RET(0);
    if (A3) W32(A3, it->second.bits);
    RET(SCE_KERNEL_ERROR_EVF_COND);
}
HLE(sceKernelWaitEventFlag) {
    auto it = g_evfs.find((int)A0);
    if (it == g_evfs.end()) RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID);
    if (evf_try(it->second, A1, A2, A3)) RET(0);
    Thread *t = thread_current();
    t->wait_a = A1;
    t->wait_b = A2;
    t->wait_c = A3;
    uint32_t tmo = T0 ? R32(T0) : 0;
    RET(thread_wait(WaitType::EventFlag, (int)A0, tmo, T0, false));
}

// ---------------------------------------------------------------- callbacks
HLE(sceKernelCreateCallback) { RET(callback_create(mem_str(A0), A1, A2)); }
HLE(sceKernelDeleteCallback) { RET(0); }
HLE(sceKernelCheckCallback) {
    Thread *t = thread_current();
    bool had = t->cb_pending;
    if (had) {
        t->wait_cb = true;
        t->wait = WaitType::None;
        // ejecuta los callbacks pendientes de forma síncrona
        thread_wait(WaitType::Callback, 0, 1, 0, true);
    }
    RET(had ? 1 : 0);
}

// ---------------------------------------------------------------- interrupciones
HLE(sceKernelRegisterSubIntrHandler) {
    LOGD("intr", "registrar intr %u sub %u handler=%08x arg=%08x", A0, A1, A2, A3);
    if (A0 == 30) intr_register_vblank_handler((int)A1, A2, A3);
    RET(0);
}
HLE(sceKernelReleaseSubIntrHandler) {
    if (A0 == 30) intr_release_vblank_handler((int)A1);
    RET(0);
}
HLE(sceKernelEnableSubIntr) {
    if (A0 == 30) intr_enable((int)A1, true);
    RET(0);
}
static uint32_t g_intr_flag = 1;
HLE(sceKernelCpuSuspendIntr) { RET(g_intr_flag); }
HLE(sceKernelCpuResumeIntr) { RET(0); }
