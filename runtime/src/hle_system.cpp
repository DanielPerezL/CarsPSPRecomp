// Servicios del sistema: memoria, pantalla, mandos, reloj, energía, módulos, utilidades y red.
#include <ctime>
#include <map>

#include "display.h"
#include "threads.h"

int next_uid();

// ---------------------------------------------------------------- SysMem
static std::map<int, uint32_t> g_mem_blocks;  // uid -> dirección

HLE(sceKernelAllocPartitionMemory) {
    // (partición, nombre, tipo 0=bajo 1=alto 2=dirección, tamaño, dirección)
    uint32_t addr = mem_alloc(A3, A2 == 1, mem_str(A1));
    if (!addr) RET(0x800200D9);
    int id = next_uid();
    g_mem_blocks[id] = addr;
    RET(id);
}
HLE(sceKernelFreePartitionMemory) {
    auto it = g_mem_blocks.find((int)A0);
    if (it == g_mem_blocks.end()) RET(0x800200CB);
    mem_free(it->second);
    g_mem_blocks.erase(it);
    RET(0);
}
HLE(sceKernelGetBlockHeadAddr) {
    auto it = g_mem_blocks.find((int)A0);
    RET(it == g_mem_blocks.end() ? 0 : it->second);
}
HLE(sceKernelSetCompiledSdkVersion) { RET(0); }
HLE(sceKernelSetCompilerVersion) { RET(0); }
HLE(sceKernelPrintf) {
    LOGI("printf", "%s", mem_str(A0));
    RET(0);
}

// ---------------------------------------------------------------- Utils
HLE(sceKernelDcacheWritebackAll) { RET(0); }
HLE(sceKernelDcacheWritebackRange) { RET(0); }
HLE(sceKernelLibcClock) { RET((uint32_t)time_us()); }
HLE(sceKernelLibcTime) {
    uint32_t t = (uint32_t)time(nullptr);
    if (A0) W32(A0, t);
    RET(t);
}
HLE(sceKernelLibcGettimeofday) {
    if (A0) {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        W32(A0, (uint32_t)ts.tv_sec);
        W32(A0 + 4, (uint32_t)(ts.tv_nsec / 1000));
    }
    RET(0);
}
HLE(sceKernelSetGPO) { RET(0); }
HLE(sceDmacMemcpy) {
    memmove(MEMP(A0), MEMP(A1), A2);
    RET(0);
}

// ---------------------------------------------------------------- Stdio
HLE(sceKernelStdin) { RET(0); }
HLE(sceKernelStdout) { RET(1); }
HLE(sceKernelStderr) { RET(2); }

// ---------------------------------------------------------------- Pantalla
HLE(sceDisplaySetMode) { RET(0); }
HLE(sceDisplaySetFrameBuf) {
    display_set_framebuf(A0, A1, A2);
    RET(0);
}
HLE(sceDisplayGetFrameBuf) {
    uint32_t addr, stride, fmt;
    display_get_framebuf(&addr, &stride, &fmt);
    if (A0) W32(A0, addr);
    if (A1) W32(A1, stride);
    if (A2) W32(A2, fmt);
    RET(0);
}
HLE(sceDisplayGetFramePerSec) {
    c->f[0] = 59.94f;
    RET(0);
}
HLE(sceDisplayWaitVblankStart) { RET(thread_wait(WaitType::Vblank, 0, 0, 0, false)); }
HLE(sceDisplayWaitVblank) { RET(thread_wait(WaitType::Vblank, 0, 0, 0, false)); }

// ---------------------------------------------------------------- Mandos
HLE(sceCtrlSetSamplingMode) { RET(0); }
HLE(sceCtrlPeekBufferPositive) {
    uint8_t *d = MEMP(A0);
    for (uint32_t i = 0; i < A1; i++) {
        uint8_t *e = d + i * 16;
        memset(e, 0, 16);
        uint32_t ts = (uint32_t)time_us();
        memcpy(e, &ts, 4);
        uint32_t buttons = display_buttons();
        if (buttons) LOGT("ctrl", "botones %04x", buttons);
        memcpy(e + 4, &buttons, 4);
        display_analog(&e[8], &e[9]);
    }
    RET(A1);
}

// ---------------------------------------------------------------- Reloj
static uint64_t rtc_tick() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // ticks (µs) desde 0001-01-01; el epoch Unix es 62135596800 s después
    return (uint64_t)(ts.tv_sec + 62135596800ULL) * 1000000ULL + ts.tv_nsec / 1000;
}
HLE(sceRtcGetCurrentTick) {
    uint64_t t = rtc_tick();
    W32(A0, (uint32_t)t);
    W32(A0 + 4, (uint32_t)(t >> 32));
    RET(0);
}
HLE(sceRtcGetCurrentClockLocalTime) {
    time_t now = time(nullptr);
    tm lt;
    localtime_r(&now, &lt);
    uint16_t v[6] = {(uint16_t)(lt.tm_year + 1900), (uint16_t)(lt.tm_mon + 1), (uint16_t)lt.tm_mday,
                     (uint16_t)lt.tm_hour, (uint16_t)lt.tm_min, (uint16_t)lt.tm_sec};
    memcpy(MEMP(A0), v, 12);
    W32(A0 + 12, 0);
    RET(0);
}
HLE(sceRtcGetTime_t) {
    uint64_t tick = (uint64_t)R32(A0) | ((uint64_t)R32(A0 + 8) << 32);
    (void)tick;
    W32(A1, (uint32_t)time(nullptr));
    RET(0);
}

