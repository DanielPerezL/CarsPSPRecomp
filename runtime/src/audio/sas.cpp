// sceSasCore: mezclador por hardware de la PSP (32 voces PS-ADPCM/PCM con envolvente ADSR).
// Adaptado de PPSSPP (Core/HW/SasAudio.cpp y Core/HLE/sceSas.cpp, GPL-2.0+): decodificación VAG,
// envolventes, remuestreo por tono con interpolación lineal y mezcla con volúmenes de 12 bits.
// No implementa la reverberación (efecto "wet"); el sonido seco es el principal.
#include <algorithm>
#include <cstdint>

#include "../threads.h"

namespace {

constexpr int VOICES = 32, PITCH_BASE = 0x1000, PITCH_MASK = 0xFFF, PITCH_SHIFT = 12;
constexpr int64_t ENV_MAX = 0x40000000;

enum Curve { LINEAR_INC, LINEAR_DEC, LINEAR_BENT, EXP_DEC, EXP_INC, DIRECT };
enum EnvState { ATTACK, DECAY, SUSTAIN, RELEASE, OFF, KEYON, KEYON_STEP };

inline int16_t clamp16(int v) { return (int16_t)std::clamp(v, -32768, 32767); }

int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 26) >> (n >> 2);
    return r ? r : 1;
}
int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = ((7 - (n & 3)) << 24) >> (n >> 2);
    return r ? r : 1;
}

struct Envelope {
    int attack_rate = 0, decay_rate = 0, sustain_rate = 0, release_rate = 0;
    int attack_type = LINEAR_INC, decay_type = LINEAR_DEC, sustain_type = LINEAR_DEC, release_type = LINEAR_DEC;
    int64_t sustain_level = 0x100;
    int64_t height = 0;
    int state = OFF;

    void set_simple(uint32_t e1, uint32_t e2) {
        attack_rate = simple_rate(e1 >> 8);
        attack_type = (e1 & 0x8000) ? LINEAR_BENT : LINEAR_INC;
        int dn = e1 >> 4 & 0xF;
        decay_rate = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
        decay_type = EXP_DEC;
        sustain_type = e2 >> 14 & 3;
        sustain_rate = sustain_type == EXP_DEC ? exponent_rate(e2 >> 6) : simple_rate(e2 >> 6);
        release_type = (e2 & 0x20) ? EXP_DEC : LINEAR_DEC;
        int rn = e2 & 0x1F;
        if (rn == 31) release_rate = 0;
        else if (release_type == LINEAR_DEC) release_rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (0x10000000 >> rn);
        else release_rate = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
        sustain_level = (int64_t)((e1 & 0xF) + 1) << 26;
    }

    void walk(int type, int rate) {
        int64_t d;
        switch (type) {
        case LINEAR_INC: height += rate; break;
        case LINEAR_DEC: height -= rate; break;
        case LINEAR_BENT: height += height <= ENV_MAX * 3 / 4 ? rate : rate / 4; break;
        case EXP_DEC:
            d = height - ENV_MAX;
            d += (-d * (int64_t)(uint32_t)rate) >> 32;
            height = d + ENV_MAX - ((uint64_t)rate + 3) / 4;
            break;
        case EXP_INC:
            d = height - ENV_MAX;
            d += (-d * (int64_t)(uint32_t)rate) >> 32;
            height = d + 0x4000 + ENV_MAX;
            break;
        case DIRECT: height = rate; break;
        }
    }
    void set_state(int s) {
        if (height > ENV_MAX) height = ENV_MAX;
        state = s;
    }
    void step() {
        switch (state) {
        case ATTACK: walk(attack_type, attack_rate); if (height >= ENV_MAX || height < 0) set_state(DECAY); break;
        case DECAY: walk(decay_type, decay_rate); if (height < sustain_level) set_state(SUSTAIN); break;
        case SUSTAIN: walk(sustain_type, sustain_rate); if (height <= 0) { height = 0; set_state(RELEASE); } break;
        case RELEASE: walk(release_type, release_rate); if (height <= 0) { height = 0; set_state(OFF); } break;
        case KEYON: height = 0; set_state(KEYON_STEP); break;
        case KEYON_STEP: if (++height >= 31) { height = 0; set_state(ATTACK); } break;
        default: break;
        }
    }
    int get() const { return (int)(height > ENV_MAX ? ENV_MAX : height); }
    bool needs_keyon() const { return state == KEYON; }
};

// Decodificador PS-ADPCM (VAG) con bucles por flags de bloque.
struct Vag {
    uint32_t data = 0, read = 0;
    int num_blocks = 0, cur_block = -1, loop_start = -1, cur_sample = 28;
    int s1 = 0, s2 = 0;
    bool loop_enabled = false, loop_next = false, end = false;
    int16_t samples[28] = {};

