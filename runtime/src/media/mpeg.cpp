// sceMpeg: vídeos PSMF (logos y cinemáticas) con libavcodec.
//
// El juego lee el fichero a través de un ring buffer cuyo callback MIPS rellena paquetes de 2048 bytes.
// Cada paquete se copia a un búfer del host y se demultiplexa (MPEG-PS propio, incremental) en vídeo
// H.264 y audio ATRAC3+; GetAvcAu/GetAtracAu entregan unidades de acceso con su PTS y AvcDecode/
// AtracDecode las decodifican a la memoria del juego. Como el host guarda los datos, el ring buffer
// se da siempre por vacío y el juego puede leer al ritmo que quiera.
// Estructuras y formato del audio en PSMF según PPSSPP (Core/HLE/sceMpeg.cpp, Core/HW/MpegDemux.cpp).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <deque>
#include <map>
#include <vector>

#include "../threads.h"

int next_uid();

namespace {

constexpr uint32_t MPEG_NO_DATA = 0x80618001;
constexpr int PACKET = 2048;

// SceMpegRingBuffer
constexpr uint32_t RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_DATA = 20, RB_CALLBACK = 24,
                   RB_ARG = 28;
// SceMpegAu: pts (alto, bajo), dts (alto, bajo), esBuffer, esSize
constexpr uint32_t AU_PTS_HI = 0, AU_PTS_LO = 4, AU_DTS_HI = 8, AU_DTS_LO = 12;

struct Unit {
    std::vector<uint8_t> data;
    int64_t pts = -1;
};

struct Mpeg {
    uint32_t ringbuf = 0;
    int pixel_mode = 3;
    // demultiplexado
    std::vector<uint8_t> ps;  // datos MPEG-PS pendientes de analizar
    std::vector<uint8_t> audio_es;
    int64_t audio_pts = -1;
    std::deque<Unit> video, audio;
    int64_t last_video_pts = -3003;
    // decodificación
    AVCodecParserContext *parser = nullptr;
    AVCodecContext *vctx = nullptr, *actx = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    Unit cur_video, cur_audio;
    bool ended = false;

