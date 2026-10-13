#include "texture_replace.h"

#include <cstdio>
#include <filesystem>
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

extern "C" {
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

namespace fs = std::filesystem;

uint64_t texrep_content_hash(const uint8_t *rgba, int w, int h) {
    uint8_t head[8], digest[16];
    for (int i = 0; i < 4; i++) {
        head[i] = (uint8_t)((uint32_t)w >> (8 * i));
        head[4 + i] = (uint8_t)((uint32_t)h >> (8 * i));
    }
    AVMD5 *md5 = av_md5_alloc();
    av_md5_init(md5);
    av_md5_update(md5, head, 8);
    av_md5_update(md5, rgba, (size_t)w * h * 4);
    av_md5_final(md5, digest);
    av_free(md5);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | digest[i];  // "%016llx" imprime los bytes en orden
    return v;
}

static std::string g_replace_dir, g_dump_dir;
static std::unordered_set<uint64_t> g_available, g_dumped;

std::string texrep_name(uint64_t hash) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx.png", (unsigned long long)hash);
    return buf;
}

int texrep_rescan() {
    g_available.clear();
    std::error_code ec;
    if (fs::is_directory(g_replace_dir, ec)) {
        for (const auto &e : fs::directory_iterator(g_replace_dir, ec)) {
            unsigned long long h;
            char ext[8];
            if (sscanf(e.path().filename().string().c_str(), "%16llx.%4s", &h, ext) == 2) g_available.insert(h);
        }
        LOGI("tex", "%zu texturas de sustitución en %s", g_available.size(), g_replace_dir.c_str());
    }
    return (int)g_available.size();
}

void texrep_init(const std::string &replace_dir, const std::string &dump_dir) {
    g_replace_dir = replace_dir;
    g_dump_dir = dump_dir;
    std::error_code ec;
    if (!dump_dir.empty()) fs::create_directories(dump_dir, ec);
    texrep_rescan();
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
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4);
}

void texrep_write_png(const std::string &path, const uint8_t *rgba, int w, int h) {
    stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4);
}
