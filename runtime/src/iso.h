// Lectura del UMD directamente desde la ISO (ISO9660), incluido el acceso por sector "sce_lbn0x..._size0x...".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct IsoEntry {
    std::string name;
    uint32_t lbn = 0, size = 0;
    bool dir = false;
};

bool iso_open(const std::string &path);
bool iso_loaded();
// Busca una ruta del UMD ("/PSP_GAME/USRDIR/X.DFF", sin distinguir mayúsculas) o "sce_lbn0x.._size0x..".
bool iso_lookup(const std::string &path, IsoEntry *out);
std::vector<IsoEntry> iso_list(const IsoEntry &dir);
size_t iso_read(uint32_t lbn, uint64_t offset, void *dst, size_t len);
