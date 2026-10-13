// Capa mínima de sistema operativo: memoria virtual, fibers y esperas (Linux/POSIX y Windows).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

// Reserva (y deja accesible) un bloque grande de memoria inicializado a cero bajo demanda.
void *os_reserve_memory(size_t size);

// Fibers cooperativas: cada hilo de la PSP corre en una con su propia pila del host.
struct OsFiber;
// Convierte el hilo actual del host en fiber (la del planificador). Llamar una vez al principio.
OsFiber *os_fiber_main();
// Crea una fiber que ejecutará fn(arg). fn no debe retornar: al terminar, cambia a otra fiber.
OsFiber *os_fiber_create(size_t stack_size, void (*fn)(void *), void *arg);
// Cambia de la fiber actual a `to`.
void os_fiber_switch(OsFiber *to);
// Libera una fiber que no esté en ejecución.
void os_fiber_destroy(OsFiber *f);

void os_sleep_us(uint64_t us);

// Utilidades portables
int os_stricmp(const char *a, const char *b);
int64_t os_fseek(FILE *f, int64_t off, int whence);  // devuelve la nueva posición o -1
void os_localtime(time_t t, struct tm *out);
uint64_t os_unix_time_us();  // hora real (UTC) en microsegundos desde 1970

// Carpeta del ejecutable, terminada en separador ("" si no se sabe).
std::string os_exe_dir();
// En Windows el ejecutable no tiene consola: si stderr no va a ninguna parte, lo manda a log_path.
void os_redirect_log(const std::string &log_path);
// Mensaje de error visible aunque no haya consola (cuadro de diálogo en Windows); texto en UTF-8.
void os_error_box(const char *text);
