#include "iso.h"

#include <cstdio>
#include <cstring>

#include "platform/os.h"

static FILE *g_iso = nullptr;
static IsoEntry g_root;
static constexpr uint32_t SECTOR = 2048;

bool iso_loaded() { return g_iso != nullptr; }

size_t iso_read(uint32_t lbn, uint64_t offset, void *dst, size_t len) {
    if (!g_iso) return 0;
    os_fseek(g_iso, (int64_t)lbn * SECTOR + (int64_t)offset, SEEK_SET);
    return fread(dst, 1, len, g_iso);
}

bool iso_open(const std::string &path) {
    g_iso = fopen(path.c_str(), "rb");
    if (!g_iso) return false;
    uint8_t pvd[SECTOR];
    if (iso_read(16, 0, pvd, SECTOR) != SECTOR || memcmp(pvd + 1, "CD001", 5) != 0) {
        fclose(g_iso);
        g_iso = nullptr;
        return false;
    }
    memcpy(&g_root.lbn, pvd + 156 + 2, 4);
    memcpy(&g_root.size, pvd + 156 + 10, 4);
    g_root.dir = true;
    return true;
}

std::vector<IsoEntry> iso_list(const IsoEntry &dir) {
    std::vector<IsoEntry> out;
    std::vector<uint8_t> data(dir.size);
    iso_read(dir.lbn, 0, data.data(), data.size());
    size_t i = 0;
    while (i < data.size()) {
        uint8_t len = data[i];
        if (len == 0) {  // relleno hasta el siguiente sector
            i = (i / SECTOR + 1) * SECTOR;
            continue;
        }
        uint8_t namelen = data[i + 32];
        const char *name = reinterpret_cast<const char *>(&data[i + 33]);
        if (!(namelen == 1 && (name[0] == 0 || name[0] == 1))) {
            IsoEntry e;
            e.name.assign(name, namelen);
            size_t semi = e.name.find(';');
            if (semi != std::string::npos) e.name.resize(semi);
            memcpy(&e.lbn, &data[i + 2], 4);
            memcpy(&e.size, &data[i + 10], 4);
            e.dir = data[i + 25] & 2;
            out.push_back(e);
        }
        i += len;
    }
    return out;
}

bool iso_lookup(const std::string &path_in, IsoEntry *out) {
    std::string path = path_in;
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    unsigned lbn = 0, size = 0;
    if (sscanf(path.c_str(), "sce_lbn0x%x_size0x%x", &lbn, &size) == 2) {
        *out = IsoEntry{path, lbn, size, false};
        return true;
    }
    IsoEntry cur = g_root;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string comp = path.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        if (!cur.dir) return false;
        bool found = false;
        for (const IsoEntry &e : iso_list(cur)) {
            if (os_stricmp(e.name.c_str(), comp.c_str()) == 0) {
                cur = e;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    *out = cur;
    return true;
}