    ~Mpeg() {
        if (parser) av_parser_close(parser);
        if (vctx) avcodec_free_context(&vctx);
        if (actx) avcodec_free_context(&actx);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
    }
};

std::map<uint32_t, Mpeg> g_mpegs;  // por dirección del manejador
std::map<uint32_t, uint32_t> g_ring_owner;  // ring buffer -> mpeg

Mpeg *get(uint32_t handle) {
    auto it = g_mpegs.find(handle);
    return it == g_mpegs.end() ? nullptr : &it->second;
}

int64_t read_pts(const uint8_t *p) {
    return ((int64_t)(p[0] & 0x0E) << 29) | (p[1] << 22) | ((p[2] & 0xFE) << 14) | (p[3] << 7) | (p[4] >> 1);
}

// Vídeo: el ES H.264 se trocea en unidades de acceso con el parser de libavcodec.
void push_video(Mpeg &m, const uint8_t *data, int size, int64_t pts) {
    while (size > 0) {
        uint8_t *out;
        int out_size;
        int used = av_parser_parse2(m.parser, m.vctx, &out, &out_size, data, size, pts, pts, 0);
        pts = AV_NOPTS_VALUE;
        data += used;
        size -= used;
        if (out_size) {
            // Solo algunos PES traen PTS: los que faltan se deducen a 29,97 fps (3003 ticks de 90 kHz).
            int64_t fp = m.parser->pts != AV_NOPTS_VALUE ? m.parser->pts : m.last_video_pts + 3003;
            m.last_video_pts = fp;
            m.video.push_back({std::vector<uint8_t>(out, out + out_size), fp});
        }
    }
}

// Audio: tramas ATRAC3+ con cabecera de 8 bytes (0x0F 0xD0 ...) que codifica su tamaño.
void push_audio(Mpeg &m, const uint8_t *data, int size, int64_t pts) {
    if (m.audio_es.empty() && pts >= 0) m.audio_pts = pts;
    m.audio_es.insert(m.audio_es.end(), data, data + size);
    size_t pos = 0;
    while (pos + 8 <= m.audio_es.size()) {
        if (m.audio_es[pos] != 0x0F || m.audio_es[pos + 1] != 0xD0) { pos++; continue; }
        int frame = (((m.audio_es[pos + 2] & 3) << 8) | (m.audio_es[pos + 3] * 8)) + 0x10;
        if (pos + frame > m.audio_es.size()) break;
        m.audio.push_back({std::vector<uint8_t>(m.audio_es.begin() + pos + 8, m.audio_es.begin() + pos + frame),
                           m.audio_pts});
        if (m.audio_pts >= 0) m.audio_pts += 2048 * 90000 / 44100;
        pos += frame;
    }
    m.audio_es.erase(m.audio_es.begin(), m.audio_es.begin() + pos);
}

// Analiza paquetes completos de MPEG-PS.
void demux(Mpeg &m) {
    size_t i = 0;
    std::vector<uint8_t> &b = m.ps;
    while (i + 6 <= b.size()) {
        if (b[i] || b[i + 1] || b[i + 2] != 1) { i++; continue; }
        uint8_t code = b[i + 3];
        if (code == 0xBA) {  // pack header
            if (i + 14 > b.size()) break;
            i += 14 + (b[i + 13] & 7);
            continue;
        }
        if (code == 0xB9) { i += 4; continue; }  // fin de programa
        size_t len = (b[i + 4] << 8) | b[i + 5];
        if (i + 6 + len > b.size()) break;
        const uint8_t *p = &b[i + 6], *end = p + len;
        if (code == 0xE0 || code == 0xBD) {
            int64_t pts = AV_NOPTS_VALUE;
            if ((p[0] & 0xC0) == 0x80) {  // cabecera PES de MPEG-2
                if (p[1] & 0x80) pts = read_pts(p + 3);
                p += 3 + p[2];
            }
            if (code == 0xE0) {
                push_video(m, p, (int)(end - p), pts);
            } else if (p < end) {
                uint8_t channel = *p++;
                p += 3;
                if (channel >= 0xB0 && channel <= 0xBF) p++;
                if (p < end) push_audio(m, p, (int)(end - p), pts == AV_NOPTS_VALUE ? -1 : pts);
            }
        }
        i += 6 + len;
    }
    b.erase(b.begin(), b.begin() + i);
}

void write_au(uint32_t au, int64_t pts) {
    if (pts < 0) pts = 0;
    W32(au + AU_PTS_HI, (uint32_t)(pts >> 32));
    W32(au + AU_PTS_LO, (uint32_t)pts);
    W32(au + AU_DTS_HI, (uint32_t)(pts >> 32));
    W32(au + AU_DTS_LO, (uint32_t)pts);
}

inline uint8_t clamp8(int v) { return (uint8_t)std::clamp(v, 0, 255); }

// YUV 4:2:0 (BT.601) -> formato de píxel de la PSP en la memoria del juego.
void write_frame(const AVFrame *f, uint32_t dst, int stride, int mode) {
    int w = std::min(f->width, 480), h = std::min(f->height, 272);
    for (int y = 0; y < h; y++) {
        const uint8_t *Y = f->data[0] + y * f->linesize[0];
        const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
        const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
        for (int x = 0; x < w; x++) {
            int c = Y[x] - 16, d = U[x / 2] - 128, e = V[x / 2] - 128;
            uint8_t r = clamp8((298 * c + 409 * e + 128) >> 8);
            uint8_t g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            uint8_t bl = clamp8((298 * c + 516 * d + 128) >> 8);
            uint32_t off = dst + (y * stride + x) * (mode == 3 ? 4 : 2);
            if (mode == 3) W32(off, r | g << 8 | bl << 16 | 0xFF000000u);
            else if (mode == 0) W16(off, (uint16_t)((r >> 3) | (g >> 2) << 5 | (bl >> 3) << 11));
            else if (mode == 1) W16(off, (uint16_t)((r >> 3) | (g >> 3) << 5 | (bl >> 3) << 10 | 0x8000));
            else W16(off, (uint16_t)((r >> 4) | (g >> 4) << 4 | (bl >> 4) << 8 | 0xF000));
        }
    }
}

}  // namespace

