// Volcado de las texturas originales y sustitución por versiones editadas, por hash de contenido.
// Las texturas se identifican por el hash de sus texels + CLUT + formato y tamaño (texture_hash), estable
// entre ejecuciones: --dump-textures escribe <dir>/<hash>.png con cada textura que aparece en pantalla y
// el fichero <textures>/<hash>.png la sustituye, al mismo tamaño o a mayor resolución (misma proporción).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

void texrep_init(const std::string &replace_dir, const std::string &dump_dir);
// Vuelve a leer la carpeta de sustituciones (recarga en caliente); devuelve cuántas hay.
int texrep_rescan();
// Carga la sustitución de una textura si existe.
bool texrep_load(uint64_t hash, std::vector<uint8_t> &rgba, int &w, int &h);
// Guarda la textura original (una vez por hash) si el volcado está activo.
void texrep_dump(uint64_t hash, const uint8_t *rgba, int w, int h);
std::string texrep_name(uint64_t hash);
// Escribe un PNG (depuración).
void texrep_write_png(const std::string &path, const uint8_t *rgba, int w, int h);
