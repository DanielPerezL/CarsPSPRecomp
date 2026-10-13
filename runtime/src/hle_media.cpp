// Vídeo (sceMpeg, PMF) y fuentes (sceLibFont, PGF): de momento solo lo mínimo para que el juego avance
// (los vídeos se saltan y el juego dibuja el texto con sus propias fuentes en textura).
#include "threads.h"

int next_uid();

// ---------------------------------------------------------------- MPEG (vídeos PMF): sin decodificar aún
static uint32_t be32(uint32_t addr) { return __builtin_bswap32(R32(addr)); }
HLE(sceMpegInit) { RET(0); }
HLE(sceMpegFinish) { RET(0); }
HLE(sceMpegQueryMemSize) { RET(0x10000); }
HLE(sceMpegCreate) { RET(0); }
HLE(sceMpegDelete) { RET(0); }
HLE(sceMpegRingbufferQueryMemSize) { RET(A0 * 2048); }
HLE(sceMpegRingbufferConstruct) { RET(0); }
HLE(sceMpegRingbufferDestruct) { RET(0); }
HLE(sceMpegRingbufferAvailableSize) { RET(0); }
HLE(sceMpegRingbufferPut) { RET(0); }
HLE(sceMpegQueryStreamOffset) {
    W32(A2, be32(A1 + 8));
    RET(0);
}
HLE(sceMpegQueryStreamSize) {
    W32(A1, be32(A0 + 12));
    RET(0);
}
HLE(sceMpegRegistStream) { RET(next_uid()); }
HLE(sceMpegUnRegistStream) { RET(0); }
HLE(sceMpegMallocAvcEsBuf) { RET(1); }
HLE(sceMpegFreeAvcEsBuf) { RET(0); }
HLE(sceMpegInitAu) { RET(0); }
HLE(sceMpegQueryAtracEsSize) {
    if (A1) W32(A1, 2112);
    if (A2) W32(A2, 8192);
    RET(0);
}
HLE(sceMpegGetAvcAu) { RET(0x80618001); }   // sin más datos
HLE(sceMpegGetAtracAu) { RET(0x80618001); }
HLE(sceMpegAvcDecode) { RET(0x80628002); }
HLE(sceMpegAvcDecodeMode) { RET(0); }
HLE(sceMpegAvcDecodeStop) { RET(0); }
HLE(sceMpegAtracDecode) { RET(0); }
HLE(sceMpegFlushAllStream) { RET(0); }

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
