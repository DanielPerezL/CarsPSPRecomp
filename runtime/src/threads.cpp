#include "threads.h"

#include <time.h>

#include <algorithm>
#include <map>

#include "display.h"
#include "gpu/renderer.h"

// Pila del host por hilo: el driver de OpenGL se ejecuta dentro de los hilos (la GE dibuja al encolar).
static constexpr size_t HOST_STACK = 4 << 20;
static constexpr uint64_t VBLANK_US = 16683;  // 59.94 Hz

static std::map<int, std::unique_ptr<Thread>> g_threads;
static Thread *g_current = nullptr;
static ucontext_t g_sched_uc;
static int g_next_uid = 0x100;
static uint64_t g_seq = 0;
static uint64_t g_next_vblank = VBLANK_US;
static uint64_t g_vblanks = 0;
static std::vector<void (*)(uint64_t)> g_hooks;

int next_uid() { return g_next_uid++; }

Thread *thread_current() { return g_current; }
Thread *thread_get(int id) {
    if (id == 0) return g_current;
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second.get();
}

static void make_ready(Thread *t) {
    t->state = ThreadState::Ready;
    t->ready_seq = ++g_seq;
}

// ---------------------------------------------------------------- fibers
static void fiber_entry() {
    Thread *t = g_current;
    recomp_call(&t->ctx, t->entry);
    // retorno de la función de entrada = sceKernelExitThread(v0)
    t->exit_status = t->ctx.r[2];
    t->state = ThreadState::Dormant;
    for (Thread *w : threads_waiting_on(WaitType::ThreadEnd, t->id)) thread_wake(w, t->exit_status);
    LOGD("thread", "hilo %d '%s' terminó (%08x)", t->id, t->name.c_str(), t->exit_status);
    // uc_link vuelve al planificador
}

int thread_create(const char *name, uint32_t entry, int prio, uint32_t stack_size, uint32_t attr) {
    auto t = std::make_unique<Thread>();
    t->id = next_uid();
    t->name = name ? name : "";
    t->entry = entry;
    t->prio = prio;
    t->attr = attr;
    t->stack_size = (stack_size + 0xFF) & ~0xFFu;
    t->stack_addr = mem_alloc(t->stack_size, true, ("stack " + t->name).c_str(), 256);
    if (!t->stack_addr) return -1;
    memset(MEMP(t->stack_addr), 0xFF, t->stack_size);
    int id = t->id;
    LOGD("thread", "crear %d '%s' entry=%08x prio=%d pila=%08x+%x", id, t->name.c_str(), entry, prio, t->stack_addr,
         t->stack_size);
    g_threads[id] = std::move(t);
    return id;
}

void thread_start(Thread *t, uint32_t argsize, uint32_t argp) {
    memset(&t->ctx, 0, sizeof(t->ctx));
    uint32_t sp = t->stack_addr + t->stack_size - 64;
    if (argp && argsize) {
        sp -= (argsize + 15) & ~15u;
        memcpy(MEMP(sp), MEMP(argp), argsize);
        argp = sp;
    }
    t->ctx.r[4] = argsize;
    t->ctx.r[5] = argp ? sp : 0;
    t->ctx.r[28] = g_recomp_gp;
    t->ctx.r[29] = sp - 64;
    t->ctx.fcr31 = 0x00000E00;
    t->ctx.vfpu_ctrl[0] = t->ctx.vfpu_ctrl[1] = 0xE4;  // prefijos S/T neutros
    t->host_stack.reset(new uint8_t[HOST_STACK]);
    getcontext(&t->uc);
    t->uc.uc_stack.ss_sp = t->host_stack.get();
    t->uc.uc_stack.ss_size = HOST_STACK;
    t->uc.uc_link = &g_sched_uc;
    makecontext(&t->uc, fiber_entry, 0);
    t->wait = WaitType::None;
    t->wakeup_count = 0;
    make_ready(t);
    LOGD("thread", "iniciar %d '%s' argsize=%u", t->id, t->name.c_str(), argsize);
}

// Vuelve al planificador desde el hilo actual.
static void yield_to_scheduler() {
    Thread *t = g_current;
    swapcontext(&t->uc, &g_sched_uc);
}

