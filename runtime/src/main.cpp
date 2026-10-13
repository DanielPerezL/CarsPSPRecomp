// Arranque: carga la imagen relocalizada, crea el hilo principal del módulo y entra en el planificador.
#include <cstdlib>
#include <string>

#include "display.h"
#include "gpu/renderer.h"
#include "iso.h"
#include "platform/window.h"
#include "threads.h"

void audio_init(bool enabled, const std::string &record_path);

static void usage() {
    fprintf(stderr,
            "uso: cars --iso JUEGO.iso [opciones]\n"
            "  --iso FICHERO       imagen ISO del UMD de Cars (obligatoria)\n"
            "  --scale N           resolución interna = 480x272 * N (por defecto 4 = 1920x1088)\n"
            "  --size AxB          tamaño inicial de la ventana (por defecto 1280x720)\n"
            "  --fps N             fotogramas por segundo presentados (por defecto, los del monitor)\n"
            "  --no-interpolation  sin fotogramas intermedios (30 fps como en la PSP)\n"
            "  --textures DIR      texturas de sustitución <hash>.png (por defecto textures)\n"
            "  --dump-textures DIR vuelca las texturas originales como <hash>.png\n"
            "  --fullscreen        pantalla completa (F11 alterna)\n"
            "  --vsync             sincronizar con el monitor\n"
            "  --headless          sin ventana ni renderizado (solo lógica)\n"
            "  --hidden            ventana oculta (pruebas automáticas con capturas)\n"
            "  --screenshot T:F    captura en el segundo T al fichero F (.ppm); repetible\n"
            "  --exit-after T      salir tras T segundos\n"
            "  --record-audio F    guarda la mezcla de audio en un WAV\n"
            "  --image FICHERO     imagen relocalizada (por defecto build/gen/image.bin)\n"
            "  --saves DIR         carpeta de partidas (por defecto saves)\n"
            "  -v | -vv            más registro (estado de hilos / traza de llamadas al sistema)\n");
    exit(1);
}

int main(int argc, char **argv) {
    std::string image = "build/gen/image.bin";
    std::string iso;
    WindowConfig wcfg;
    RendererConfig rcfg;
    bool headless = false;
    std::string record_audio;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (a == "--disc" && i + 1 < argc) g_disc_root = argv[++i];
        else if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--saves" && i + 1 < argc) g_save_root = argv[++i];
        else if (a == "--scale" && i + 1 < argc) rcfg.scale = atoi(argv[++i]);
        else if (a == "--no-interpolation") rcfg.interpolate = false;
        else if (a == "--textures" && i + 1 < argc) rcfg.textures_dir = argv[++i];
        else if (a == "--dump-textures" && i + 1 < argc) rcfg.dump_dir = argv[++i];
        else if (a == "--fps" && i + 1 < argc) rcfg.max_fps = atoi(argv[++i]);
        else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &wcfg.width, &wcfg.height);
        else if (a == "--fullscreen") wcfg.fullscreen = true;
        else if (a == "--vsync") wcfg.vsync = true;
        else if (a == "--headless") headless = true;
        else if (a == "--record-audio" && i + 1 < argc) record_audio = argv[++i];
        else if (a == "--hidden") wcfg.hidden = true;
        else if (a == "--exit-after" && i + 1 < argc) display_set_exit_after(atof(argv[++i]));
        else if (a == "--screenshot" && i + 1 < argc) {
            std::string v = argv[++i];
            size_t colon = v.find(':');
            if (colon == std::string::npos) usage();
            display_add_screenshot(atof(v.substr(0, colon).c_str()), v.substr(colon + 1));
        }
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
    if (!headless) {
        if (!window_init(wcfg) || !renderer_init(rcfg)) fatal("no se pudo inicializar la ventana/OpenGL");
    }
    display_init(!headless);
    audio_init(!headless, record_audio);
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
