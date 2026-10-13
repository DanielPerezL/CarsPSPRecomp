// Renderizador OpenGL 3.3 core de la GE: emula la tubería fija de la PSP a resolución interna escalada
// y, opcionalmente, genera fotogramas intermedios interpolando entre fotogramas lógicos del juego.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core.h"
#include "../platform/window.h"
#include "../ui/overlay.h"
#include "gl_funcs.h"
#include "renderer.h"
#include "texture.h"
#include "texture_replace.h"

namespace {

// Valor de 16 bits de un píxel guardado como RGBA8 en un framebuffer de la PSP de 16 bits (565, 5551, 4444).
// Se trunca igual que la GE al escribir en 16 bits.
#define RAW16_GLSL \
    "uint raw16(vec4 c, int fmt) {\n" \
    "    uvec4 v = uvec4(c * 255.0 + 0.5);\n" \
    "    if (fmt == 0) return v.r >> 3 | (v.g >> 2) << 5 | (v.b >> 3) << 11;\n" \
    "    if (fmt == 1) return v.r >> 3 | (v.g >> 3) << 5 | (v.b >> 3) << 10 | (v.a >> 7) << 15;\n" \
    "    return v.r >> 4 | (v.g >> 4) << 4 | (v.b >> 4) << 8 | (v.a >> 4) << 12;\n" \
    "}\n"

const char *VS = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_col;

uniform int u_through;
uniform mat4 u_world, u_view, u_proj;
uniform vec3 u_vpScale, u_vpOff;
uniform vec2 u_fbSize;
uniform vec4 u_uvScaleOff;
uniform vec2 u_texSize;
uniform vec2 u_uvFix;  // framebuffer como textura: tamaño de la textura de la PSP / tamaño del framebuffer

uniform int u_lighting, u_matUpdate, u_lightMode;
uniform vec4 u_matAmbient;     // rgb + alfa
uniform vec3 u_matEmissive, u_matDiffuse, u_matSpecular;
uniform float u_specPower;
uniform vec4 u_ambient;
uniform int u_lightOn[4], u_lightType[4], u_lightComp[4];
uniform vec3 u_lightPos[4], u_lightDir[4], u_lightAtt[4];
uniform float u_spotExp[4], u_spotCut[4];
uniform vec3 u_lightAmb[4], u_lightDiff[4], u_lightSpec[4];
uniform vec2 u_fogCoef;

out vec4 v_col0;
out vec3 v_col1;
out vec2 v_uv;
out float v_fog;

void main() {
    if (u_through == 1) {
        gl_Position = vec4(a_pos.x / u_fbSize.x * 2.0 - 1.0, 1.0 - a_pos.y / u_fbSize.y * 2.0,
                           a_pos.z / 65535.0 * 2.0 - 1.0, 1.0);
        v_col0 = a_col;
        v_col1 = vec3(0.0);
        v_uv = a_uv / u_texSize;
        v_fog = 1.0;
        return;
    }
    vec3 wpos = (u_world * vec4(a_pos, 1.0)).xyz;
    vec4 vpos = u_view * vec4(wpos, 1.0);
    vec4 clip = u_proj * vpos;
    vec3 px = clip.xyz / clip.w * u_vpScale + u_vpOff;
    vec3 ndc = vec3(px.x / u_fbSize.x * 2.0 - 1.0, 1.0 - px.y / u_fbSize.y * 2.0, px.z / 65535.0 * 2.0 - 1.0);
    gl_Position = vec4(ndc * clip.w, clip.w);
    v_uv = (a_uv * u_uvScaleOff.xy + u_uvScaleOff.zw) * u_uvFix;
    v_fog = clamp((vpos.z + u_fogCoef.x) * u_fogCoef.y, 0.0, 1.0);

    if (u_lighting == 0) {
        v_col0 = a_col;
        v_col1 = vec3(0.0);
        return;
    }
    vec3 n = normalize(mat3(u_world) * a_nrm);
    vec4 matAmb = (u_matUpdate & 1) != 0 ? a_col : u_matAmbient;
    vec3 matDiff = (u_matUpdate & 2) != 0 ? a_col.rgb : u_matDiffuse;
    vec3 matSpec = (u_matUpdate & 4) != 0 ? a_col.rgb : u_matSpecular;
    vec3 c0 = u_matEmissive + u_ambient.rgb * matAmb.rgb;
    vec3 c1 = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        if (u_lightOn[i] == 0) continue;
        vec3 toL;
        float att = 1.0;
        if (u_lightType[i] == 0) {
            toL = normalize(u_lightPos[i]);
        } else {
            toL = u_lightPos[i] - wpos;
            float d = length(toL);
            toL /= max(d, 1e-6);
            att = clamp(1.0 / (u_lightAtt[i].x + u_lightAtt[i].y * d + u_lightAtt[i].z * d * d), 0.0, 1.0);
            if (u_lightType[i] == 2) {
                float s = dot(normalize(u_lightDir[i]), -toL);
                att *= s >= u_spotCut[i] ? pow(max(s, 0.0), u_spotExp[i]) : 0.0;
            }
        }
        float ndl = dot(n, toL);
        float diff = max(ndl, 0.0);
        if (u_lightComp[i] == 2) diff = pow(diff, u_specPower);
        c0 += (u_lightAmb[i] * matAmb.rgb + u_lightDiff[i] * matDiff * diff) * att;
        if (u_lightComp[i] == 1 && ndl >= 0.0) {
            vec3 h = normalize(toL + vec3(0.0, 0.0, 1.0));
            c1 += u_lightSpec[i] * matSpec * pow(max(dot(n, h), 0.0), u_specPower) * att;
        }
    }
    if (u_lightMode == 0) { c0 += c1; c1 = vec3(0.0); }
    v_col0 = clamp(vec4(c0, matAmb.a * u_ambient.a), 0.0, 1.0);
    v_col1 = clamp(c1, 0.0, 1.0);
}
)";

const char *FS = R"(#version 330 core
in vec4 v_col0;
in vec3 v_col1;
in vec2 v_uv;
in float v_fog;
out vec4 o_color;