void thread_exit_current(uint32_t status, bool del) {
    Thread *t = g_current;
    t->exit_status = status;
    t->state = del ? ThreadState::Dead : ThreadState::Dormant;
    for (Thread *w : threads_waiting_on(WaitType::ThreadEnd, t->id)) thread_wake(w, status);
    LOGD("thread", "exit %d '%s' (%08x)%s", t->id, t->name.c_str(), status, del ? " y borrar" : "");
    yield_to_scheduler();
    fatal("hilo terminado reanudado");
}

// ---------------------------------------------------------------- esperas
static void run_callbacks(Thread *t);

uint32_t thread_wait(WaitType type, int id, uint64_t timeout_us, uint32_t timeout_ptr, bool cb) {
    Thread *t = g_current;
    t->wait = type;
    t->wait_id = id;
    t->wait_cb = cb;
    t->timeout_ptr = timeout_ptr;
    t->wake_at = timeout_us ? time_us() + timeout_us : 0;
    t->wait_result = 0;
    t->state = ThreadState::Waiting;
    for (;;) {
        yield_to_scheduler();
        if (t->cb_pending && t->wait_cb) {
            run_callbacks(t);
            if (t->wait != WaitType::None) {
                t->state = ThreadState::Waiting;
                continue;
            }
        }
        break;
    }
    return t->wait_result;
}

void thread_wake(Thread *t, uint32_t result) {
    if (t->state != ThreadState::Waiting) return;
    if (t->timeout_ptr && t->wake_at) {
        uint64_t now = time_us();
        W32(t->timeout_ptr, t->wake_at > now ? (uint32_t)(t->wake_at - now) : 0);
    }
    t->wait = WaitType::None;
    t->wait_result = result;
    make_ready(t);
}

std::vector<Thread *> threads_waiting_on(WaitType type, int id) {
    std::vector<Thread *> out;
    for (auto &[_, t] : g_threads)
        if (t->state == ThreadState::Waiting && t->wait == type && t->wait_id == id) out.push_back(t.get());
    std::sort(out.begin(), out.end(), [](Thread *a, Thread *b) { return a->ready_seq < b->ready_seq; });
    return out;
}

static Thread *pick_ready() {
    Thread *best = nullptr;
    for (auto &[_, t] : g_threads)
        if (t->state == ThreadState::Ready &&
            (!best || t->prio < best->prio || (t->prio == best->prio && t->ready_seq < best->ready_seq)))
            best = t.get();
    return best;
}

void thread_reschedule() {
    Thread *cur = g_current;
    Thread *best = pick_ready();
    if (cur && best && best->prio < cur->prio) {
        make_ready(cur);
        yield_to_scheduler();
    }
}

// ---------------------------------------------------------------- callbacks
struct Callback {
    int id;
    std::string name;
    uint32_t func, arg;
    int thread;
    int notify_count = 0;
    uint32_t notify_arg = 0;
};
static std::map<int, Callback> g_callbacks;

int callback_create(const char *name, uint32_t func, uint32_t arg) {
    int id = next_uid();
    g_callbacks[id] = Callback{id, name ? name : "", func, arg, g_current ? g_current->id : 0};
    LOGD("thread", "callback %d '%s' func=%08x", id, name, func);
    return id;
}

void callback_notify(int cbid, uint32_t arg) {
    auto it = g_callbacks.find(cbid);
    if (it == g_callbacks.end()) return;
    it->second.notify_count++;
    it->second.notify_arg = arg;
    if (Thread *t = thread_get(it->second.thread)) {
        t->cb_pending = true;
        if (t->state == ThreadState::Waiting && t->wait_cb) make_ready(t);
    }
}

static void run_callbacks(Thread *t) {
    t->cb_pending = false;
    for (auto &[id, cb] : g_callbacks) {
        if (cb.thread != t->id || cb.notify_count == 0) continue;
        int count = cb.notify_count;
        cb.notify_count = 0;
        uint32_t r = call_mips(&t->ctx, cb.func, {(uint32_t)count, cb.notify_arg, cb.arg});
        if (r) g_callbacks.erase(id);  // devolver != 0 borra el callback
        break;  // el iterador puede quedar invalidado; el resto en la siguiente vuelta
    }
    for (auto &[id, cb] : g_callbacks)
        if (cb.thread == t->id && cb.notify_count) t->cb_pending = true;
}

