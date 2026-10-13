// Sustitución de texturas (p. ej. reescaladas con IA) y volcado de las originales, por hash de contenido.
// Las texturas se identifican por el hash de sus texels + CLUT + formato y tamaño (texture_hash), estable
// entre ejecuciones; el fichero <dir>/<hash>.png sustituye a la original a cualquier resolución.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

void texrep_init(const std::string &replace_dir, const std::string &dump_dir);
// Carga la sustitución de una textura si existe.
bool texrep_load(uint64_t hash, std::vector<uint8_t> &rgba, int &w, int &h);
// Guarda la textura original (una vez por hash) si el volcado está activo.
void texrep_dump(uint64_t hash, const uint8_t *rgba, int w, int h);
std::string texrep_name(uint64_t hash);
