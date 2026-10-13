#include "os.h"

#include <SDL.h>

#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *os_reserve_memory(size_t size) {
    // MEM_COMMIT en Windows solo cuenta contra el archivo de paginación: las páginas se materializan
    // (a cero) al tocarlas por primera vez.
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

struct OsFiber {
    LPVOID handle = nullptr;
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
};

static OsFiber *g_current = nullptr;

static VOID CALLBACK fiber_start(LPVOID p) {
    OsFiber *f = static_cast<OsFiber *>(p);
    f->fn(f->arg);
}

OsFiber *os_fiber_main() {
    static OsFiber main_fiber;
    main_fiber.handle = ConvertThreadToFiber(nullptr);
    g_current = &main_fiber;
    return &main_fiber;
}

OsFiber *os_fiber_create(size_t stack_size, void (*fn)(void *), void *arg) {
    OsFiber *f = new OsFiber;
    f->fn = fn;
    f->arg = arg;
    f->handle = CreateFiber(stack_size, fiber_start, f);
    return f;
}

void os_fiber_switch(OsFiber *to) {
    g_current = to;
    SwitchToFiber(to->handle);
}

void os_fiber_destroy(OsFiber *f) {
    if (!f) return;
    DeleteFiber(f->handle);
    delete f;
}

#else  // POSIX
#include <sys/mman.h>
#include <ucontext.h>

#include <memory>

void *os_reserve_memory(size_t size) {
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

struct OsFiber {
    ucontext_t uc{};
    std::unique_ptr<uint8_t[]> stack;
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
};

static OsFiber *g_current = nullptr;
static OsFiber *g_starting = nullptr;

static void fiber_start() {
    OsFiber *f = g_starting;
    f->fn(f->arg);
}

OsFiber *os_fiber_main() {
    static OsFiber main_fiber;
    g_current = &main_fiber;
    return &main_fiber;
}

OsFiber *os_fiber_create(size_t stack_size, void (*fn)(void *), void *arg) {
    OsFiber *f = new OsFiber;
    f->fn = fn;
    f->arg = arg;
    f->stack.reset(new uint8_t[stack_size]);
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack.get();
    f->uc.uc_stack.ss_size = stack_size;
    f->uc.uc_link = nullptr;
    makecontext(&f->uc, fiber_start, 0);
    return f;
}

void os_fiber_switch(OsFiber *to) {
    OsFiber *from = g_current;
    g_current = to;
    g_starting = to;  // solo lo usa fiber_start en la primera entrada de la fiber
    swapcontext(&from->uc, &to->uc);
}

void os_fiber_destroy(OsFiber *f) { delete f; }
#endif

void os_sleep_us(uint64_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }

int os_stricmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}

int64_t os_fseek(FILE *f, int64_t off, int whence) {
#ifdef _WIN32
    if (_fseeki64(f, off, whence) != 0) return -1;
    return _ftelli64(f);
#else
    if (fseeko(f, (off_t)off, whence) != 0) return -1;
    return ftello(f);
#endif
}

void os_localtime(time_t t, struct tm *out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

uint64_t os_unix_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string os_exe_dir() {
    char *base = SDL_GetBasePath();
    std::string dir = base ? base : "";
    SDL_free(base);
    return dir;
}

void os_redirect_log(const std::string &log_path) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) freopen(log_path.c_str(), "w", stderr);
#else
    (void)log_path;
#endif
}

void os_error_box(const char *text) {
#ifdef _WIN32
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cars", text, nullptr);
#else
    (void)text;  // en Linux basta con stderr
#endif
}
