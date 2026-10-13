// Audio, ATRAC3+, SAS, MPEG y fuentes.
// F0: el audio no suena todavía, pero las salidas bloqueantes respetan el tiempo real
// (los hilos de audio marcan el ritmo de parte del juego). Vídeo y fuentes: mínimos.
#include <map>

#include "threads.h"

int next_uid();

// ---------------------------------------------------------------- sceAudio
struct AudioChannel {
    bool reserved = false;
    uint32_t samples = 0, format = 0;
    uint64_t busy_until = 0;
};
static AudioChannel g_channels[8];

HLE(sceAudioChReserve) {
    int ch = (int)A0;
    if (ch < 0) {
        for (ch = 7; ch >= 0 && g_channels[ch].reserved; ch--) {}
        if (ch < 0) RET(0x80260002);
    }
    g_channels[ch] = {true, A1, A2, 0};
    RET(ch);
}
HLE(sceAudioChRelease) {
    if (A0 < 8) g_channels[A0].reserved = false;
    RET(0);
}
static void output(Ctx *c, int ch, bool blocking) {
    AudioChannel &a = g_channels[ch & 7];
    uint64_t now = time_us();
    if (a.busy_until > now) {
        if (!blocking) RET(0x80260009);  // canal ocupado
        thread_wait(WaitType::Delay, 0, a.busy_until - now, 0, false);
        now = time_us();
    }
    uint64_t dur = (uint64_t)a.samples * 1000000 / 44100;
    a.busy_until = std::max(now, a.busy_until) + dur;
    RET(a.samples);
}
HLE(sceAudioOutputBlocking) { output(c, (int)A0, true); }
HLE(sceAudioOutputPannedBlocking) { output(c, (int)A0, true); }
HLE(sceAudioOutputPanned) { output(c, (int)A0, false); }
HLE(sceAudioChangeChannelConfig) { RET(0); }
HLE(sceAudioChangeChannelVolume) { RET(0); }
HLE(sceAudioSetChannelDataLen) {
    g_channels[A0 & 7].samples = A1;
    RET(0);
}
HLE(sceAudioGetChannelRestLength) { RET(0); }

// ---------------------------------------------------------------- SAS (mezclador por hardware)
static uint32_t g_sas_grain = 256;
HLE(__sceSasInit) {
    g_sas_grain = A1;
    RET(0);
}
HLE(__sceSasCore) {
    memset(MEMP(A1), 0, g_sas_grain * 4);
    RET(0);
}
HLE(__sceSasCoreWithMix) { RET(0); }
HLE(__sceSasGetEndFlag) { RET(0xFFFFFFFF); }
HLE(__sceSasGetEnvelopeHeight) { RET(0); }
HLE(__sceSasGetPauseFlag) { RET(0); }
#define SAS_OK(name) HLE(name) { RET(0); }
SAS_OK(__sceSasRevEVOL)
SAS_OK(__sceSasRevParam)
SAS_OK(__sceSasRevType)
SAS_OK(__sceSasRevVON)
SAS_OK(__sceSasSetADSR)
SAS_OK(__sceSasSetADSRmode)
SAS_OK(__sceSasSetKeyOff)
SAS_OK(__sceSasSetKeyOn)
SAS_OK(__sceSasSetNoise)
SAS_OK(__sceSasSetPause)
SAS_OK(__sceSasSetPitch)
SAS_OK(__sceSasSetSimpleADSR)
SAS_OK(__sceSasSetSL)
SAS_OK(__sceSasSetVoice)
SAS_OK(__sceSasSetVolume)

// ---------------------------------------------------------------- ATRAC3+ (música)
static int g_next_atrac = 0;
HLE(sceAtracSetDataAndGetID) { RET(g_next_atrac++ & 3); }
HLE(sceAtracReleaseAtracID) { RET(0); }
HLE(sceAtracDecodeData) {
    // (id, salida, *muestras, *fin, *frames restantes): sin decodificador aún -> fin inmediato
    if (A2) W32(A2, 0);
    if (A3) W32(A3, 1);
    if (T0) W32(T0, 0);
    RET(0);
}
HLE(sceAtracGetRemainFrame) {
    if (A1) W32(A1, 0xFFFFFFFF);
    RET(0);
}
HLE(sceAtracGetStreamDataInfo) {
    if (A1) W32(A1, 0);
    if (A2) W32(A2, 0);
    if (A3) W32(A3, 0);
    RET(0);
}
HLE(sceAtracAddStreamData) { RET(0); }
HLE(sceAtracGetInternalErrorInfo) {
    if (A1) W32(A1, 0);
    RET(0);
}

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
