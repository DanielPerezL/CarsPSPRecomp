// Estado y servicios internos del runtime (no visibles para el código recompilado).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "recomp.h"

// En MinGW el printf es el de C99 (__USE_MINGW_ANSI_STDIO), no el de msvcrt: comprobar con esas reglas.
#ifdef _WIN32
#define CARS_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#else
#define CARS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#endif

// ---------------------------------------------------------------- log
enum LogLevel { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE };
extern int g_log_level;
void log_msg(LogLevel lvl, const char *tag, const char *fmt, ...) CARS_PRINTF(3, 4);
#define LOGE(tag, ...) log_msg(LOG_ERROR, tag, __VA_ARGS__)
#define LOGW(tag, ...) log_msg(LOG_WARN, tag, __VA_ARGS__)
#define LOGI(tag, ...) log_msg(LOG_INFO, tag, __VA_ARGS__)
#define LOGD(tag, ...) do { if (g_log_level >= LOG_DEBUG) log_msg(LOG_DEBUG, tag, __VA_ARGS__); } while (0)
#define LOGT(tag, ...) do { if (g_log_level >= LOG_TRACE) log_msg(LOG_TRACE, tag, __VA_ARGS__); } while (0)
[[noreturn]] void fatal(const char *fmt, ...) CARS_PRINTF(1, 2);

// ---------------------------------------------------------------- memoria
constexpr uint32_t USER_MEM_START = 0x08800000;
constexpr uint32_t USER_MEM_END = 0x0A000000;
constexpr uint32_t VRAM_START = 0x04000000;
constexpr uint32_t VRAM_SIZE = 0x00200000;
constexpr uint32_t SCRATCHPAD_START = 0x00010000;

void mem_init();
// Reserva en la partición de usuario; high=true asigna desde arriba (pilas).
uint32_t mem_alloc(uint32_t size, bool high, const char *name, uint32_t align = 16);
void mem_free(uint32_t addr);
inline char *mem_str(uint32_t addr) { return reinterpret_cast<char *>(MEMP(addr)); }

// ---------------------------------------------------------------- funciones recompiladas
void func_table_init();
RecompFunc func_lookup(uint32_t addr);
// Ejecuta código MIPS en el contexto dado preservando todos los registros salvo v0/v1.
uint32_t call_mips(Ctx *c, uint32_t addr, std::initializer_list<uint32_t> args);

// ---------------------------------------------------------------- tiempo
uint64_t time_us();  // tiempo de la PSP en microsegundos desde el arranque

// ---------------------------------------------------------------- HLE
using HleFunc = void (*)(Ctx *);
void hle_register(const char *name, HleFunc fn);
void hle_init();
struct HleRegistrar {
    HleRegistrar(const char *name, HleFunc fn) { hle_register(name, fn); }
};
#define HLE(name)                                                   \
    static void hle_##name(Ctx *c);                                 \
    static HleRegistrar reg_##name(#name, hle_##name);              \
    static void hle_##name([[maybe_unused]] Ctx *c)
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]
#define T0 c->r[8]
#define T1 c->r[9]
#define T2 c->r[10]
#define T3 c->r[11]
#define RET(v) do { c->r[2] = (uint32_t)(v); return; } while (0)

// Códigos de error del kernel de la PSP usados por varios módulos
constexpr uint32_t SCE_KERNEL_ERROR_UNKNOWN_THID = 0x80020198;
constexpr uint32_t SCE_KERNEL_ERROR_UNKNOWN_SEMID = 0x80020199;
constexpr uint32_t SCE_KERNEL_ERROR_UNKNOWN_EVFID = 0x8002019A;
constexpr uint32_t SCE_KERNEL_ERROR_UNKNOWN_CBID = 0x800201A1;
constexpr uint32_t SCE_KERNEL_ERROR_WAIT_TIMEOUT = 0x800201A8;
constexpr uint32_t SCE_KERNEL_ERROR_WAIT_DELETE = 0x800201B5;
constexpr uint32_t SCE_KERNEL_ERROR_SEMA_ZERO = 0x800201AD;
constexpr uint32_t SCE_KERNEL_ERROR_EVF_COND = 0x800201AF;
constexpr uint32_t SCE_KERNEL_ERROR_NOT_DORMANT = 0x800201A4;
constexpr uint32_t SCE_KERNEL_ERROR_DORMANT = 0x800201A2;
constexpr uint32_t SCE_ERROR_FILE_NOT_FOUND = 0x80010002;
constexpr uint32_t SCE_ERROR_BAD_FD = 0x80010009;

// ---------------------------------------------------------------- rutas
extern std::string g_disc_root;  // carpeta con el contenido del UMD (extracted/)
extern std::string g_save_root;  // memory stick (partidas guardadas)
