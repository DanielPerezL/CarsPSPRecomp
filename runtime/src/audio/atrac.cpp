// sceAtrac3plus: música y voces ATRAC3/ATRAC3+ decodificadas con libavcodec.
// Modelo simplificado del streaming: el runtime guarda una copia de todo lo que el juego ha ido
// aportando del fichero; sceAtracGetStreamDataInfo pide siempre el siguiente tramo desde el principio
// del búfer del juego y sceAtracAddStreamData lo copia. Así no hace falta emular el búfer circular.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <vector>

#include "../threads.h"

namespace {

constexpr int MAX_IDS = 6;
constexpr uint32_t ERR_BAD_ID = 0x80630005, ERR_NO_DATA = 0x80630010, ERR_ALL_DECODED = 0x80630024,
                   ERR_UNKNOWN_FORMAT = 0x80630006;

struct Atrac {
    bool used = false;
    uint32_t buf = 0, buf_size = 0;
    std::vector<uint8_t> file;  // bytes recibidos del fichero (desde el offset 0)
    uint32_t file_size = 0, data_off = 0, data_end = 0, pos = 0;
    int channels = 2, block_align = 0, sample_rate = 44100;
    bool all_in_memory = false;
    AVCodecContext *ctx = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;

    void reset() {
        if (ctx) avcodec_free_context(&ctx);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        *this = Atrac();
    }
};

Atrac g_atrac[MAX_IDS];

uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

// Lee la cabecera RIFF/WAVE y prepara el decodificador.
uint32_t setup(Atrac &a) {
    const uint8_t *p = a.file.data();
    size_t n = a.file.size();
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return ERR_UNKNOWN_FORMAT;
    a.file_size = rd32(p + 4) + 8;
    uint16_t fmt = 0;
    std::vector<uint8_t> extra;
    for (size_t off = 12; off + 8 <= n;) {
        uint32_t size = rd32(p + off + 4);
        if (!memcmp(p + off, "fmt ", 4) && off + 8 + 16 <= n) {
            const uint8_t *f = p + off + 8;
            fmt = rd16(f);
            a.channels = rd16(f + 2);
            a.sample_rate = (int)rd32(f + 4);
            a.block_align = rd16(f + 12);
            if (size > 18) extra.assign(f + 18, f + std::min<size_t>(size, n - off - 8));
        } else if (!memcmp(p + off, "data", 4)) {
            a.data_off = (uint32_t)off + 8;
            a.data_end = a.data_off + size;
            break;
        }
        off += 8 + size + (size & 1);
    }
    if (!a.data_off || !a.block_align) return ERR_UNKNOWN_FORMAT;
    if (a.data_end > a.file_size) a.data_end = a.file_size;
    AVCodecID id = fmt == 0x0270 ? AV_CODEC_ID_ATRAC3 : AV_CODEC_ID_ATRAC3P;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) return ERR_UNKNOWN_FORMAT;
    a.ctx = avcodec_alloc_context3(codec);
    av_channel_layout_default(&a.ctx->ch_layout, a.channels);
    a.ctx->sample_rate = a.sample_rate;
    a.ctx->block_align = a.block_align;
    if (id == AV_CODEC_ID_ATRAC3 && !extra.empty()) {
        a.ctx->extradata = (uint8_t *)av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(a.ctx->extradata, extra.data(), extra.size());
        a.ctx->extradata_size = (int)extra.size();
    }
    if (avcodec_open2(a.ctx, codec, nullptr) < 0) return ERR_UNKNOWN_FORMAT;
    a.pkt = av_packet_alloc();
    a.frame = av_frame_alloc();
    a.pos = a.data_off;
    LOGD("atrac", "%s %d Hz %d canales, bloque %d, datos %u bytes", fmt == 0x0270 ? "ATRAC3" : "ATRAC3+",
         a.sample_rate, a.channels, a.block_align, a.data_end - a.data_off);
    return 0;
}

