#include "texture_replace.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <unordered_set>

#include "../core.h"

// stb (dominio público), código de terceros: sin avisos
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop

static std::string g_replace_dir, g_dump_dir;
static std::unordered_set<uint64_t> g_available, g_dumped;

std::string texrep_name(uint64_t hash) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx.png", (unsigned long long)hash);
    return buf;
}

void texrep_init(const std::string &replace_dir, const std::string &dump_dir) {
    g_replace_dir = replace_dir;
    g_dump_dir = dump_dir;
    if (!dump_dir.empty()) mkdir(dump_dir.c_str(), 0755);
    if (DIR *d = opendir(replace_dir.c_str())) {
        while (dirent *e = readdir(d)) {
            unsigned long long h;
            char ext[8];
            if (sscanf(e->d_name, "%16llx.%4s", &h, ext) == 2) g_available.insert(h);
        }
        closedir(d);
        LOGI("tex", "%zu texturas de sustitución en %s", g_available.size(), replace_dir.c_str());
    }
}

bool texrep_load(uint64_t hash, std::vector<uint8_t> &rgba, int &w, int &h) {
    if (!g_available.count(hash)) return false;
    std::string path = g_replace_dir + "/" + texrep_name(hash);
    int n;
    uint8_t *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px) {
        LOGW("tex", "no se pudo cargar %s", path.c_str());
        g_available.erase(hash);
        return false;
    }
    rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

void texrep_dump(uint64_t hash, const uint8_t *rgba, int w, int h) {
    if (g_dump_dir.empty() || !g_dumped.insert(hash).second) return;
    std::string path = g_dump_dir + "/" + texrep_name(hash);
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return;
    stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4);
}