uniform sampler2D u_tex;
uniform int u_texOn, u_texFunc, u_texRGBA, u_colorDouble, u_texFlipY;
// Framebuffer leído como textura con CLUT: 0 = no (RGBA tal cual), 1 = píxeles de 16 bits (CLUT16),
// 2 = profundidad de 16 bits (CLUT16), 3 = píxeles de 32 bits (CLUT32).
uniform int u_texClut, u_srcFmt;
uniform sampler2D u_clut;          // 512x1, ya convertida a RGBA
uniform ivec3 u_clutIdx;           // desplazamiento, máscara, inicio
uniform int u_texFb, u_fbScale, u_fbLinear;  // textura = framebuffer a u_fbScale x la resolución de la PSP
uniform vec2 u_texSize;
uniform vec3 u_envColor;
uniform int u_alphaTest, u_alphaFunc, u_alphaRef, u_alphaMask;
uniform int u_fog;
uniform vec3 u_fogColor;
uniform int u_clear;
uniform vec3 u_srcMul;  // factor fijo de la fuente cuando la mezcla usa dos colores fijos distintos


)" RAW16_GLSL R"(
vec4 apply_clut(vec4 t) {
    uvec4 b = uvec4(t * 255.0 + 0.5);
    uint raw = u_texClut == 1 ? raw16(t, u_srcFmt)
             : u_texClut == 2 ? uint(t.r * 65535.0 + 0.5)
             : b.r | b.g << 8 | b.b << 16 | b.a << 24;
    uint i = ((raw >> uint(u_clutIdx.x)) & uint(u_clutIdx.y)) | uint(u_clutIdx.z);
    return texelFetch(u_clut, ivec2(int(i & 511u), 0), 0);
}

// Píxel q (de la PSP) del framebuffer: el centro de su bloque escalado, con la CLUT aplicada.
vec4 fb_texel(ivec2 q) {
    q = clamp(q, ivec2(0), ivec2(u_texSize) - 1);
    return apply_clut(texelFetch(u_tex, q * u_fbScale + u_fbScale / 2, 0));
}

// Framebuffer como textura, filtrado como en la PSP: con CLUT (índices o profundidad) se filtra el color ya
// convertido entre píxeles de la PSP; sin CLUT, al reducir se lee el mipmap de la resolución de la PSP
// (un punto de cada bloque escalado centelleaba con la cámara en movimiento).
vec4 sample_fb(vec2 uv) {
    vec2 p = uv * u_texSize;
    if (u_texClut != 0) {
        if (u_fbLinear == 0) return fb_texel(ivec2(floor(p)));
        p -= 0.5;
        ivec2 i = ivec2(floor(p));
        vec2 f = p - vec2(i);
        return mix(mix(fb_texel(i), fb_texel(i + ivec2(1, 0)), f.x),
                   mix(fb_texel(i + ivec2(0, 1)), fb_texel(i + ivec2(1, 1)), f.x), f.y);
    }
    float m = max(length(dFdx(p)), length(dFdy(p))) * float(u_fbScale);
    return textureLod(u_tex, uv, m > 1.01 ? log2(float(u_fbScale)) : 0.0);
}

bool cmp(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}

void main() {
    vec4 c = v_col0;
    if (u_clear == 0 && u_texOn == 1) {
        vec2 uv = u_texFlipY == 1 ? vec2(v_uv.x, 1.0 - v_uv.y) : v_uv;
        vec4 t = u_texFb == 1 ? sample_fb(uv) : texture(u_tex, uv);
        vec3 rgb;
        float a = c.a;
        if (u_texFunc == 0) { rgb = c.rgb * t.rgb; if (u_texRGBA == 1) a = c.a * t.a; }
        else if (u_texFunc == 1) { rgb = u_texRGBA == 1 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }
        else if (u_texFunc == 2) { rgb = mix(c.rgb, u_envColor, t.rgb); if (u_texRGBA == 1) a = c.a * t.a; }
        else if (u_texFunc == 3) { rgb = t.rgb; if (u_texRGBA == 1) a = t.a; }
        else { rgb = c.rgb + t.rgb; if (u_texRGBA == 1) a = c.a * t.a; }
        if (u_colorDouble == 1) rgb *= 2.0;
        c = vec4(clamp(rgb + v_col1, 0.0, 1.0), a);
    } else if (u_clear == 0) {
        c.rgb = clamp(c.rgb + v_col1, 0.0, 1.0);
    }
    if (u_clear == 0 && u_alphaTest == 1) {
        int ia = int(c.a * 255.0 + 0.5);
        if (!cmp(u_alphaFunc, ia & u_alphaMask, u_alphaRef & u_alphaMask)) discard;
    }
    if (u_clear == 0 && u_fog == 1) c.rgb = mix(u_fogColor, c.rgb, v_fog);
    o_color = vec4(c.rgb * u_srcMul, c.a);
}
)";

// Reinterpretación de un framebuffer al cambiar de formato con el mismo ancho en bytes (como en la VRAM):
// dos píxeles de 16 bits forman uno de 32 (bytes R G | B A) y al revés. Trabaja a la resolución escalada.
const char *REFMT_VS = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char *REFMT_FS = "#version 330 core\n" RAW16_GLSL R"(
uniform sampler2D u_src;
uniform int u_srcFmt, u_dstFmt, u_scale;
out vec4 o_color;

vec4 from16(uint r, int fmt) {
    if (fmt == 0) return vec4(float(r & 31u) / 31.0, float(r >> 5 & 63u) / 63.0, float(r >> 11) / 31.0, 1.0);
    if (fmt == 1) return vec4(vec3(r & 31u, r >> 5 & 31u, r >> 10 & 31u) / 31.0, float(r >> 15));
    return vec4(r & 15u, r >> 4 & 15u, r >> 8 & 15u, r >> 12) / 15.0;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int x = p.x / u_scale, sub = p.x % u_scale;
    if (u_dstFmt == 3 && u_srcFmt != 3) {
        uint lo = raw16(texelFetch(u_src, ivec2(2 * x * u_scale + sub, p.y), 0), u_srcFmt);
        uint hi = raw16(texelFetch(u_src, ivec2((2 * x + 1) * u_scale + sub, p.y), 0), u_srcFmt);
        o_color = vec4(lo & 255u, lo >> 8, hi & 255u, hi >> 8) / 255.0;
    } else if (u_srcFmt == 3 && u_dstFmt != 3) {
        uvec4 v = uvec4(texelFetch(u_src, ivec2(x / 2 * u_scale + sub, p.y), 0) * 255.0 + 0.5);
        o_color = from16((x & 1) == 0 ? v.r | v.g << 8 : v.b | v.a << 8, u_dstFmt);
    } else {
        vec4 c = texelFetch(u_src, p, 0);
        o_color = u_srcFmt == u_dstFmt ? c : from16(raw16(c, u_srcFmt), u_dstFmt);
    }
}
)";

// Instantánea del estado de la GE necesaria para ejecutar un dibujo (mismos accesores que GeState).
struct Snap {
    uint32_t cmd[256];
    float world[12], view[12], proj[16];
    GLuint tex = 0;
    float tex_w = 1, tex_h = 1;
    bool tex_hd = false;
    // Framebuffer como textura: se resuelve al ejecutar (su formato y su textura de GL pueden cambiar).
    int tex_fb = 0;           // 0 = no, 1 = color, 2 = profundidad
    uint32_t tex_fb_addr = 0;  // dirección del framebuffer o del búfer de profundidad
    GLuint clut = 0;          // CLUT para leer el framebuffer como CLUT16/CLUT32

    uint32_t arg(int c) const { return cmd[c] & 0xFFFFFF; }
    bool through() const { return cmd[0x12] >> 23 & 1; }
    float f24(int c) const {
        uint32_t v = cmd[c] << 8;
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    void color(int c, float out[3]) const {
        uint32_t v = arg(c);
        out[0] = (v & 0xFF) / 255.f;
        out[1] = (v >> 8 & 0xFF) / 255.f;
        out[2] = (v >> 16 & 0xFF) / 255.f;
    }
    uint32_t fb_addr() const { return arg(0x9C) & 0x1FFFF0; }
};

struct DrawCmd {
    Snap snap;
    GpuPrim prim;
    std::vector<GpuVertex> verts;
    uint64_t key;
};

struct Frame {
    std::vector<DrawCmd> cmds;
    uint64_t t_us = 0;
    uint32_t display = 0, stride = 512, fmt = 3;
};

struct Target {
    uint32_t addr = 0;
    int w = 480, h = 272;      // en píxeles de la PSP; w = stride
    int fmt = 3, stride = 512;  // formato (0-2 = 16 bits, 3 = 8888) y stride con los que se dibujó por última vez
    uint32_t zaddr = 0;        // búfer de profundidad usado en el último dibujo
    GLuint fbo = 0, color = 0, depth = 0;
    int depth_w = 0;           // ancho de la textura de profundidad (píxeles de la PSP)
    GLuint views[2] = {};      // texturas de color para 16 y 32 bits (se alterna al cambiar de formato)
    int view_w[2] = {};
    bool cpu_written = false;  // la CPU/DMA lo ha sobrescrito desde el último dibujo de la GE (vídeos)
    bool mips_dirty = true;    // mipmaps de la vista de color desactualizados
};

struct CachedTex {
    GLuint tex = 0;
    uint64_t hash = 0;
    int w = 0, h = 0;
    bool replaced = false;  // sustituida por una versión de mayor resolución (con mipmaps)
};

RendererConfig g_cfg;
GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
std::unordered_map<std::string, GLint> g_loc;
std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint32_t, uint32_t> g_depth_owner;  // búfer de profundidad -> último framebuffer que lo usó
struct ClutTex { GLuint tex = 0; uint64_t used = 0; };
std::unordered_map<uint64_t, ClutTex> g_cluts;         // CLUT convertida a RGBA, por contenido
uint64_t g_frame_no = 0;
GLuint g_refmt_prog = 0, g_refmt_vao = 0, g_copy_fbo = 0;
struct Scratch { GLuint tex = 0; int w = 0, h = 0; };
Scratch g_scratch;  // copia de un framebuffer que se usa como textura de sí mismo
std::unordered_map<uint64_t, CachedTex> g_textures;
Target *g_presented = nullptr;
float g_max_aniso = 0;
std::string g_pending_shot;
GLuint g_cpu_tex = 0, g_cpu_fbo = 0;
std::vector<uint8_t> g_rgba;

// interpolación
Frame g_recording, g_prev, g_last;
bool g_have_prev = false, g_have_last = false;
uint64_t g_present_interval = 0, g_last_present = 0;
uint64_t g_stat_presents = 0, g_stat_flips = 0, g_stat_t0 = 0;

void count_stats(bool flip) {
    (flip ? g_stat_flips : g_stat_presents)++;
    uint64_t now = time_us();
    if (now - g_stat_t0 >= 2000000) {
        double s = (now - g_stat_t0) / 1e6;
        LOGI("gpu", "%.0f fps presentados, %.0f fotogramas lógicos/s", g_stat_presents / s, g_stat_flips / s);
        g_stat_presents = g_stat_flips = 0;
        g_stat_t0 = now;
    }
}
std::vector<GpuVertex> g_lerp_verts;

GLint U(const char *name) {
    auto it = g_loc.find(name);
    if (it != g_loc.end()) return it->second;
    GLint l = glGetUniformLocation(g_prog, name);
    g_loc[name] = l;
    return l;
}

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fatal("error compilando shader: %s", log);
    }
    return s;
}

