// Renderizador OpenGL 3.3 core de la GE: emula la tubería fija de la PSP a resolución interna escalada.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "../core.h"
#include "../platform/window.h"
#include "gl_funcs.h"
#include "renderer.h"
#include "texture.h"

namespace {

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
    v_uv = a_uv * u_uvScaleOff.xy + u_uvScaleOff.zw;
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
uniform vec3 u_envColor;
uniform int u_alphaTest, u_alphaFunc, u_alphaRef, u_alphaMask;
uniform int u_fog;
uniform vec3 u_fogColor;
uniform int u_clear;

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
        vec4 t = texture(u_tex, uv);
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
    o_color = c;
}
)";

struct Target {
    uint32_t addr = 0;
    int w = 480, h = 272;
    GLuint fbo = 0, color = 0, depth = 0;
    uint64_t last_frame = 0;
};

struct CachedTex {
    GLuint tex = 0;
    uint64_t hash = 0;
    int w = 0, h = 0;
};

RendererConfig g_cfg;
GLuint g_prog = 0, g_vao = 0, g_vbo = 0;
std::unordered_map<std::string, GLint> g_loc;
std::unordered_map<uint32_t, Target> g_targets;
std::unordered_map<uint64_t, CachedTex> g_textures;
uint64_t g_frame = 1;
Target *g_presented = nullptr;
GLuint g_cpu_tex = 0, g_cpu_fbo = 0;
std::vector<uint8_t> g_rgba;

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

