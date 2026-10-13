// sceAudio: 8 canales de salida de la PSP mezclados en un dispositivo SDL (44,1 kHz, estéreo, s16).
// Las salidas bloqueantes esperan mientras el canal tenga demasiadas muestras en cola: así el propio
// consumo del dispositivo marca el ritmo de los hilos de audio del juego, sin derivas.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "../threads.h"

namespace {

constexpr int RATE = 44100;
constexpr size_t MAX_QUEUED = 2048 * 2;  // muestras (L+R) en cola por canal antes de bloquear

struct Channel {
    bool reserved = false;
    uint32_t samples = 0;
    bool mono = false;
    int vol_l = 0x8000, vol_r = 0x8000;
    std::deque<int16_t> queue;
};

Channel g_ch[8];
std::mutex g_mutex;
SDL_AudioDeviceID g_dev = 0;
bool g_enabled = false;
FILE *g_record = nullptr;  // --record-audio: copia de la mezcla final en WAV
uint32_t g_recorded = 0;

void finish_record() {
    if (!g_record) return;
    uint32_t data = g_recorded, riff = 36 + data, rate = RATE, brate = RATE * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(g_record, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_record); fwrite(&riff, 4, 1, g_record); fwrite("WAVEfmt ", 1, 8, g_record);
    fwrite(&fmt, 4, 1, g_record); fwrite(&pcm, 2, 1, g_record); fwrite(&ch, 2, 1, g_record);
    fwrite(&rate, 4, 1, g_record); fwrite(&brate, 4, 1, g_record); fwrite(&align, 2, 1, g_record);
    fwrite(&bits, 2, 1, g_record); fwrite("data", 1, 4, g_record); fwrite(&data, 4, 1, g_record);
    fclose(g_record);
    g_record = nullptr;
}

void callback(void *, Uint8 *stream, int len) {
    int16_t *out = reinterpret_cast<int16_t *>(stream);
    int n = len / 2;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < n; i++) {
        int acc = 0;
        for (auto &c : g_ch) {
            if (c.queue.empty()) continue;
            acc += c.queue.front();
            c.queue.pop_front();
        }
        out[i] = (int16_t)std::clamp(acc, -32768, 32767);
    }
    if (g_record) {
        fwrite(stream, 1, len, g_record);
        g_recorded += len;
    }
}

void push(Channel &c, uint32_t addr, int vl, int vr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < c.samples; i++) {
        int16_t l, r;
        if (c.mono) {
            l = r = (int16_t)R16(addr + i * 2);
        } else {
            l = (int16_t)R16(addr + i * 4);
            r = (int16_t)R16(addr + i * 4 + 2);
        }
        c.queue.push_back((int16_t)((l * vl) >> 15));
        c.queue.push_back((int16_t)((r * vr) >> 15));
    }
}

size_t queued(Channel &c) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return c.queue.size();
}

// Envía un bloque al canal; si blocking, espera a que haya sitio (como sceAudioOutputBlocking).
void output(Ctx *c, int ch, int vl, int vr, uint32_t buf, bool blocking) {
    Channel &a = g_ch[ch & 7];
    if (!a.reserved) RET(0x80260008);
    if (!g_enabled) {  // sin dispositivo (headless): solo se respeta el tiempo
        uint64_t us = (uint64_t)a.samples * 1000000 / RATE;
        if (blocking) thread_wait(WaitType::Delay, 0, us, 0, false);
        RET(a.samples);
    }
    while (queued(a) > MAX_QUEUED) {
        if (!blocking) RET(0x80260009);  // canal ocupado
        thread_wait(WaitType::Delay, 0, 2000, 0, false);
    }
    if (buf) push(a, buf, vl, vr);
    RET(a.samples);
}

}  // namespace

void audio_init(bool enabled, const std::string &record_path) {
    if (!enabled) return;
    if (!record_path.empty() && (g_record = fopen(record_path.c_str(), "wb"))) {
        fseek(g_record, 44, SEEK_SET);
        atexit(finish_record);
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOGW("audio", "sin audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        LOGW("audio", "no se pudo abrir el dispositivo de audio: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    g_enabled = true;
    LOGI("audio", "salida de audio %d Hz", have.freq);
}

HLE(sceAudioChReserve) {
    int ch = (int)A0;
    if (ch < 0) {
        for (ch = 7; ch >= 0 && g_ch[ch].reserved; ch--) {}
        if (ch < 0) RET(0x80260002);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ch[ch].reserved = true;
    g_ch[ch].samples = A1;
    g_ch[ch].mono = A2 == 0x10;
    g_ch[ch].queue.clear();
    RET(ch);
}
HLE(sceAudioChRelease) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (A0 < 8) {
        g_ch[A0].reserved = false;
        g_ch[A0].queue.clear();
    }
    RET(0);
}
HLE(sceAudioOutputBlocking) { output(c, (int)A0, (int)A1, (int)A1, A2, true); }
HLE(sceAudioOutputPannedBlocking) { output(c, (int)A0, (int)A1, (int)A2, A3, true); }
HLE(sceAudioOutputPanned) { output(c, (int)A0, (int)A1, (int)A2, A3, false); }
HLE(sceAudioChangeChannelConfig) {
    g_ch[A0 & 7].mono = A1 == 0x10;
    RET(0);
}
HLE(sceAudioChangeChannelVolume) {
    g_ch[A0 & 7].vol_l = (int)A1;
    g_ch[A0 & 7].vol_r = (int)A2;
    RET(0);
}
HLE(sceAudioSetChannelDataLen) {
    g_ch[A0 & 7].samples = A1;
    RET(0);
}
HLE(sceAudioGetChannelRestLength) { RET(queued(g_ch[A0 & 7]) / 2); }