HLE(sceMpegInit) { RET(0); }
HLE(sceMpegFinish) { RET(0); }
HLE(sceMpegQueryMemSize) { RET(0x10000); }
HLE(sceMpegRingbufferQueryMemSize) { RET(A0 * (PACKET + 104)); }

HLE(sceMpegRingbufferConstruct) {  // (ring, paquetes, datos, tamaño, callback, arg)
    uint32_t rb = A0;
    memset(MEMP(rb), 0, 48);
    W32(rb + RB_PACKETS, A1);
    W32(rb + 16, PACKET);
    W32(rb + RB_DATA, A2);
    W32(rb + RB_CALLBACK, T0);
    W32(rb + RB_ARG, T1);
    RET(0);
}
HLE(sceMpegRingbufferDestruct) { RET(0); }

HLE(sceMpegCreate) {  // (mpeg, datos, tamaño, ring, anchoFrame, modo, ddrtop)
    g_mpegs.erase(A0);
    Mpeg &m = g_mpegs[A0];
    m.ringbuf = A3;
    g_ring_owner[A3] = A0;
    W32(A3 + 40, A0);
    const AVCodec *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    m.vctx = avcodec_alloc_context3(h264);
    avcodec_open2(m.vctx, h264, nullptr);
    m.parser = av_parser_init(AV_CODEC_ID_H264);
    const AVCodec *at3 = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    m.actx = avcodec_alloc_context3(at3);
    av_channel_layout_default(&m.actx->ch_layout, 2);
    m.actx->sample_rate = 44100;
    m.pkt = av_packet_alloc();
    m.frame = av_frame_alloc();
    LOGD("mpeg", "create %08x ring=%08x", A0, A3);
    RET(0);
}
HLE(sceMpegDelete) {
    g_mpegs.erase(A0);
    RET(0);
}
HLE(sceMpegAvcDecodeMode) {
    if (Mpeg *m = get(A0)) {
        if (A1) m->pixel_mode = (int)R32(A1 + 4);
    }
    RET(0);
}
HLE(sceMpegRegistStream) { RET(next_uid()); }
HLE(sceMpegUnRegistStream) { RET(0); }
HLE(sceMpegMallocAvcEsBuf) { RET(1); }
HLE(sceMpegFreeAvcEsBuf) { RET(0); }
HLE(sceMpegInitAu) {
    write_au(A2, -1);
    W32(A2 + 16, A1);
    W32(A2 + 20, 0);
    RET(0);
}
HLE(sceMpegQueryAtracEsSize) {
    if (A1) W32(A1, 2112);
    if (A2) W32(A2, 8192);
    RET(0);
}
HLE(sceMpegQueryStreamOffset) {  // (mpeg, cabecera PSMF, *offset)
    W32(A2, __builtin_bswap32(R32(A1 + 8)));
    RET(0);
}
HLE(sceMpegQueryStreamSize) {  // (cabecera PSMF, *tamaño)
    W32(A1, __builtin_bswap32(R32(A0 + 12)));
    RET(0);
}

HLE(sceMpegRingbufferAvailableSize) { RET(R32(A0 + RB_PACKETS) - R32(A0 + RB_AVAIL)); }

HLE(sceMpegRingbufferPut) {  // (ring, paquetes, disponibles): el callback del juego lee del disco
    uint32_t rb = A0;
    int want = std::min((int)A1, (int)A2);
    auto owner = g_ring_owner.find(rb);
    Mpeg *m = owner == g_ring_owner.end() ? nullptr : get(owner->second);
    int total = 0;
    while (want > 0) {
        uint32_t packets = R32(rb + RB_PACKETS), pos = R32(rb + RB_WRITEPOS);
        int chunk = std::min<int>(want, (int)(packets - pos));
        uint32_t dst = R32(rb + RB_DATA) + pos * PACKET;
        int got = (int)call_mips(c, R32(rb + RB_CALLBACK), {dst, (uint32_t)chunk, R32(rb + RB_ARG)});
        if (got <= 0) break;
        if (m) {
            m->ps.insert(m->ps.end(), MEMP(dst), MEMP(dst) + got * PACKET);
            demux(*m);
        }
        W32(rb + RB_WRITEPOS, (pos + got) % packets);
        W32(rb + RB_READ, R32(rb + RB_READ) + got);
        total += got;
        want -= got;
        if (got < chunk) break;
    }
    RET(total);
}