    void start(uint32_t addr, uint32_t size, bool loop) {
        *this = Vag();
        data = read = addr;
        num_blocks = size / 16;
        loop_enabled = loop;
    }
    void decode_block() {
        static const int F[16][2] = {{0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0}, {52, 0},
                                     {55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151}};
        if (cur_block == num_blocks - 1) { end = true; return; }
        const uint8_t *p = MEMP(read);
        int pred = p[0] >> 4, shift = p[0] & 0xF, flags = p[1];
        if (flags == 7) { end = true; return; }
        if (flags == 6) loop_start = cur_block;
        else if (flags == 3 && loop_enabled) loop_next = true;
        int c1 = F[pred][0], c2 = -F[pred][1];
        int a = s1, b = s2;
        for (int i = 0; i < 28; i += 2) {
            uint8_t d = p[2 + i / 2];
            int x1 = (int16_t)((d & 0xF) << 12) >> shift;
            int x2 = (int16_t)((d & 0xF0) << 8) >> shift;
            b = clamp16(x1 + ((a * c1 + b * c2) >> 6));
            a = clamp16(x2 + ((b * c1 + a * c2) >> 6));
            samples[i] = (int16_t)b;
            samples[i + 1] = (int16_t)a;
        }
        s1 = a;
        s2 = b;
        cur_sample = 0;
        cur_block++;
        read += 16;
    }
    void get(int16_t *out, int n) {
        for (int i = 0; i < n; i++) {
            if (end) { std::fill(out + i, out + n, 0); return; }
            if (cur_sample == 28) {
                if (loop_next) {
                    read = data + 16 * loop_start + 16;
                    cur_block = loop_start;
                    loop_next = false;
                }
                decode_block();
                if (end) { std::fill(out + i, out + n, 0); return; }
            }
            out[i] = samples[cur_sample++];
        }
    }
};

struct Voice {
    bool playing = false, paused = false, on = false;
    bool pcm = false;
    uint32_t vag_addr = 0, vag_size = 0;
    bool loop = false;
    uint32_t pcm_addr = 0;
    int pcm_size = 0, pcm_index = 0, pcm_loop = 0;
    int pitch = PITCH_BASE;
    uint32_t frac = 0;
    int vol_l = 0x1000, vol_r = 0x1000, fx_l = 0x1000, fx_r = 0x1000;
    int16_t hist[2] = {0, 0};
    Envelope env;
    Vag vag;

    void key_on() {
        env.set_state(KEYON);
        if (!pcm) vag.start(vag_addr, vag_size, loop);
        playing = on = true;
        paused = false;
        frac = 0;
    }
    void key_off() {
        on = false;
        env.set_state(RELEASE);
    }
    void read(int16_t *out, int n) {
        if (!pcm) { vag.get(out, n); return; }
        int i = 0;
        while (i < n && on) {
            int m = std::min(pcm_size - pcm_index, n - i);
            memcpy(out + i, MEMP(pcm_addr + pcm_index * 2), m * 2);
            pcm_index += m;
            i += m;
            if (pcm_index >= pcm_size) {
                if (!loop) break;
                pcm_index = pcm_loop;
            }
        }
        std::fill(out + i, out + n, 0);
    }
    bool ended() const { return pcm ? pcm_index >= pcm_size : vag.end; }
};

struct Sas {
    int grain = 256, max_voices = 32, output_mode = 0;
    Voice voices[VOICES];
    int32_t mix[2048 * 2] = {};
    int16_t tmp[2048 * 4 + 4] = {};

    void mix_voice(Voice &v) {
        if (!v.pcm && !v.vag_addr) return;
        if (v.pcm && !v.pcm_addr) return;
        int delay = 0;
        if (v.env.needs_keyon()) delay = ((32 * (uint32_t)v.pitch) >> PITCH_SHIFT) + (v.pcm ? 0 : 1);
        tmp[0] = v.hist[0];
        tmp[1] = v.hist[1];
        uint32_t frac = v.frac;
        int to_read = (int)((frac + (uint32_t)v.pitch * std::max(0, grain - delay)) >> PITCH_SHIFT);
        to_read = std::min(to_read, (int)(sizeof(tmp) / 2) - 4);
        int pos = 2;
        if (v.env.needs_keyon()) { pos = 0; to_read += 2; }
        v.read(&tmp[pos], to_read);
        int end = pos + to_read;
        for (int i = 0; i < delay; i++) v.env.step();
        bool interp = v.pitch != PITCH_BASE || (frac & PITCH_MASK);
        for (int i = delay; i < grain; i++) {
            const int16_t *s = tmp + (frac >> PITCH_SHIFT);
            int sample = s[0];
            if (interp) sample = s[0] - (((s[0] - s[1]) * (int)(frac & PITCH_MASK)) >> PITCH_SHIFT);
            frac += v.pitch;
            int e = (v.env.get() + (1 << 14)) >> 15;
            v.env.step();
            sample = (sample * e + (1 << 14)) >> 15;
            mix[i * 2] += (sample * v.vol_l) >> 12;
            mix[i * 2 + 1] += (sample * v.vol_r) >> 12;
        }
        v.hist[0] = tmp[end - 2];
        v.hist[1] = tmp[end - 1];
        v.frac = frac - (end - 2) * PITCH_BASE;
        if (v.ended()) { v.env.set_state(OFF); v.env.height = 0; }
        if (v.env.state == OFF) v.playing = v.on = false;
    }

