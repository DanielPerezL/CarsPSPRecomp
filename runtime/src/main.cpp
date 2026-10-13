// Arranque: carga la imagen relocalizada, crea el hilo principal del módulo y entra en el planificador.
#include <cstdlib>
#include <string>

#include "display.h"
#include "iso.h"
#include "threads.h"

static void usage() {
    fprintf(stderr,
            "uso: cars --iso JUEGO.iso [--image FICHERO] [--saves DIR] [-v|-vv]\n"
            "  --iso    imagen ISO del UMD de Cars (obligatoria)\n"
            "  --image  imagen relocalizada generada por el recompilador (build/gen/image.bin)\n");
    exit(1);
}

int main(int argc, char **argv) {
    std::string image = "build/gen/image.bin";
    std::string iso;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (a == "--disc" && i + 1 < argc) g_disc_root = argv[++i];
        else if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--saves" && i + 1 < argc) g_save_root = argv[++i];
        else if (a == "-v") g_log_level = LOG_DEBUG;
        else if (a == "-vv") g_log_level = LOG_TRACE;
        else usage();
    }
    if (const char *lvl = getenv("CARS_LOG")) g_log_level = atoi(lvl);

    if (iso.empty() || !iso_open(iso)) {
        fprintf(stderr, "no se pudo abrir la ISO '%s'\n", iso.c_str());
        usage();
    }
    mem_init();
    FILE *f = fopen(image.c_str(), "rb");
    if (!f) fatal("no se encuentra la imagen %s (ejecuta el recompilador)", image.c_str());
    size_t n = fread(MEMP(g_recomp_image_base), 1, g_recomp_image_size, f);
    fclose(f);
    if (n != g_recomp_image_size) fatal("imagen incompleta: %zu de %u bytes", n, g_recomp_image_size);

    func_table_init();
    hle_init();
    display_init();
    sched_init();

    // El cargador de la PSP arranca module_start en un hilo "user_main" con la ruta del ejecutable.
    const char *path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = mem_alloc(64, false, "argv");
    strcpy(mem_str(argp), path);
    int tid = thread_create("user_main", g_recomp_entry, 0x20, 0x40000, 0x80000000);
    thread_start(thread_get(tid), (uint32_t)strlen(path) + 1, argp);
    LOGI("main", "arranque: entrada %08x, %u funciones recompiladas", g_recomp_entry, g_recomp_func_count);
    sched_run();
}