Target &target_for(uint32_t addr) {
    Target &t = g_targets[addr];
    if (t.fbo) return t;
    t.addr = addr;
    uint32_t stride = g_ge.cmd[0x9D] & 0x7FF;
    int sx2 = (g_ge.arg(0xD5) & 0x3FF) + 1, sy2 = (g_ge.arg(0xD5) >> 10 & 0x3FF) + 1;
    t.w = stride >= 480 ? 480 : std::max<int>(stride, 16);
    t.h = t.w == 480 ? 272 : std::min(std::max(sy2, 16), 512);
    (void)sx2;
    int S = g_cfg.scale;
    glGenTextures(1, &t.color);
    glBindTexture(GL_TEXTURE_2D, t.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w * S, t.h * S, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &t.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t.w * S, t.h * S);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("FBO incompleto");
    glClearColor(0, 0, 0, 0);
    glClearDepth(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    LOGI("gpu", "framebuffer %06x: %dx%d (interno %dx%d)", addr, t.w, t.h, t.w * S, t.h * S);
    return t;
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

void bind_texture() {
    TexParams p;
    p.addr = g_ge.tex_addr(0);
    p.bufw = g_ge.tex_bufw(0);
    p.w = g_ge.tex_w(0);
    p.h = g_ge.tex_h(0);
    p.format = g_ge.arg(0xC3) & 0xF;
    p.swizzled = g_ge.arg(0xC2) & 1;
    p.clutfmt = g_ge.arg(0xC5);
    if (p.format > 10) p.format = 3;
    bool clamp_s = g_ge.arg(0xC7) & 1, clamp_t = g_ge.arg(0xC7) >> 8 & 1;
    bool linear = g_ge.arg(0xC6) >> 8 & 1;

    // ¿render-to-texture? (la textura es un framebuffer que hemos dibujado)
    uint32_t vram_off = p.addr & 0x1FFFF0;
    if ((p.addr & 0x0F000000) == 0x04000000) {
        auto it = g_targets.find(vram_off);
        if (it != g_targets.end()) {
            glBindTexture(GL_TEXTURE_2D, it->second.color);
            glUniform1i(U("u_texFlipY"), 1);
            glUniform2f(U("u_texSize"), (float)it->second.w, (float)it->second.h);
            return;
        }
    }
    glUniform1i(U("u_texFlipY"), 0);
    uint64_t key = (uint64_t)p.addr << 32 ^ (uint64_t)p.format << 28 ^ (uint64_t)p.w << 16 ^ (uint64_t)p.h << 4 ^
                   (p.clutfmt * 0x9E3779B97F4A7C15ULL);
    CachedTex &ct = g_textures[key];
    uint64_t h = texture_hash(p, g_ge.clut);
    if (!ct.tex || ct.hash != h) {
        texture_decode(p, g_ge.clut, g_rgba);
        if (!ct.tex) glGenTextures(1, &ct.tex);
        glBindTexture(GL_TEXTURE_2D, ct.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p.w, p.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba.data());
        ct.hash = h;
        ct.w = p.w;
        ct.h = p.h;
    } else {
        glBindTexture(GL_TEXTURE_2D, ct.tex);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_s ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_t ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glUniform2f(U("u_texSize"), (float)p.w, (float)p.h);
}

void set_light_uniforms() {
    const GeState &g = g_ge;
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

}  // namespace

bool renderer_init(const RendererConfig &cfg) {
    g_cfg = cfg;
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
    LOGI("gpu", "renderizador OpenGL listo (escala x%d)", cfg.scale);
    return true;
}

void renderer_draw(GpuPrim prim, const GpuVertex *v, int count) {
    const GeState &g = g_ge;
    Target &t = target_for(g.fb_addr());
    t.last_frame = g_frame;
    int S = g_cfg.scale;
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, t.w * S, t.h * S);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);

    // scissor
    int x1 = g.arg(0xD4) & 0x3FF, y1 = g.arg(0xD4) >> 10 & 0x3FF;
    int x2 = g.arg(0xD5) & 0x3FF, y2 = g.arg(0xD5) >> 10 & 0x3FF;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x1 * S, (t.h - (y2 + 1)) * S, std::max(0, x2 - x1 + 1) * S, std::max(0, y2 - y1 + 1) * S);

    bool through = g.through();
    bool clear = g.arg(0xD3) & 1;
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
        glUniform1i(U("u_texOn"), 0);
        glUniform1i(U("u_lighting"), 0);
        glUniform1i(U("u_fog"), 0);
    } else {
        // profundidad
        if (g.arg(0x23) & 1) {
            static const GLenum F[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(F[g.arg(0xDE) & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(!(g.arg(0xE7) & 1) && (g.arg(0x23) & 1));
        uint32_t cm = g.arg(0xE8);
        glColorMask(!(cm & 0xFF), !(cm >> 8 & 0xFF), !(cm >> 16 & 0xFF), !(g.arg(0xE9) & 0xFF));

        // mezcla
        if (g.arg(0x21) & 1) {
            uint32_t b = g.arg(0xDF);
            GLenum fa = fix_factor(g.arg(0xE0)), fb = fix_factor(g.arg(0xE1));
            uint32_t fixc = fa == GL_CONSTANT_COLOR ? g.arg(0xE0) : g.arg(0xE1);
            glBlendColor((fixc & 0xFF) / 255.f, (fixc >> 8 & 0xFF) / 255.f, (fixc >> 16 & 0xFF) / 255.f, 1.f);
            glEnable(GL_BLEND);
            GLenum src = blend_factor(b & 0xF, true, fa), dst = blend_factor(b >> 4 & 0xF, false, fb);
            glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
            static const GLenum EQ[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD};
            GLenum eq = EQ[std::min<uint32_t>(b >> 8 & 0xF, 5)];
            glBlendEquationSeparate(eq, GL_FUNC_ADD);
        } else {
            glDisable(GL_BLEND);
        }

        // culling (no se aplica en through)
        if ((g.arg(0x1D) & 1) && !through && prim == GpuPrim::Triangles) {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glCullFace((g.arg(0x9B) & 1) ? GL_FRONT : GL_BACK);
        } else {
            glDisable(GL_CULL_FACE);
        }

        // matrices y viewport de la GE
        float m[16];
        mat43_to_44(g.world, m);
        glUniformMatrix4fv(U("u_world"), 1, GL_FALSE, m);
        mat43_to_44(g.view, m);
        glUniformMatrix4fv(U("u_view"), 1, GL_FALSE, m);
        glUniformMatrix4fv(U("u_proj"), 1, GL_FALSE, g.proj);
        float ox = g.f24(0x45) - (g.arg(0x4C) & 0xFFFF) / 16.f;
        float oy = g.f24(0x46) - (g.arg(0x4D) & 0xFFFF) / 16.f;
        glUniform3f(U("u_vpScale"), g.f24(0x42), g.f24(0x43), g.f24(0x44));
        glUniform3f(U("u_vpOff"), ox, oy, g.f24(0x47));
        glUniform4f(U("u_uvScaleOff"), g.f24(0x48), g.f24(0x49), g.f24(0x4A), g.f24(0x4B));
        set_light_uniforms();

        // textura
        bool tex_on = g.arg(0x1E) & 1;
        glUniform1i(U("u_texOn"), tex_on);
        if (tex_on) {
            glActiveTexture(GL_TEXTURE0);
            bind_texture();
            uint32_t tf = g.arg(0xC9);
            glUniform1i(U("u_texFunc"), tf & 7);
            glUniform1i(U("u_texRGBA"), tf >> 8 & 1);
            glUniform1i(U("u_colorDouble"), tf >> 16 & 1);
            float env[3];
            g.color(0xCA, env);
            glUniform3fv(U("u_envColor"), 1, env);
        }

        // prueba de alfa
        uint32_t at = g.arg(0xDB);
        glUniform1i(U("u_alphaTest"), g.arg(0x22) & 1);
        glUniform1i(U("u_alphaFunc"), at & 7);
        glUniform1i(U("u_alphaRef"), at >> 8 & 0xFF);
        glUniform1i(U("u_alphaMask"), at >> 16 & 0xFF);

        // niebla
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

void renderer_invalidate(uint32_t, uint32_t) {}

void renderer_present(uint32_t fb_addr, uint32_t stride, uint32_t fmt) {
    int ww, wh;
    window_drawable_size(&ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    uint32_t off = fb_addr & 0x1FFFF0;
    auto it = g_targets.find(off);
    GLuint src_fbo;
    int sw, sh;
    // Si la GE ha dibujado alguna vez en esa dirección se usa su FBO (aunque no se redibuje cada fotograma,
    // como en las cargas); si no, el contenido lo ha escrito la CPU (vídeos) y se sube desde memoria.
    if (it != g_targets.end()) {
        g_presented = &it->second;
        src_fbo = it->second.fbo;
        sw = it->second.w * g_cfg.scale;
        sh = it->second.h * g_cfg.scale;
    } else if (fb_addr) {
        // framebuffer escrito por la CPU (vídeos, cargas): se sube tal cual
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
        window_swap();
        g_frame++;
        return;
    }
    // ajuste con bandas manteniendo la proporción 480:272
    float aspect = 480.f / 272.f;
    int dw = ww, dh = (int)(ww / aspect);
    if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, sw, sh, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    window_swap();
    g_frame++;
}

bool renderer_screenshot(const std::string &path) {
    Target *t = g_presented;
    int w = t ? t->w * g_cfg.scale : 480, h = t ? t->h * g_cfg.scale : 272;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t ? t->fbo : g_cpu_fbo);
    std::vector<uint8_t> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
    LOGI("gpu", "captura %s (%dx%d)", path.c_str(), w, h);
    return true;
}
