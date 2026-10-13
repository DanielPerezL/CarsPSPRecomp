// Hilos de la PSP sobre fibers del host y planificador por prioridad (cooperativo como el kernel real:
// sin reparto de tiempo; solo se cambia de hilo al bloquear o al despertar uno más prioritario).
#pragma once
#include <ucontext.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core.h"

enum class WaitType { None, Delay, Sleep, Sema, EventFlag, ThreadEnd, Vblank, Callback, Generic };
enum class ThreadState { Dormant, Ready, Running, Waiting, Dead };

struct Thread {
    int id = 0;
    std::string name;
    uint32_t entry = 0, attr = 0, stack_addr = 0, stack_size = 0;
    int prio = 0;
    Ctx ctx{};
    ThreadState state = ThreadState::Dormant;
    uint64_t ready_seq = 0;

    // espera
    WaitType wait = WaitType::None;
    int wait_id = 0;
    uint32_t wait_a = 0, wait_b = 0, wait_c = 0;  // parámetros según el tipo
    uint64_t wake_at = 0;                         // 0 = sin timeout
    uint32_t timeout_ptr = 0;
    bool wait_cb = false;      // espera "CB": puede ejecutar callbacks
    bool cb_pending = false;   // hay callbacks que ejecutar en este hilo
    uint32_t wait_result = 0;  // valor de retorno al despertar

    int wakeup_count = 0;
    uint32_t exit_status = 0;

    // fiber
    ucontext_t uc{};
    std::unique_ptr<uint8_t[]> host_stack;
    bool started_fiber = false;
};

void sched_init();
[[noreturn]] void sched_run();
Thread *thread_current();
Thread *thread_get(int id);
int thread_create(const char *name, uint32_t entry, int prio, uint32_t stack_size, uint32_t attr);
void thread_start(Thread *t, uint32_t argsize, uint32_t argp);

// Bloquea el hilo actual hasta que lo despierten; devuelve wait_result.
uint32_t thread_wait(WaitType type, int id, uint64_t timeout_us, uint32_t timeout_ptr, bool cb);
void thread_wake(Thread *t, uint32_t result);
// Termina el hilo actual (Dormant, o Dead si se borra) y no vuelve.
[[noreturn]] void thread_exit_current(uint32_t status, bool del);
// Cede si hay un hilo listo más prioritario que el actual.
void thread_reschedule();
std::vector<Thread *> threads_waiting_on(WaitType type, int id);

// Interrupciones (vblank) y callbacks
void intr_register_vblank_handler(int sub, uint32_t handler, uint32_t arg);
void intr_release_vblank_handler(int sub);
void intr_enable(int sub, bool on);
uint64_t vblank_count();
// Ejecuta código MIPS en contexto de interrupción (pila propia), desde el planificador.
uint32_t intr_call(uint32_t addr, std::initializer_list<uint32_t> args);
void callback_notify(int cbid, uint32_t arg);
int callback_create(const char *name, uint32_t func, uint32_t arg);
// Tarea periódica del planificador (p. ej. audio o GE); se llama en cada vuelta.
void sched_add_hook(void (*fn)(uint64_t now));
void threads_dump();