    void run(uint32_t out, uint32_t in, int lv, int rv) {
        memset(mix, 0, sizeof(int32_t) * grain * 2);
        for (auto &v : voices)
            if (v.playing && !v.paused) mix_voice(v);
        for (int i = 0; i < grain * 2; i += 2) {
            int l = mix[i], r = mix[i + 1];
            if (in) {
                l += ((int16_t)R16(in + i * 2) * lv) >> 12;
                r += ((int16_t)R16(in + i * 2 + 2) * rv) >> 12;
            }
            W16(out + i * 2, (uint16_t)clamp16(l));
            W16(out + i * 2 + 2, (uint16_t)clamp16(r));
        }
    }
};

Sas g_sas;

Voice *voice(Ctx *c) {
    int v = (int)A1;
    return v >= 0 && v < VOICES ? &g_sas.voices[v] : nullptr;
}
constexpr uint32_t ERR_VOICE = 0x80420010;

}  // namespace

HLE(__sceSasInit) {
    g_sas = Sas();
    g_sas.grain = std::min<int>((int)A1, 2048);
    g_sas.max_voices = (int)A2;
    g_sas.output_mode = (int)A3;
    for (auto &v : g_sas.voices) v.env.set_simple(0x000F, 0x1F00);
    LOGI("sas", "init grain=%d voces=%d modo=%d", g_sas.grain, g_sas.max_voices, g_sas.output_mode);
    RET(0);
}
HLE(__sceSasCore) {
    g_sas.run(A1, 0, 0, 0);
    RET(0);
}
HLE(__sceSasCoreWithMix) {
    g_sas.run(A1, A1, (int)A2, (int)A3);
    RET(0);
}
HLE(__sceSasGetEndFlag) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++)
        if (!g_sas.voices[i].playing) f |= 1u << i;
    RET(f);
}
HLE(__sceSasGetPauseFlag) {
    uint32_t f = 0;
    for (int i = 0; i < VOICES; i++)
        if (g_sas.voices[i].paused) f |= 1u << i;
    RET(f);
}
HLE(__sceSasSetPause) {
    for (int i = 0; i < VOICES; i++)
        if (A1 >> i & 1) g_sas.voices[i].paused = A2 != 0;
    RET(0);
}
HLE(__sceSasSetVoice) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    v->pcm = false;
    v->vag_addr = A2;
    v->vag_size = A3;
    v->loop = T0 != 0;
    RET(0);
}
HLE(__sceSasSetVolume) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    v->vol_l = (int)A2;
    v->vol_r = (int)A3;
    v->fx_l = (int)T0;
    v->fx_r = (int)T1;
    RET(0);
}
HLE(__sceSasSetPitch) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    v->pitch = std::clamp((int)A2, 0, 0x4000);
    RET(0);
}
HLE(__sceSasSetKeyOn) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    if (!v->paused) v->key_on();
    RET(0);
}
HLE(__sceSasSetKeyOff) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    if (v->playing && !v->paused) v->key_off();
    RET(0);
}
HLE(__sceSasSetNoise) { RET(0); }
HLE(__sceSasSetSL) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    v->env.sustain_level = A2;
    RET(0);
}
HLE(__sceSasSetADSR) {  // (core, voz, flags, a, d, s, r)
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    if (A2 & 1) v->env.attack_rate = (int)A3;
    if (A2 & 2) v->env.decay_rate = (int)T0;
    if (A2 & 4) v->env.sustain_rate = (int)T1;
    if (A2 & 8) v->env.release_rate = (int)T2;
    RET(0);
}
HLE(__sceSasSetADSRmode) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    if (A2 & 1) v->env.attack_type = (int)A3;
    if (A2 & 2) v->env.decay_type = (int)T0;
    if (A2 & 4) v->env.sustain_type = (int)T1;
    if (A2 & 8) v->env.release_type = (int)T2;
    RET(0);
}
HLE(__sceSasSetSimpleADSR) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    v->env.set_simple(A2, A3);
    RET(0);
}
HLE(__sceSasGetEnvelopeHeight) {
    Voice *v = voice(c);
    if (!v) RET(ERR_VOICE);
    RET(v->env.get());
}
HLE(__sceSasRevType) { RET(0); }
HLE(__sceSasRevParam) { RET(0); }
HLE(__sceSasRevEVOL) { RET(0); }
HLE(__sceSasRevVON) { RET(0); }
