// Memoria, tabla de funciones, log y puntos de fallo del código recompilado.
#include "core.h"

#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>

#include "threads.h"

uint8_t *g_mem = nullptr;
int g_log_level = LOG_INFO;

static const char *LEVEL_TAG[] = {"E", "W", "I", "D", "T"};

void log_msg(LogLevel lvl, const char *tag, const char *fmt, ...) {
    if (lvl > g_log_level) return;
    fprintf(stderr, "[%10.6f %s %-6s] ", time_us() / 1e6, LEVEL_TAG[lvl], tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...) {
    fprintf(stderr, "\n*** FATAL: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (Thread *t = thread_current()) {
        Ctx *c = &t->ctx;
        fprintf(stderr, "hilo %d '%s' pc~%08x ra=%08x sp=%08x\n", t->id, t->name.c_str(), c->pc, c->r[31], c->r[29]);
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c->r[i], i + 1, c->r[i + 1],
                    i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    }
    fflush(stderr);
    abort();
}

// ---------------------------------------------------------------- tiempo
static const auto g_t0 = std::chrono::steady_clock::now();
uint64_t time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
}

// ---------------------------------------------------------------- memoria
// Asignador simple de la partición de usuario: lista ordenada de bloques ocupados.
static std::map<uint32_t, std::pair<uint32_t, std::string>> g_blocks;  // dirección -> (tamaño, nombre)
static uint32_t g_heap_lo;

void mem_init() {
    void *p = mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("no se pudieron reservar 512 MB de espacio de direcciones");
    g_mem = static_cast<uint8_t *>(p);
    g_heap_lo = (g_recomp_bss_end + 0xFF) & ~0xFFu;
    // La imagen del juego ocupa desde el inicio de la partición hasta el final de .bss.
    g_blocks[USER_MEM_START] = {g_heap_lo - USER_MEM_START, "imagen del juego"};
}

uint32_t mem_alloc(uint32_t size, bool high, const char *name, uint32_t align) {
    size = (size + 15) & ~15u;
    if (align < 16) align = 16;
    std::vector<std::pair<uint32_t, uint32_t>> gaps;  // huecos libres [ini, fin)
    uint32_t cur = USER_MEM_START;
    for (auto &[addr, b] : g_blocks) {
        if (addr > cur) gaps.push_back({cur, addr});
        cur = std::max(cur, addr + b.first);
    }
    if (cur < USER_MEM_END) gaps.push_back({cur, USER_MEM_END});
    if (high) std::reverse(gaps.begin(), gaps.end());
    for (auto [lo, hi] : gaps) {
        uint32_t addr;
        if (high) {
            if (hi - lo < size) continue;
            addr = (hi - size) & ~(align - 1);
            if (addr < lo) continue;
        } else {
            addr = (lo + align - 1) & ~(align - 1);
            if (addr + size > hi) continue;
        }
        g_blocks[addr] = {size, name ? name : ""};
        memset(MEMP(addr), 0, size);
        LOGD("mem", "alloc %s %08x +%x (%s)", name ? name : "", addr, size, high ? "alto" : "bajo");
        return addr;
    }
    LOGE("mem", "sin memoria para %s (%x bytes)", name ? name : "", size);
    return 0;
}

void mem_free(uint32_t addr) {
    if (!g_blocks.erase(addr)) LOGW("mem", "free de bloque desconocido %08x", addr);
}

// ---------------------------------------------------------------- funciones
// Tabla directa por palabra de código: (addr - base) / 4 -> función.
static std::vector<RecompFunc> g_func_table;
static uint32_t g_func_lo, g_func_hi;

void func_table_init() {
    g_func_lo = 0xFFFFFFFF;
    g_func_hi = 0;
    for (uint32_t i = 0; i < g_recomp_func_count; i++) {
        g_func_lo = std::min(g_func_lo, g_recomp_func_addrs[i]);
        g_func_hi = std::max(g_func_hi, g_recomp_func_addrs[i] + 4);
    }
    g_func_table.assign((g_func_hi - g_func_lo) / 4, nullptr);
    for (uint32_t i = 0; i < g_recomp_func_count; i++)
        g_func_table[(g_recomp_func_addrs[i] - g_func_lo) / 4] = g_recomp_funcs[i];
}

RecompFunc func_lookup(uint32_t addr) {
    addr &= 0x3FFFFFFF;  // quita el bit de espejo sin caché
    if (addr < g_func_lo || addr >= g_func_hi || (addr & 3)) return nullptr;
    return g_func_table[(addr - g_func_lo) / 4];
}

uint32_t call_mips(Ctx *c, uint32_t addr, std::initializer_list<uint32_t> args) {
    Ctx saved = *c;
    int reg = 4;
    for (uint32_t a : args) c->r[reg++] = a;
    c->r[31] = 0;
    recomp_call(c, addr);
    uint32_t v0 = c->r[2], v1 = c->r[3];
    *c = saved;
    c->r[2] = v0;
    c->r[3] = v1;
    return v0;
}

extern "C" void recomp_call(Ctx *c, uint32_t addr) {
    RecompFunc f = func_lookup(addr);
    if (!f) fatal("llamada indirecta a %08x sin función (desde ~%08x)", addr, c->pc);
    f(c);
}

extern "C" void recomp_bad_jump(Ctx *c, uint32_t from, uint32_t to) {
    fatal("salto a %08x fuera de función (desde ~%08x)", to, from ? from : c->pc);
}

extern "C" void recomp_unimplemented(Ctx *c, uint32_t pc, uint32_t word) {
    (void)c;
    fatal("instrucción sin implementar %08x en %08x", word, pc);
}

extern "C" void recomp_break(Ctx *c, uint32_t pc, uint32_t code) {
    (void)c;
    fatal("break %u en %08x", code, pc);
}

extern "C" void recomp_bad_sp(Ctx *c, uint32_t pc) { fatal("sp inválido %08x tras la instrucción %08x", c->r[29], pc); }

static std::unordered_map<uint32_t, jmp_buf> g_jmpbufs;

extern "C" jmp_buf *recomp_jmpbuf(uint32_t mips_jmpbuf) { return &g_jmpbufs[mips_jmpbuf]; }

extern "C" void recomp_longjmp(Ctx *c, uint32_t mips_jmpbuf) {
    auto it = g_jmpbufs.find(mips_jmpbuf);
    if (it == g_jmpbufs.end()) fatal("longjmp a un jmp_buf %08x sin setjmp previo", mips_jmpbuf);
    (void)c;
    longjmp(it->second, 1);
}