// ---------------------------------------------------------------- interrupciones
struct SubIntr {
    uint32_t handler = 0, arg = 0;
    bool enabled = false;
};
static std::map<int, SubIntr> g_vblank_handlers;
static Ctx g_intr_ctx;
static uint32_t g_intr_stack = 0;

void intr_register_vblank_handler(int sub, uint32_t handler, uint32_t arg) { g_vblank_handlers[sub] = {handler, arg, false}; }
void intr_release_vblank_handler(int sub) { g_vblank_handlers.erase(sub); }
void intr_enable(int sub, bool on) { g_vblank_handlers[sub].enabled = on; }
uint64_t vblank_count() { return g_vblanks; }

uint32_t intr_call(uint32_t addr, std::initializer_list<uint32_t> args) {
    if (!g_intr_stack) g_intr_stack = mem_alloc(0x4000, true, "pila de interrupciones");
    memset(&g_intr_ctx, 0, sizeof(g_intr_ctx));
    g_intr_ctx.r[28] = g_recomp_gp;
    g_intr_ctx.vfpu_ctrl[0] = g_intr_ctx.vfpu_ctrl[1] = 0xE4;
    g_intr_ctx.r[29] = g_intr_stack + 0x4000 - 64;
    Thread *prev = g_current;
    g_current = nullptr;
    uint32_t r = call_mips(&g_intr_ctx, addr, args);
    g_current = prev;
    return r;
}

void sched_add_hook(void (*fn)(uint64_t)) { g_hooks.push_back(fn); }

void threads_dump() {
    static const char *WT[] = {"-", "delay", "sleep", "sema", "evflag", "thread-end", "vblank", "callback", "otro"};
    static const char *ST[] = {"dormido", "listo", "ejecutando", "esperando", "muerto"};
    for (auto &[id, t] : g_threads)
        LOGI("thread", "  %d '%s' prio=%d %s espera=%s id=%d ra=%08x", id, t->name.c_str(), t->prio,
             ST[(int)t->state], WT[(int)t->wait], t->wait_id, t->ctx.r[31]);
}

// ---------------------------------------------------------------- planificador
void sched_init() {}

static void do_vblank() {
    g_vblanks++;
    for (auto &[sub, h] : g_vblank_handlers)
        if (h.enabled && h.handler) intr_call(h.handler, {(uint32_t)sub, h.arg});
    for (auto &[_, t] : g_threads)
        if (t->state == ThreadState::Waiting && t->wait == WaitType::Vblank) thread_wake(t.get(), 0);
    display_vblank();
}

void sched_run() {
    for (;;) {
        uint64_t now = time_us();
        while (now >= g_next_vblank) {
            do_vblank();
            g_next_vblank += VBLANK_US;
        }
        for (auto fn : g_hooks) fn(now);
        // Presentación de fotogramas interpolados cuando toca, aunque haya hilos activos
        // (p. ej. el lector de vídeo hace esperas muy cortas y el planificador casi nunca está ocioso).
        if (renderer_next_present_in() == 0) display_idle();
        uint64_t next_event = g_next_vblank;
        for (auto &[_, t] : g_threads) {
            if (t->state != ThreadState::Waiting || !t->wake_at) continue;
            if (t->wake_at <= now) {
                uint32_t res = t->wait == WaitType::Delay ? 0 : SCE_KERNEL_ERROR_WAIT_TIMEOUT;
                thread_wake(t.get(), res);
            } else {
                next_event = std::min(next_event, t->wake_at);
            }
        }
        Thread *t = pick_ready();
        if (!t) {
            bool any = false;
            for (auto &[_, th] : g_threads)
                if (th->state == ThreadState::Waiting) any = true;
            if (!any) fatal("no quedan hilos vivos ni en espera");
            uint64_t wait = next_event > now ? next_event - now : 0;
            // Mientras el juego espera se presentan fotogramas interpolados.
            if (wait > 500) wait = std::min(wait, display_idle());
            if (wait) {
                timespec ts{(time_t)(wait / 1000000), (long)(wait % 1000000) * 1000};
                nanosleep(&ts, nullptr);
            }
            continue;
        }
        g_current = t;
        t->state = ThreadState::Running;
        swapcontext(&g_sched_uc, &t->uc);
        g_current = nullptr;
        if (t->state == ThreadState::Dead) g_threads.erase(t->id);
    }
}