Atrac *get(uint32_t id) { return id < MAX_IDS && g_atrac[id].used ? &g_atrac[id] : nullptr; }

int remain_frames(const Atrac &a) {
    if (a.file.size() >= a.data_end) return a.all_in_memory ? -1 : -2;
    return (int)((a.file.size() > a.pos ? a.file.size() - a.pos : 0) / a.block_align);
}

}  // namespace

HLE(sceAtracSetDataAndGetID) {
    int id = 0;
    while (id < MAX_IDS && g_atrac[id].used) id++;
    if (id == MAX_IDS) RET(0x80630008);
    Atrac &a = g_atrac[id];
    a.reset();
    a.used = true;
    a.buf = A0;
    a.buf_size = A1;
    a.file.assign(MEMP(A0), MEMP(A0) + A1);
    uint32_t err = setup(a);
    if (err) {
        a.reset();
        RET(err);
    }
    if (a.file.size() > a.file_size) a.file.resize(a.file_size);
    a.all_in_memory = a.file.size() >= a.data_end;
    RET(id);
}

HLE(sceAtracReleaseAtracID) {
    if (Atrac *a = get(A0)) a->reset();
    RET(0);
}

HLE(sceAtracDecodeData) {  // (id, salida, *muestras, *fin, *frames restantes)
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    auto finish = [&](uint32_t samples, bool end, uint32_t ret) {
        if (A2) W32(A2, samples);
        if (A3) W32(A3, end);
        if (T0) W32(T0, (uint32_t)remain_frames(*a));
        RET(ret);
    };
    if (a->pos >= a->data_end) return finish(0, true, ERR_ALL_DECODED);
    if (a->pos + a->block_align > a->file.size()) return finish(0, false, ERR_NO_DATA);
    a->pkt->data = a->file.data() + a->pos;
    a->pkt->size = a->block_align;
    a->pos += a->block_align;
    uint32_t written = 0;
    if (avcodec_send_packet(a->ctx, a->pkt) == 0) {
        while (avcodec_receive_frame(a->ctx, a->frame) == 0) {
            int ch = a->frame->ch_layout.nb_channels;
            for (int i = 0; i < a->frame->nb_samples; i++) {
                for (int k = 0; k < 2; k++) {
                    int src = k < ch ? k : 0;
                    float v = a->frame->format == AV_SAMPLE_FMT_FLTP
                                  ? reinterpret_cast<float *>(a->frame->extended_data[src])[i]
                                  : reinterpret_cast<int16_t *>(a->frame->extended_data[0])[i * ch + src] / 32768.f;
                    int s = (int)(v * 32767.f);
                    W16(A1 + (written + i) * 4 + k * 2, (uint16_t)(int16_t)std::clamp(s, -32768, 32767));
                }
            }
            written += a->frame->nb_samples;
        }
    }
    return finish(written, a->pos >= a->data_end, 0);
}

HLE(sceAtracGetRemainFrame) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    if (A1) W32(A1, (uint32_t)remain_frames(*a));
    RET(0);
}

HLE(sceAtracGetStreamDataInfo) {  // (id, *dónde escribir, *bytes escribibles, *offset en el fichero)
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    uint32_t left = a->file_size > a->file.size() ? a->file_size - (uint32_t)a->file.size() : 0;
    if (A1) W32(A1, a->buf);
    if (A2) W32(A2, std::min(left, a->buf_size));
    if (A3) W32(A3, (uint32_t)a->file.size());
    RET(0);
}

HLE(sceAtracAddStreamData) {
    Atrac *a = get(A0);
    if (!a) RET(ERR_BAD_ID);
    a->file.insert(a->file.end(), MEMP(a->buf), MEMP(a->buf) + A1);
    RET(0);
}

HLE(sceAtracGetInternalErrorInfo) {
    if (A1) W32(A1, 0);
    RET(0);
}