// ---------------------------------------------------------------- Energía
HLE(scePowerRegisterCallback) { RET(0); }
HLE(scePowerUnregisterCallback) { RET(0); }
HLE(sceKernelPowerLock) { RET(0); }
HLE(sceKernelPowerUnlock) { RET(0); }
HLE(sceKernelPowerTick) { RET(0); }
static uint32_t g_volatile_mem = 0;
HLE(sceKernelVolatileMemTryLock) {
    // 4 MB de memoria volátil (0x08400000) disponibles para el juego
    if (A1) W32(A1, 0x08400000);
    if (A2) W32(A2, 0x00400000);
    g_volatile_mem = 1;
    RET(0);
}
HLE(sceKernelVolatileMemUnlock) {
    g_volatile_mem = 0;
    RET(0);
}

// ---------------------------------------------------------------- Módulos
// Los PRX de Sony (atrac, mpeg, fuentes...) los implementa el propio HLE: cargar = devolver un id.
HLE(sceKernelLoadModule) {
    LOGI("module", "LoadModule '%s'", mem_str(A0));
    RET(next_uid());
}
HLE(sceKernelStartModule) { RET(0); }
HLE(sceKernelStopModule) { RET(0); }
HLE(sceKernelUnloadModule) { RET(0); }
HLE(sceKernelSelfStopUnloadModule) { RET(0); }
HLE(sceKernelGetModuleId) { RET(1); }
HLE(sceKernelGetModuleIdByAddress) { RET(1); }
HLE(sceKernelRegisterExitCallback) { RET(0); }
HLE(sceKernelExitGame) { fatal("el juego ha pedido salir (sceKernelExitGame)"); }

// ---------------------------------------------------------------- Utilidades (diálogos, partidas)
HLE(sceUtilityGetSystemParamInt) {
    // 8 = idioma (3 = español), 9 = botón de confirmar (1 = X)
    uint32_t v = A0 == 8 ? 3 : A0 == 9 ? 1 : 0;
    W32(A1, v);
    RET(0);
}
HLE(sceUtilityGetSystemParamString) {
    strncpy(mem_str(A1), "PSP", A2);
    RET(0);
}
HLE(sceUtilityLoadNetModule) { RET(0); }
// ---------------------------------------------------------------- Red (multijugador ad hoc)
// Se inicializa correctamente pero nunca encuentra compañeros: el juego necesita que la pila de red
// exista (crea un singleton de red al arrancar) aunque no se use.
#define NET_OK(name) HLE(name) { RET(0); }
NET_OK(sceNetInit)
NET_OK(sceNetTerm)
NET_OK(sceNetAdhocInit)
NET_OK(sceNetAdhocTerm)
NET_OK(sceNetAdhocPdpDelete)
NET_OK(sceNetAdhocctlInit)
NET_OK(sceNetAdhocctlTerm)
NET_OK(sceNetAdhocctlScan)
NET_OK(sceNetAdhocctlDisconnect)
NET_OK(sceNetAdhocctlDelHandler)
NET_OK(sceNetAdhocMatchingInit)
NET_OK(sceNetAdhocMatchingTerm)
NET_OK(sceNetAdhocMatchingDelete)
NET_OK(sceNetAdhocMatchingStart)
NET_OK(sceNetAdhocMatchingStop)
NET_OK(sceNetAdhocMatchingSelectTarget)
NET_OK(sceNetAdhocMatchingCancelTarget)
NET_OK(sceNetAdhocMatchingSetHelloOpt)
HLE(sceNetAdhocPdpCreate) { RET(next_uid()); }
HLE(sceNetAdhocMatchingCreate) { RET(next_uid()); }
HLE(sceNetAdhocctlAddHandler) { RET(0); }
HLE(sceNetAdhocPdpSend) { RET(T0); }  // finge enviar todo
HLE(sceNetAdhocPdpRecv) { RET(0x80410709); }  // nada que recibir (would block)
HLE(sceNetAdhocctlConnect) { RET(0x80410B07); }
HLE(sceNetAdhocctlCreate) { RET(0x80410B07); }
HLE(sceNetAdhocctlJoin) { RET(0x80410B07); }
HLE(sceNetAdhocctlGetPeerList) {
    if (A0) W32(A0, 0);
    RET(0);
}
HLE(sceNetAdhocctlGetScanInfo) {
    if (A0) W32(A0, 0);
    RET(0);
}
HLE(sceNetEtherNtostr) {
    strcpy(mem_str(A1), "00:00:00:00:00:00");
    RET(0);
}
HLE(sceNetGetLocalEtherAddr) {
    memset(MEMP(A0), 0, 6);
    RET(0);
}
HLE(sceWlanGetSwitchState) { RET(0); }
HLE(sceOpenPSIDGetOpenPSID) {
    memset(MEMP(A0), 0x42, 16);
    RET(0);
}