HLE(sceMpegGetAvcAu) {  // (mpeg, stream, au, *attr)
    Mpeg *m = get(A0);
    if (!m || m->video.empty()) RET(MPEG_NO_DATA);
    m->cur_video = std::move(m->video.front());
    m->video.pop_front();
    write_au(A2, m->cur_video.pts);
    if (A3) W32(A3, 1);
    RET(0);
}

HLE(sceMpegAvcDecode) {  // (mpeg, au, anchoFrame, *puntero al búfer, *frame listo)
    Mpeg *m = get(A0);
    if (!m) RET(0x80618005);
    bool got = false;
    if (!m->cur_video.data.empty()) {
        m->pkt->data = m->cur_video.data.data();
        m->pkt->size = (int)m->cur_video.data.size();
        if (avcodec_send_packet(m->vctx, m->pkt) == 0 && avcodec_receive_frame(m->vctx, m->frame) == 0) {
            write_frame(m->frame, R32(A3), (int)A2, m->pixel_mode);
            got = true;
            static int n = 0;
            if (const char *dbg = getenv("CARS_DEBUG_MPEG"); dbg && ++n % 30 == 0) {  // fotograma cada segundo
                char path[256];
                snprintf(path, sizeof(path), "%s/mpeg_%04d.ppm", dbg, n);
                if (FILE *f = fopen(path, "wb")) {
                    fprintf(f, "P6\n480 272\n255\n");
                    for (int y = 0; y < 272; y++)
                        for (int x = 0; x < 480; x++) fwrite(MEMP(R32(A3) + (y * A2 + x) * 4), 1, 3, f);
                    fclose(f);
                }
            }
        }
        m->cur_video.data.clear();
    }
    if (T0) W32(T0, got);
    LOGT("mpeg", "AvcDecode buf=%08x ancho=%u frame=%d (cola vídeo %zu, audio %zu, ps %zu)", R32(A3), A2, got,
         m->video.size(), m->audio.size(), m->ps.size());
    RET(0);
}
HLE(sceMpegAvcDecodeStop) {
    if (A3) W32(A3, 0);
    RET(0);
}

HLE(sceMpegGetAtracAu) {
    Mpeg *m = get(A0);
    if (!m || m->audio.empty()) RET(MPEG_NO_DATA);
    m->cur_audio = std::move(m->audio.front());
    m->audio.pop_front();
    write_au(A2, m->cur_audio.pts);
    if (A3) W32(A3, 0);
    RET(0);
}

HLE(sceMpegAtracDecode) {  // (mpeg, au, búfer de salida, init)
    Mpeg *m = get(A0);
    memset(MEMP(A2), 0, 8192);
    if (!m || m->cur_audio.data.empty()) RET(0);
    if (!avcodec_is_open(m->actx)) {
        m->actx->block_align = (int)m->cur_audio.data.size();
        avcodec_open2(m->actx, avcodec_find_decoder(AV_CODEC_ID_ATRAC3P), nullptr);
    }
    m->pkt->data = m->cur_audio.data.data();
    m->pkt->size = (int)m->cur_audio.data.size();
    if (avcodec_send_packet(m->actx, m->pkt) == 0 && avcodec_receive_frame(m->actx, m->frame) == 0) {
        int n = std::min(m->frame->nb_samples, 2048), ch = m->frame->ch_layout.nb_channels;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 2; k++) {
                float v = reinterpret_cast<float *>(m->frame->extended_data[k < ch ? k : 0])[i];
                W16(A2 + i * 4 + k * 2, (uint16_t)(int16_t)std::clamp((int)(v * 32767.f), -32768, 32767));
            }
    }
    m->cur_audio.data.clear();
    RET(0);
}

HLE(sceMpegFlushAllStream) {
    if (Mpeg *m = get(A0)) {
        m->ps.clear();
        m->audio_es.clear();
        m->video.clear();
        m->audio.clear();
        avcodec_flush_buffers(m->vctx);
    }
    RET(0);
}
