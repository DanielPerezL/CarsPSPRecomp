// Fuentes del sistema (sceLibFont, PGF): de momento lo mínimo para que el juego avance; el juego dibuja
// su texto con fuentes propias en textura. Los vídeos están en media/mpeg.cpp.
#include "threads.h"

int next_uid();

// ---------------------------------------------------------------- Fuentes (PGF): sin rasterizar aún
HLE(sceFontNewLib) {
    if (A1) W32(A1, 0);
    RET(next_uid());
}
HLE(sceFontDoneLib) { RET(0); }
HLE(sceFontFindOptimumFont) {
    if (A2) W32(A2, 0);
    RET(0);
}
HLE(sceFontOpen) {
    if (A3) W32(A3, 0);
    RET(next_uid());
}
HLE(sceFontOpenUserFile) {
    LOGD("font", "OpenUserFile '%s'", mem_str(A1));
    if (A3) W32(A3, 0);
    RET(next_uid());
}
HLE(sceFontClose) { RET(0); }
HLE(sceFontSetAltCharacterCode) { RET(0); }
HLE(sceFontGetFontInfo) {
    memset(MEMP(A1), 0, 264);
    RET(0);
}
HLE(sceFontGetCharInfo) {
    memset(MEMP(A2), 0, 60);
    RET(0);
}
HLE(sceFontGetCharGlyphImage_Clip) { RET(0); }