void mat43_to_44(const float *m, float *o) {
    for (int c = 0; c < 4; c++) {
        o[c * 4 + 0] = m[c * 3 + 0];
        o[c * 4 + 1] = m[c * 3 + 1];
        o[c * 4 + 2] = m[c * 3 + 2];
        o[c * 4 + 3] = c == 3 ? 1.f : 0.f;
    }
}

GLuint new_texture(GLint ifmt, int w, int h, GLenum fmt, GLenum type) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return tex;
}

int stride_width(uint32_t stride) { return std::clamp<int>(stride, 16, 1024); }

// Asocia al FBO la textura de color de la vista (16 o 32 bits) y, si hace falta, una profundidad más ancha.
// En GL 3.3 los adjuntos pueden tener tamaños distintos: se dibuja en la intersección.
void attach_view(Target &t, int view, bool clear) {
    int S = g_cfg.scale;
    if (!t.views[view] || t.view_w[view] != t.w) {
        if (t.views[view]) glDeleteTextures(1, &t.views[view]);
        t.views[view] = new_texture(GL_RGBA8, t.w * S, t.h * S, GL_RGBA, GL_UNSIGNED_BYTE);
        t.view_w[view] = t.w;
        clear = true;
    }
    t.color = t.views[view];
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
    bool new_depth = t.depth_w < t.w;
    if (new_depth) {
        // Profundidad como textura: el juego la lee (0x04600000 + dirección) para la profundidad de campo.
        if (t.depth) glDeleteTextures(1, &t.depth);
        t.depth = new_texture(GL_DEPTH24_STENCIL8, t.w * S, t.h * S, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        t.depth_w = t.w;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("FBO incompleto");
    if (clear || new_depth) {
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClearColor(0, 0, 0, 0);
        glClearDepth(0);
        glClearStencil(0);
        glClear((clear ? GL_COLOR_BUFFER_BIT : 0) | (new_depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : 0));
    }
}

Target &target_for(uint32_t addr, uint32_t stride, uint32_t scissor_br, int fmt) {
    Target &t = g_targets[addr];
    if (t.fbo) return t;
    t.addr = addr;
    int sy2 = (scissor_br >> 10 & 0x3FF) + 1;
    t.fmt = fmt;
    t.stride = stride;
    t.w = stride_width(stride);
    t.h = stride >= 480 ? 272 : std::min(std::max(sy2, 16), 512);
    glGenFramebuffers(1, &t.fbo);
    attach_view(t, fmt == 3, true);
    LOGI("gpu", "framebuffer %06x: %dx%d fmt %d (interno %dx%d)", addr, t.w, t.h, fmt, t.w * g_cfg.scale,
         t.h * g_cfg.scale);
    return t;
}

// El juego dibuja en la misma dirección con otro formato o stride. Si el ancho en bytes coincide (p. ej.
// 512 píxeles de 16 bits = 256 de 32), los bytes de la VRAM son los mismos y se reinterpretan: la
// profundidad de campo escribe así en 16 bits lo que luego es el alfa de una imagen de 32.
void reformat_target(Target &t, int fmt, uint32_t stride) {
    bool was32 = t.fmt == 3, is32 = fmt == 3;
    bool keep = (uint32_t)t.stride * (was32 ? 4 : 2) == stride * (is32 ? 4 : 2);
    GLuint src = t.color;
    int old_fmt = t.fmt;
    t.fmt = fmt;
    t.stride = stride;
    t.w = stride_width(stride);
    // Mismo tamaño de píxel (565 <-> 5551/4444, casi inexistente): se conservan los colores tal cual.
    if (keep && was32 == is32) return;
    attach_view(t, is32, !keep);
    if (!keep) return;
    int S = g_cfg.scale;
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w * S, t.h * S);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(g_refmt_prog);
    glBindVertexArray(g_refmt_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(glGetUniformLocation(g_refmt_prog, "u_srcFmt"), old_fmt);
    glUniform1i(glGetUniformLocation(g_refmt_prog, "u_dstFmt"), fmt);
    glUniform1i(glGetUniformLocation(g_refmt_prog, "u_scale"), S);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
}

// Copia de un framebuffer para usarlo como textura mientras se dibuja en él (el desenfoque de la
// profundidad de campo suma desplazada su propia imagen); en GL leer y escribir a la vez no está definido.
GLuint self_copy(const Target &t) {
    int S = g_cfg.scale, w = t.w * S, h = t.h * S;
    if (g_scratch.w != w || g_scratch.h != h) {
        if (g_scratch.tex) glDeleteTextures(1, &g_scratch.tex);
        g_scratch.tex = new_texture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
        g_scratch.w = w;
        g_scratch.h = h;
        glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scratch.tex, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return g_scratch.tex;
}

// CLUT del dibujo convertida a RGBA (512 entradas, el máximo índice tras desplazamiento, máscara e inicio).
GLuint clut_texture(const uint8_t *clut, uint32_t clutfmt) {
    uint64_t h = 1469598103934665603ULL ^ (clutfmt & 3);
    for (int i = 0; i < 2048; i++) h = (h ^ clut[i]) * 1099511628211ULL;
    ClutTex &c = g_cluts[h];
    c.used = g_frame_no;
    if (c.tex) return c.tex;
    uint8_t rgba[512 * 4];
    for (uint32_t i = 0; i < 512; i++) clut_color(clut, (clutfmt & 3) | 0xFF00 | (i >> 4 & 0x1F) << 16, i & 0xFF, rgba + i * 4);
    c.tex = new_texture(GL_RGBA8, 512, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return c.tex;
}

GLenum blend_factor(int f, bool src, GLenum fix) {
    switch (f) {
    case 0: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default: return fix;
    }
}

GLenum fix_factor(uint32_t color) {
    if ((color & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
    if ((color & 0xFFFFFF) == 0) return GL_ZERO;
    return GL_CONSTANT_COLOR;
}

// Resuelve (y sube si ha cambiado) la textura del estado actual de la GE.
void resolve_texture(Snap &s) {
    TexParams p;
    p.addr = g_ge.tex_addr(0);
    p.bufw = g_ge.tex_bufw(0);
    p.w = g_ge.tex_w(0);
    p.h = g_ge.tex_h(0);
    p.format = g_ge.arg(0xC3) & 0xF;
    p.swizzled = g_ge.arg(0xC2) & 1;
    p.clutfmt = g_ge.arg(0xC5);
    if (p.format > 10) p.format = 3;
    // ¿Render a textura? La textura es un framebuffer que hemos dibujado o, en los espejos 0x04200000 y
    // 0x04600000 de la VRAM, el búfer de profundidad. Se resuelve al ejecutar el dibujo.
    if ((p.addr & 0x0F000000) == 0x04000000) {
        uint32_t off = p.addr & 0x1FFFF0;
        bool depth = (p.addr & 0x00200000) && g_depth_owner.count(off);
        if (depth || g_targets.count(off)) {
            s.tex_fb = depth ? 2 : 1;
            s.tex_fb_addr = off;
            s.tex = 0;
            s.clut = p.format == 6 || p.format == 7 ? clut_texture(g_ge.clut, p.clutfmt) : 0;
            return;
        }
    }
    uint64_t key = (uint64_t)p.addr << 32 ^ (uint64_t)p.format << 28 ^ (uint64_t)p.w << 16 ^ (uint64_t)p.h << 4 ^
                   (p.clutfmt * 0x9E3779B97F4A7C15ULL);
    CachedTex &ct = g_textures[key];
    uint64_t h = texture_hash(p, g_ge.clut);
    if (!ct.tex || ct.hash != h) {
        // El nombre de la textura (volcado y sustitución) es el hash de la imagen decodificada, que también
        // calcula tools/textures/export.py leyendo los ficheros del juego.
        int w = p.w, hh = p.h;
        texture_decode(p, g_ge.clut, g_rgba);
        uint64_t name = texrep_content_hash(g_rgba.data(), p.w, p.h);
        bool replaced = texrep_load(name, g_rgba, w, hh);
        if (!replaced) texrep_dump(name, g_rgba.data(), p.w, p.h);
        if (!ct.tex) glGenTextures(1, &ct.tex);
        glBindTexture(GL_TEXTURE_2D, ct.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba.data());
        if (replaced) {
            // Mipmaps solo hasta la resolución original: la PSP no los tenía, y reducir más mezcla el alfa
            // de zonas transparentes (p. ej. la barra de nitro, un quad muy estrecho, se veía como una
            // franja semitransparente; la vegetación con test de alfa se desvanecería a lo lejos).
            int levels = 0;
            while ((p.w << (levels + 1)) <= w && (p.h << (levels + 1)) <= hh) levels++;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels);
            if (g_max_aniso > 1) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, g_max_aniso);
            glGenerateMipmap(GL_TEXTURE_2D);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        ct.hash = h;
        ct.w = w;
        ct.h = hh;
        ct.replaced = replaced;
    }
    s.tex = ct.tex;
    s.tex_hd = ct.replaced;
    s.tex_w = (float)p.w;
    s.tex_h = (float)p.h;
}

void set_light_uniforms(const Snap &g) {
    float v[3];
    glUniform1i(U("u_lighting"), g.arg(0x17) & 1);
    glUniform1i(U("u_matUpdate"), g.arg(0x53) & 7);
    glUniform1i(U("u_lightMode"), g.arg(0x5E) & 1);
    g.color(0x55, v);
    glUniform4f(U("u_matAmbient"), v[0], v[1], v[2], (g.arg(0x58) & 0xFF) / 255.f);
    g.color(0x54, v);
    glUniform3fv(U("u_matEmissive"), 1, v);
    g.color(0x56, v);
    glUniform3fv(U("u_matDiffuse"), 1, v);
    g.color(0x57, v);
    glUniform3fv(U("u_matSpecular"), 1, v);
    glUniform1f(U("u_specPower"), g.f24(0x5B));
    g.color(0x5C, v);
    glUniform4f(U("u_ambient"), v[0], v[1], v[2], (g.arg(0x5D) & 0xFF) / 255.f);
    int on[4], type[4], comp[4];
    float pos[12], dir[12], att[12], sexp[4], scut[4], amb[12], diff[12], spec[12];
    for (int i = 0; i < 4; i++) {
        on[i] = g.arg(0x18 + i) & 1;
        comp[i] = g.arg(0x5F + i) & 3;
        type[i] = g.arg(0x5F + i) >> 8 & 3;
        for (int k = 0; k < 3; k++) {
            pos[i * 3 + k] = g.f24(0x63 + i * 3 + k);
            dir[i * 3 + k] = g.f24(0x6F + i * 3 + k);
            att[i * 3 + k] = g.f24(0x7B + i * 3 + k);
        }
        sexp[i] = g.f24(0x87 + i);
        scut[i] = g.f24(0x8B + i);
        g.color(0x8F + i * 3, &amb[i * 3]);
        g.color(0x90 + i * 3, &diff[i * 3]);
        g.color(0x91 + i * 3, &spec[i * 3]);
    }
    glUniform1iv(U("u_lightOn"), 4, on);
    glUniform1iv(U("u_lightType"), 4, type);
    glUniform1iv(U("u_lightComp"), 4, comp);
    glUniform3fv(U("u_lightPos"), 4, pos);
    glUniform3fv(U("u_lightDir"), 4, dir);
    glUniform3fv(U("u_lightAtt"), 4, att);
    glUniform1fv(U("u_spotExp"), 4, sexp);
    glUniform1fv(U("u_spotCut"), 4, scut);
    glUniform3fv(U("u_lightAmb"), 4, amb);
    glUniform3fv(U("u_lightDiff"), 4, diff);
    glUniform3fv(U("u_lightSpec"), 4, spec);
}

// Ejecuta un dibujo; las matrices pueden venir interpoladas (si no, las de la instantánea).
void execute(const Snap &g, GpuPrim prim, const GpuVertex *v, int count, const float *world, const float *view,
             const float *proj) {
    int fmt = g.arg(0xD2) & 3;
    uint32_t stride = g.cmd[0x9D] & 0x7FF;
    Target &t = target_for(g.fb_addr(), stride, g.arg(0xD5), fmt);
    if (t.fmt != fmt || t.stride != (int)stride) reformat_target(t, fmt, stride);
    t.cpu_written = false;
    bool clear = g.arg(0xD3) & 1;
    bool self_read = false;
    t.zaddr = g.arg(0x9E) & 0x1FFFF0;
    if ((g.arg(0x23) & 1) || (clear && (g.arg(0xD3) >> 10 & 1))) g_depth_owner[t.zaddr] = t.addr;

    // Framebuffer (o su profundidad) como textura
    GLuint fb_tex = 0;
    float fb_w = 1, fb_h = 1;
    int clut_mode = 0, src_fmt = 3;
    if (g.tex_fb && !clear) {
        uint32_t a = g.tex_fb_addr;
        if (g.tex_fb == 2) {
            auto o = g_depth_owner.find(a);
            a = o != g_depth_owner.end() ? o->second : ~0u;
        }
        auto it = g_targets.find(a);
        if (it != g_targets.end()) {
            const Target &src = it->second;
            int tf = g.arg(0xC3) & 0xF;
            src_fmt = src.fmt;
            if (g.tex_fb == 2) {
                fb_tex = src.depth;
                fb_w = (float)src.depth_w;
                if (tf == 6 && g.clut) clut_mode = 2;
            } else {
                self_read = &src == &t;
                fb_tex = self_read ? self_copy(t) : src.color;
                // Mipmaps hasta la resolución de la PSP, para las reducciones
                if (!self_read && !clut_mode && g_cfg.scale > 1 && it->second.mips_dirty) {
                    int levels = 0;
                    while ((1 << levels) < g_cfg.scale) levels++;
                    glBindTexture(GL_TEXTURE_2D, fb_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    it->second.mips_dirty = false;
                }
                fb_w = (float)src.w;
                if (tf == 6 && g.clut && src.fmt != 3) clut_mode = 1;
                if (tf == 7 && g.clut && src.fmt == 3) clut_mode = 3;
            }
            fb_h = (float)src.h;
        }
    }

    t.mips_dirty = true;
    int S = g_cfg.scale;
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w * S, t.h * S);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);

    int x1 = g.arg(0xD4) & 0x3FF, y1 = g.arg(0xD4) >> 10 & 0x3FF;
    int x2 = g.arg(0xD5) & 0x3FF, y2 = g.arg(0xD5) >> 10 & 0x3FF;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x1 * S, (t.h - (y2 + 1)) * S, std::max(0, x2 - x1 + 1) * S, std::max(0, y2 - y1 + 1) * S);

    bool through = g.through();
    glUniform1i(U("u_through"), through);
    glUniform1i(U("u_clear"), clear);
    glUniform2f(U("u_fbSize"), (float)t.w, (float)t.h);

    if (clear) {
        uint32_t m = g.arg(0xD3);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(m >> 10 & 1);
        bool col = m >> 8 & 1, alpha = m >> 9 & 1;
        glColorMask(col, col, col, alpha);
        // En la PSP el stencil es el canal alfa del framebuffer: borrar el alfa borra también el stencil,
        // con el alfa del color de borrado.
        if (alpha && count > 0) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, v[count - 1].col[3], 0xFF);
            glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
            glStencilMask(0xFF);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        glUniform1i(U("u_texOn"), 0);
        glUniform1i(U("u_lighting"), 0);
        glUniform1i(U("u_fog"), 0);
        glUniform3f(U("u_srcMul"), 1.f, 1.f, 1.f);
    } else {
        if (g.arg(0x23) & 1) {
            static const GLenum F[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(F[g.arg(0xDE) & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(!(g.arg(0xE7) & 1) && (g.arg(0x23) & 1));
        // En la PSP el alfa del framebuffer es el stencil: solo cambia si el test de stencil lo modifica (como en
        // PPSSPP). La profundidad de campo escribe una máscara en el alfa y luego dibuja el color sin tocarla.
        uint32_t cm = g.arg(0xE8), so = g.arg(0xDD);
        bool alpha_out = (g.arg(0x24) & 1) && (g.arg(0xE9) & 0xFF) != 0xFF && fmt != 0 &&
                         ((so & 7) || (so >> 8 & 7) || (so >> 16 & 7));
        glColorMask(!(cm & 0xFF), !(cm >> 8 & 0xFF), !(cm >> 16 & 0xFF), alpha_out);

        // Stencil (p. ej. el minimapa circular: se marca el círculo y el mapa solo se pinta dentro).
        if (g.arg(0x24) & 1) {
            static const GLenum F[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
            static const GLenum OP[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};
            uint32_t sf = g.arg(0xDC), so = g.arg(0xDD);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(F[sf & 7], sf >> 8 & 0xFF, sf >> 16 & 0xFF);
            glStencilOp(OP[so & 7], OP[so >> 8 & 7], OP[so >> 16 & 7]);
            glStencilMask(~g.arg(0xE9) & 0xFF);  // la máscara de escritura del alfa es la del stencil
        } else {
            glDisable(GL_STENCIL_TEST);
        }

        glUniform3f(U("u_srcMul"), 1.f, 1.f, 1.f);
        if (g.arg(0x21) & 1) {
            uint32_t b = g.arg(0xDF);
            GLenum fa = fix_factor(g.arg(0xE0)), fb = fix_factor(g.arg(0xE1));
            bool fix_a = (b & 0xF) >= 10, fix_b = (b >> 4 & 0xF) >= 10;
            if (fix_a && fix_b && fa == GL_CONSTANT_COLOR && fb == GL_CONSTANT_COLOR) {
                // GL solo tiene un color de mezcla: el de la fuente se aplica en el shader (como PPSSPP).
                // El desenfoque de la profundidad de campo mezcla 0xDD * fuente + 0x22 * destino; con un
                // único color (0xDD a los dos) se saturaba a blanco y velaba toda la imagen.
                float m[3];
                g.color(0xE0, m);
                glUniform3fv(U("u_srcMul"), 1, m);
                fa = GL_ONE;
            }
            uint32_t fixc = fb == GL_CONSTANT_COLOR ? g.arg(0xE1) : g.arg(0xE0);
            glBlendColor((fixc & 0xFF) / 255.f, (fixc >> 8 & 0xFF) / 255.f, (fixc >> 16 & 0xFF) / 255.f, 1.f);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(blend_factor(b & 0xF, true, fa), blend_factor(b >> 4 & 0xF, false, fb), GL_ONE, GL_ZERO);
            static const GLenum EQ[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
            glBlendEquationSeparate(EQ[std::min<uint32_t>(b >> 8 & 0xF, 5)], GL_FUNC_ADD);
        } else {
            glDisable(GL_BLEND);
        }

        if ((g.arg(0x1D) & 1) && !through && prim == GpuPrim::Triangles) {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glCullFace((g.arg(0x9B) & 1) ? GL_FRONT : GL_BACK);
        } else {
            glDisable(GL_CULL_FACE);
        }

        float m[16];
        mat43_to_44(world, m);
        glUniformMatrix4fv(U("u_world"), 1, GL_FALSE, m);
        mat43_to_44(view, m);
        glUniformMatrix4fv(U("u_view"), 1, GL_FALSE, m);
        glUniformMatrix4fv(U("u_proj"), 1, GL_FALSE, proj);
        float ox = g.f24(0x45) - (g.arg(0x4C) & 0xFFFF) / 16.f;
        float oy = g.f24(0x46) - (g.arg(0x4D) & 0xFFFF) / 16.f;
        glUniform3f(U("u_vpScale"), g.f24(0x42), g.f24(0x43), g.f24(0x44));
        glUniform3f(U("u_vpOff"), ox, oy, g.f24(0x47));
        glUniform4f(U("u_uvScaleOff"), g.f24(0x48), g.f24(0x49), g.f24(0x4A), g.f24(0x4B));
        set_light_uniforms(g);

        bool tex_on = (g.arg(0x1E) & 1) && (g.tex || fb_tex);
        glUniform1i(U("u_texOn"), tex_on);
        glUniform2f(U("u_uvFix"), 1.f, 1.f);
        glUniform1i(U("u_texClut"), 0);
        glUniform1i(U("u_texFb"), 0);
        if (tex_on && fb_tex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fb_tex);
            bool linear = (g.arg(0xC6) >> 8 & 1) || (g.arg(0xC6) & 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            glUniform1i(U("u_texFb"), 1);
            glUniform1i(U("u_fbScale"), S);
            glUniform1i(U("u_fbLinear"), linear);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glUniform1i(U("u_texFlipY"), 1);
            glUniform2f(U("u_texSize"), fb_w, fb_h);
            // Fuera del modo through las UV van normalizadas al tamaño de la textura de la PSP (potencia de 2)
            glUniform2f(U("u_uvFix"), (1 << (g.arg(0xB8) & 0xF)) / fb_w, (1 << (g.arg(0xB8) >> 8 & 0xF)) / fb_h);
            if (clut_mode) {
                uint32_t cf = g.arg(0xC5);
                glUniform1i(U("u_texClut"), clut_mode);
                glUniform1i(U("u_srcFmt"), src_fmt);
                glUniform3i(U("u_clutIdx"), cf >> 2 & 0x1F, cf >> 8 & 0xFF, (cf >> 16 & 0x1F) << 4);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g.clut);
                glActiveTexture(GL_TEXTURE0);
            }
        } else if (tex_on) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g.tex);
            bool linear = (g.arg(0xC6) >> 8 & 1) || g.tex_hd;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            g.tex_hd ? GL_LINEAR_MIPMAP_LINEAR : linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (g.arg(0xC7) & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (g.arg(0xC7) >> 8 & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glUniform1i(U("u_texFlipY"), 0);
            glUniform2f(U("u_texSize"), g.tex_w, g.tex_h);
        }
        if (tex_on) {
            uint32_t tf = g.arg(0xC9);
            glUniform1i(U("u_texFunc"), tf & 7);
            glUniform1i(U("u_texRGBA"), tf >> 8 & 1);
            glUniform1i(U("u_colorDouble"), tf >> 16 & 1);
            float env[3];
            g.color(0xCA, env);
            glUniform3fv(U("u_envColor"), 1, env);
        }

        uint32_t at = g.arg(0xDB);
        glUniform1i(U("u_alphaTest"), g.arg(0x22) & 1);
        glUniform1i(U("u_alphaFunc"), at & 7);
        glUniform1i(U("u_alphaRef"), at >> 8 & 0xFF);
        glUniform1i(U("u_alphaMask"), at >> 16 & 0xFF);

        bool fog = (g.arg(0x1F) & 1) && !through;
        glUniform1i(U("u_fog"), fog);
        if (fog) {
            float fc[3];
            g.color(0xCF, fc);
            glUniform3fv(U("u_fogColor"), 1, fc);
            glUniform2f(U("u_fogCoef"), g.f24(0xCD), g.f24(0xCE));
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(GpuVertex), v, GL_STREAM_DRAW);
    glDrawArrays(prim == GpuPrim::Triangles ? GL_TRIANGLES : prim == GpuPrim::Lines ? GL_LINES : GL_POINTS, 0, count);
}

void execute(const DrawCmd &d) {
    execute(d.snap, d.prim, d.verts.data(), (int)d.verts.size(), d.snap.world, d.snap.view, d.snap.proj);
}

// Dibuja la interfaz propia encima, hace la captura pendiente (lo mismo que se ve) y presenta.
void finish_window() {
    overlay_render();
    if (!g_pending_shot.empty()) {
        int w, h;
        window_drawable_size(&w, &h);
        std::vector<uint8_t> px((size_t)w * h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (FILE *f = fopen(g_pending_shot.c_str(), "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; y--)
                for (int x = 0; x < w; x++) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
            fclose(f);
            LOGI("gpu", "captura %s (%dx%d)", g_pending_shot.c_str(), w, h);
        }
        g_pending_shot.clear();
    }
    window_swap();
}

// Copia el framebuffer de la PSP (FBO o memoria si lo escribió la CPU) a la ventana con bandas.
void blit_to_window(uint32_t fb_addr, uint32_t stride, uint32_t fmt) {
    int ww, wh;
    window_drawable_size(&ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    auto it = g_targets.find(fb_addr & 0x1FFFF0);
    GLuint src_fbo;
    int sw, sh;
    if (it != g_targets.end() && !it->second.cpu_written) {
        // La GE ha dibujado ahí: se usa su FBO aunque no se redibuje cada vblank (cargas).
        g_presented = &it->second;
        src_fbo = it->second.fbo;
        sw = std::min(it->second.w, 480) * g_cfg.scale;
        sh = std::min(it->second.h, 272) * g_cfg.scale;
    } else if (fb_addr) {
        // Framebuffer escrito por la CPU (vídeos): se sube tal cual.
        g_presented = nullptr;
        std::vector<uint8_t> px(480 * 272 * 4);
        for (int y = 0; y < 272; y++)
            for (int x = 0; x < 480; x++) {
                uint8_t *o = &px[((271 - y) * 480 + x) * 4];
                if (fmt == 3) {
                    memcpy(o, MEMP(fb_addr + (y * stride + x) * 4), 4);
                } else {
                    uint16_t p = R16(fb_addr + (y * stride + x) * 2);
                    if (fmt == 0) { o[0] = (p & 0x1F) << 3; o[1] = (p >> 5 & 0x3F) << 2; o[2] = (p >> 11) << 3; }
                    else if (fmt == 1) { o[0] = (p & 0x1F) << 3; o[1] = (p >> 5 & 0x1F) << 3; o[2] = (p >> 10 & 0x1F) << 3; }
                    else { o[0] = (p & 0xF) << 4; o[1] = (p >> 4 & 0xF) << 4; o[2] = (p >> 8 & 0xF) << 4; }
                }
                o[3] = 255;
            }
        glBindTexture(GL_TEXTURE_2D, g_cpu_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, g_cpu_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_cpu_tex, 0);
        src_fbo = g_cpu_fbo;
        sw = 480;
        sh = 272;
    } else {
        finish_window();
        return;
    }
    float aspect = 480.f / 272.f;
    int dw = ww, dh = (int)(ww / aspect);
    if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, sw, sh, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    finish_window();
}

// ---------------------------------------------------------------- interpolación
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Un salto grande de cámara (corte) no se interpola.
bool camera_cut(const float *a, const float *b) {
    float dt = 0, dr = 0;
    for (int i = 9; i < 12; i++) dt += (a[i] - b[i]) * (a[i] - b[i]);
    for (int i = 0; i < 9; i++) dr += (a[i] - b[i]) * (a[i] - b[i]);
    return dt > 400.f || dr > 0.5f;
}

// ¿Algún vértice se mueve más de un 10 % del tamaño de la malla entre dos fotogramas? Con giros pequeños
// (hasta ~13° en una rueda) la interpolación lineal apenas encoge la malla.
bool large_deformation(const std::vector<GpuVertex> &a, const std::vector<GpuVertex> &b) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, move = 0;
    for (size_t i = 0; i < b.size(); i++) {
        float d = 0;
        for (int k = 0; k < 3; k++) {
            lo[k] = std::min(lo[k], b[i].pos[k]);
            hi[k] = std::max(hi[k], b[i].pos[k]);
            d += (a[i].pos[k] - b[i].pos[k]) * (a[i].pos[k] - b[i].pos[k]);
        }
        move = std::max(move, d);
    }
    float size = 0;
    for (int k = 0; k < 3; k++) size += (hi[k] - lo[k]) * (hi[k] - lo[k]);
    return move > 0.01f * size;
}

// Emparejamiento de los dibujos del último fotograma con los del anterior (se calcula una vez por fotograma
// lógico). Entre los de la misma clave (misma malla y textura: p. ej. cada mata de vegetación) se elige el
// más cercano por la posición de su matriz de mundo: el juego puede ordenarlos distinto en cada fotograma
// y, emparejados por orden, se interpolaba entre ejemplares distintos (parpadeos a medio camino).
std::vector<const DrawCmd *> g_match;
bool g_match_valid = false;

void match_draws() {
    std::unordered_map<uint64_t, std::vector<const DrawCmd *>> prev;
    for (const DrawCmd &d : g_prev.cmds) prev[d.key].push_back(&d);
    std::unordered_map<uint64_t, std::vector<bool>> used;
    g_match.assign(g_last.cmds.size(), nullptr);
    for (size_t i = 0; i < g_last.cmds.size(); i++) {
        const DrawCmd &d = g_last.cmds[i];
        auto it = prev.find(d.key);
        if (it == prev.end()) continue;
        std::vector<bool> &u = used[d.key];
        u.resize(it->second.size());
        int best = -1;
        float best_d = 0;
        for (size_t k = 0; k < it->second.size(); k++) {
            if (u[k]) continue;
            const float *a = it->second[k]->snap.world, *b = d.snap.world;
            float dist = (a[9] - b[9]) * (a[9] - b[9]) + (a[10] - b[10]) * (a[10] - b[10]) + (a[11] - b[11]) * (a[11] - b[11]);
            if (best < 0 || dist < best_d) {
                best = (int)k;
                best_d = dist;
                if (dist == 0) break;
            }
        }
        if (best >= 0) {
            u[best] = true;
            g_match[i] = it->second[best];
        }
    }
    g_match_valid = true;
}

void render_interpolated(float t) {
    if (!g_match_valid) match_draws();
    float world[12], view[12], proj[16];
    for (size_t i = 0; i < g_last.cmds.size(); i++) {
        const DrawCmd &d = g_last.cmds[i];
        const DrawCmd *p = g_match[i];
        if (!p || d.snap.through() || t >= 1.f || camera_cut(p->snap.view, d.snap.view)) {
            execute(d);
            continue;
        }
        for (int i = 0; i < 12; i++) {
            world[i] = lerp(p->snap.world[i], d.snap.world[i], t);
            view[i] = lerp(p->snap.view[i], d.snap.view[i], t);
        }
        for (int i = 0; i < 16; i++) proj[i] = lerp(p->snap.proj[i], d.snap.proj[i], t);
        const GpuVertex *verts = d.verts.data();
        if (p->verts.size() == d.verts.size()) {  // mallas deformadas (skinning en CPU): se interpolan vértices
            bool differ = memcmp(p->verts.data(), d.verts.data(), d.verts.size() * sizeof(GpuVertex)) != 0;
            if (differ && large_deformation(p->verts, d.verts)) {
                // Una rueda gira decenas de grados por fotograma lógico: interpolar en línea recta acerca sus
                // vértices al eje (se encoge, se descentra y parece transparente). Se dibuja sin interpolar
                // la deformación; la matriz de mundo sí se interpola y la rueda sigue al coche.
                differ = false;
            }
            if (differ) {
                g_lerp_verts = d.verts;
                for (size_t i = 0; i < d.verts.size(); i++)
                    for (int k = 0; k < 3; k++) {
                        g_lerp_verts[i].pos[k] = lerp(p->verts[i].pos[k], d.verts[i].pos[k], t);
                        g_lerp_verts[i].nrm[k] = lerp(p->verts[i].nrm[k], d.verts[i].nrm[k], t);
                    }
                verts = g_lerp_verts.data();
            }
        }
        execute(d.snap, d.prim, verts, (int)d.verts.size(), world, view, proj);
    }
}

}  // namespace

bool renderer_init(const RendererConfig &cfg) {
    g_cfg = cfg;
    // Filtrado anisotrópico para las texturas en alta resolución (núcleo en GL 4.6; extensión casi universal antes).
    if (SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic") ||
        SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g_max_aniso);
        g_max_aniso = std::min(g_max_aniso, 16.f);
    }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    GLint ok;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(g_prog, sizeof(log), nullptr, log);
        fatal("error enlazando shaders: %s", log);
    }
    glUseProgram(g_prog);
    glUniform1i(U("u_tex"), 0);
    glUniform1i(U("u_clut"), 1);
    g_refmt_prog = glCreateProgram();
    glAttachShader(g_refmt_prog, compile(GL_VERTEX_SHADER, REFMT_VS));
    glAttachShader(g_refmt_prog, compile(GL_FRAGMENT_SHADER, REFMT_FS));
    glLinkProgram(g_refmt_prog);
    glGetProgramiv(g_refmt_prog, GL_LINK_STATUS, &ok);
    if (!ok) fatal("error enlazando el shader de reinterpretación");
    glGenVertexArrays(1, &g_refmt_vao);
    glGenFramebuffers(1, &g_copy_fbo);
    glUseProgram(g_prog);
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void *)offsetof(GpuVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void *)offsetof(GpuVertex, nrm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void *)offsetof(GpuVertex, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex), (void *)offsetof(GpuVertex, col));
    glGenTextures(1, &g_cpu_tex);
    glGenFramebuffers(1, &g_cpu_fbo);
    texrep_init(cfg.textures_dir, cfg.dump_dir);

    int hz = cfg.max_fps;
    if (cfg.interpolate && hz <= 0) {
        SDL_DisplayMode mode;
        hz = SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate : 144;
    }
    g_present_interval = cfg.interpolate ? 1000000ULL / std::max(hz, 30) : 0;
    LOGI("gpu", "renderizador OpenGL listo (escala x%d, %s)", cfg.scale,
         cfg.interpolate ? ("interpolación a " + std::to_string(hz) + " fps").c_str() : "sin interpolación");
    return true;
}

bool renderer_interpolating() { return g_cfg.interpolate; }

// Depuración: CARS_GE_DUMP=T registra el estado de cada dibujo del primer fotograma tras el segundo T.
static const double g_dump_at = getenv("CARS_GE_DUMP") ? atof(getenv("CARS_GE_DUMP")) : 1e30;
static int g_dump_state = 0, g_dump_index = 0;  // 0 = esperando, 1 = volcando, 2 = hecho

static void dump_draw(GpuPrim prim, const GpuVertex *v, int count) {
    const uint32_t *c = g_ge.cmd;
    auto A = [&](int i) { return c[i] & 0xFFFFFF; };
    float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < count; i++) {
        x0 = std::min(x0, v[i].pos[0]); x1 = std::max(x1, v[i].pos[0]);
        y0 = std::min(y0, v[i].pos[1]); y1 = std::max(y1, v[i].pos[1]);
    }
    LOGI("gedump", "#%d fb=%06x fmt=%u prim=%d n=%d thr=%d bbox=(%.0f,%.0f)-(%.0f,%.0f) clear=%d:%03x "
         "tex=%d:%06x fmt%u blend=%d:%03x %06x/%06x atest=%d:%06x stencil=%d func=%06x op=%06x "
         "depth=%d:%u zmask=%u cmask=%06x amask=%02x logic=%d",
         g_dump_index++, A(0x9C) & 0x1FFFF0, A(0xD2), (int)prim, count, (c[0x12] >> 23) & 1, x0, y0, x1, y1,
         A(0xD3) & 1, A(0xD3) >> 8 & 0x7, A(0x1E) & 1, A(0xA0) | (A(0xA8) & 0xFF0000) << 8, A(0xC3),
         A(0x21) & 1, A(0xDF), A(0xE0), A(0xE1), A(0x22) & 1, A(0xDB), A(0x24) & 1, A(0xDC), A(0xDD),
         A(0x23) & 1, A(0xDE) & 7, A(0xE7) & 1, A(0xE8), A(0xE9) & 0xFF, A(0x25) & 1);
    if (count > 12) return;
    LOGI("gedump", "    stride=%u zbuf=%06x clutfmt=%06x clut[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x",
         A(0x9D) & 0x7FF, A(0x9E) & 0xFFFFF0, A(0xC5), ((uint32_t *)g_ge.clut)[0], ((uint32_t *)g_ge.clut)[1],
         ((uint32_t *)g_ge.clut)[2], ((uint32_t *)g_ge.clut)[3], ((uint32_t *)g_ge.clut)[4],
         ((uint32_t *)g_ge.clut)[5], ((uint32_t *)g_ge.clut)[6], ((uint32_t *)g_ge.clut)[7]);
    if (const char *dir = getenv("CARS_GE_DUMP_DIR"); dir && (A(0x1E) & 1)) {  // textura tal y como se usa
        TexParams p{g_ge.tex_addr(0), g_ge.tex_bufw(0), g_ge.tex_w(0), g_ge.tex_h(0), (int)(A(0xC3) & 0xF),
                    (A(0xC2) & 1) != 0, A(0xC5)};
        std::vector<uint8_t> rgba;
        texture_decode(p, g_ge.clut, rgba);
        char path[512];
        snprintf(path, sizeof(path), "%s/%03d_%08x.png", dir, g_dump_index - 1, p.addr);
        texrep_write_png(path, rgba.data(), p.w, p.h);
        LOGI("gedump", "    clutfmt=%06x clutaddr=%08x -> %s", p.clutfmt,
             (A(0xB0) & 0xFFFFF0) | ((c[0xB1] & 0x0F0000) << 8), path);
    }
    auto F = [&](int i) { uint32_t u = c[i] << 8; float f; memcpy(&f, &u, 4); return f; };
    LOGI("gedump", "    tex %dx%d wrap=%04x filt=%04x uvscale=(%.3f,%.3f) off=(%.3f,%.3f) texfunc=%06x vtype=%06x",
         1 << (A(0xB8) & 0xF), 1 << (A(0xB8) >> 8 & 0xF), A(0xC7), A(0xC6), F(0x48), F(0x49), F(0x4A), F(0x4B),
         A(0xC9), A(0x12));
    // Vértices en coordenadas de pantalla de la PSP (proyección * vista * mundo), UV y color
    float w44[16], v44[16];
    mat43_to_44(g_ge.world, w44);
    mat43_to_44(g_ge.view, v44);
    for (int i = 0; i < count; i++) {
        float p[4] = {v[i].pos[0], v[i].pos[1], v[i].pos[2], 1}, q[4], r[4];
        float sx = p[0], sy = p[1];
        if (!((c[0x12] >> 23) & 1)) {
            for (int k = 0; k < 4; k++) q[k] = w44[k] * p[0] + w44[4 + k] * p[1] + w44[8 + k] * p[2] + w44[12 + k];
            for (int k = 0; k < 4; k++) r[k] = v44[k] * q[0] + v44[4 + k] * q[1] + v44[8 + k] * q[2] + v44[12 + k] * q[3];
            for (int k = 0; k < 4; k++)
                q[k] = g_ge.proj[k] * r[0] + g_ge.proj[4 + k] * r[1] + g_ge.proj[8 + k] * r[2] + g_ge.proj[12 + k] * r[3];
            sx = 240 + 240 * q[0] / q[3];
            sy = 136 - 136 * q[1] / q[3];
        }
        LOGI("gedump", "    v%d pant=(%.1f,%.1f) uv=(%.3f,%.3f) col=%02x%02x%02x%02x", i, sx, sy, v[i].uv[0], v[i].uv[1],
             v[i].col[0], v[i].col[1], v[i].col[2], v[i].col[3]);
    }
}

void renderer_draw(GpuPrim prim, const GpuVertex *v, int count, uint64_t match_key) {
    if (g_dump_state == 1) dump_draw(prim, v, count);
    DrawCmd d;
    memcpy(d.snap.cmd, g_ge.cmd, sizeof(d.snap.cmd));
    memcpy(d.snap.world, g_ge.world, sizeof(d.snap.world));
    memcpy(d.snap.view, g_ge.view, sizeof(d.snap.view));
    memcpy(d.snap.proj, g_ge.proj, sizeof(d.snap.proj));
    if ((g_ge.arg(0x1E) & 1) && !(g_ge.arg(0xD3) & 1)) resolve_texture(d.snap);
    d.prim = prim;
    d.key = match_key;
    // La GE vuelve a dibujar en este framebuffer: deja de mostrarse desde memoria. Hay que hacerlo al
    // grabar, no al ejecutar: con interpolación los dibujos solo se ejecutan si se presenta el FBO.
    // El FBO se crea ya al grabar: el presentador decide por su existencia si mostrar el FBO o la memoria.
    target_for(g_ge.fb_addr(), g_ge.cmd[0x9D] & 0x7FF, g_ge.arg(0xD5), g_ge.arg(0xD2) & 3).cpu_written = false;
    if (!g_cfg.interpolate) {
        execute(d.snap, prim, v, count, d.snap.world, d.snap.view, d.snap.proj);
        return;
    }
    d.verts.assign(v, v + count);
    g_recording.cmds.push_back(std::move(d));
}

void renderer_invalidate(uint32_t addr, uint32_t size) {
    // Escritura de CPU/DMA/transferencia en VRAM: los framebuffers afectados se muestran desde memoria.
    if ((addr & 0x0F000000) != 0x04000000) return;
    uint32_t lo = addr & 0x1FFFFF, hi = lo + size;
    for (auto &[off, t] : g_targets) {
        uint32_t t_end = off + 512 * t.h * 4;
        if (lo < t_end && hi > off) t.cpu_written = true;
    }
}

bool display_is_cpu(uint32_t fb_addr) {
    auto it = g_targets.find(fb_addr & 0x1FFFF0);
    return it == g_targets.end() || it->second.cpu_written;
}

// Con CARS_GE_DUMP_DIR, al terminar el volcado se guardan también los framebuffers (RGBA, a escala 1).
static void dump_targets() {
    const char *dir = getenv("CARS_GE_DUMP_DIR");
    if (!dir) return;
    int S = g_cfg.scale;
    for (auto &[addr, t] : g_targets) {
        std::vector<uint8_t> px((size_t)t.w * S * t.h * S * 4), out((size_t)t.w * t.h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, t.w * S, t.h * S, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++)
                memcpy(&out[((size_t)y * t.w + x) * 4], &px[((size_t)(t.h - 1 - y) * S * t.w * S + x * S) * 4], 4);
        char path[512];
        snprintf(path, sizeof(path), "%s/fb_%06x_fmt%d.png", dir, addr, t.fmt);
        texrep_write_png(path, out.data(), t.w, t.h);
    }
}

void renderer_flip(uint32_t fb_addr, uint32_t stride, uint32_t fmt) {
    if (g_dump_state == 1) {
        g_dump_state = 2;
        if (!g_cfg.interpolate) dump_targets();
    }
    if (g_dump_state == 0 && time_us() / 1e6 >= g_dump_at) g_dump_state = 1;
    // CLUTs de framebuffers que ya no se usan (las de los fotogramas grabados siguen vivas)
    if (++g_frame_no % 300 == 0) {
        for (auto it = g_cluts.begin(); it != g_cluts.end();) {
            if (g_frame_no - it->second.used > 60) {
                glDeleteTextures(1, &it->second.tex);
                it = g_cluts.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!g_cfg.interpolate) return;
    if (g_recording.cmds.empty() && g_have_last) {  // flip sin dibujos nuevos: se mantiene el fotograma
        g_last.display = fb_addr;
        return;
    }
    count_stats(true);
    g_recording.t_us = time_us();
    g_recording.display = fb_addr;
    g_recording.stride = stride;
    g_recording.fmt = fmt;
    if (g_have_last) {
        g_prev = std::move(g_last);
        g_have_prev = true;
    }
    g_last = std::move(g_recording);
    g_have_last = true;
    g_match_valid = false;
    g_recording = Frame();
}

uint64_t renderer_next_present_in() {
    if (!g_cfg.interpolate || !g_have_last) return UINT64_MAX;
    uint64_t now = time_us(), due = g_last_present + g_present_interval;
    return due > now ? due - now : 0;
}

bool renderer_present(uint32_t fb_addr, uint32_t stride, uint32_t fmt) {
    static uint64_t dbg_t = 0;
    if (g_log_level >= LOG_DEBUG && time_us() - dbg_t > 1000000) {
        dbg_t = time_us();
        auto it = g_targets.find(fb_addr & 0x1FFFF0);
        LOGD("gpu", "present fb=%08x cpu=%d existe=%d last=%d (display %08x, %zu dibujos) prev=%d", fb_addr,
             it != g_targets.end() && it->second.cpu_written, it != g_targets.end(), g_have_last, g_last.display,
             g_last.cmds.size(), g_have_prev);
    }
    if (!g_cfg.interpolate || !g_have_last || display_is_cpu(fb_addr)) {
        if (g_cfg.interpolate) {  // sin interpolación posible (vídeo): se limita a la frecuencia de presentación
            uint64_t now = time_us();
            if (now < g_last_present + g_present_interval) return false;
            g_last_present = now;
            count_stats(false);
        }
        blit_to_window(fb_addr, stride, fmt);
        return true;
    }
    uint64_t now = time_us();
    if (now < g_last_present + g_present_interval) return false;
    g_last_present = now;
    // Se muestra el fotograma anterior avanzando hacia el último: t = 0 al llegar el último, 1 al cabo
    // de un intervalo lógico (un fotograma de latencia a cambio de movimiento continuo).
    float t = 1.f;
    if (g_have_prev && g_last.t_us > g_prev.t_us) {
        t = (float)(now - g_last.t_us) / (float)(g_last.t_us - g_prev.t_us);
        t = std::clamp(t, 0.f, 1.f);
        render_interpolated(t);
    } else {
        for (const DrawCmd &d : g_last.cmds) execute(d);
    }
    if (g_dump_state == 2) {
        g_dump_state = 3;
        dump_targets();
    }
    blit_to_window(g_last.display, g_last.stride, g_last.fmt);
    count_stats(false);
    return true;
}

int renderer_reload_textures() {
    int n = texrep_rescan();
    // Se conservan los nombres de textura de GL (los fotogramas grabados los usan); solo se fuerza la
    // nueva subida en su próximo uso.
    for (auto &[key, ct] : g_textures) ct.hash = 0;
    return n;
}

bool renderer_screenshot(const std::string &path) {
    g_pending_shot = path;  // se captura en la próxima presentación, con la interfaz incluida
    return true;
}
