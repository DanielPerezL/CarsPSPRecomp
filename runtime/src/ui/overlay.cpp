#include "overlay.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../core.h"
#include "dialog.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/imgui.h"

namespace {

SDL_Window *g_window = nullptr;
bool g_ready = false;
std::string g_font_path;
float g_font_px = 0;  // tamaño con el que está generado el atlas de la fuente
float g_alpha = 0;    // aparición/desaparición del diálogo
uint64_t g_last_us = 0;

// Fuentes libres habituales de cada sistema (con acentos y ñ). Sin ninguna, la de ImGui.
std::string find_font() {
    std::vector<std::string> candidates = {
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    if (const char *windir = getenv("WINDIR")) {
        candidates.insert(candidates.begin(), std::string(windir) + "\\Fonts\\segoeui.ttf");
        candidates.push_back(std::string(windir) + "\\Fonts\\arial.ttf");
    }
    std::error_code ec;
    for (auto &c : candidates)
        if (std::filesystem::exists(c, ec)) return c;
    return "";
}

void build_font(float px) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFont *font = nullptr;
    if (!g_font_path.empty()) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        // Latin-1 y Latin Extended-A, puntuación (… “ ” –), monedas y símbolos como ™ (Memory Stick™)
        static const ImWchar ranges[] = {0x0020, 0x017F, 0x2000, 0x206F, 0x20A0, 0x20CF, 0x2100, 0x214F, 0};
        font = io.Fonts->AddFontFromFileTTF(g_font_path.c_str(), px, &cfg, ranges);
    }
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = std::round(px);
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    g_font_px = px;
}

// Rectángulo de la imagen de la PSP dentro de la ventana (mismas bandas que el renderer).
struct Screen {
    float x, y, s;  // origen y escala: 1 unidad = 1 píxel de la PSP
    ImVec2 p(float px, float py) const { return {x + px * s, y + py * s}; }
};

Screen screen_rect() {
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float s = std::min(size.x / 480.f, size.y / 272.f);
    return {(size.x - 480 * s) / 2, (size.y - 272 * s) / 2, s};
}

ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(255 * a * g_alpha)); }

// Ajuste de línea propio para centrar cada línea (AddText solo alinea a la izquierda).
std::vector<std::string> wrap(const std::string &text, float size, float width) {
    ImFont *font = ImGui::GetFont();
    auto w = [&](const std::string &s) { return font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x; };
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && w(cand) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            i = sp == std::string::npos ? para.size() : sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

// Iconos de la PSP dibujados (no dependen de la fuente): ✕ azul y ○ rojo.
void icon(ImDrawList *dl, ImVec2 c, float r, bool cross) {
    dl->AddCircleFilled(c, r, rgba(30, 30, 36, 0.9f));
    if (cross) {
        float k = r * 0.45f;
        ImU32 col = rgba(120, 170, 255, 1);
        dl->AddLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, col, r * 0.22f);
        dl->AddLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, col, r * 0.22f);
    } else {
        dl->AddCircle(c, r * 0.5f, rgba(255, 110, 110, 1), 0, r * 0.2f);
    }
}

// Botón de la barra inferior (icono + texto); devuelve si se ha pulsado con el ratón.
bool hint(ImDrawList *dl, const Screen &sc, float &right, bool cross, const std::string &label, float fs) {
    ImFont *font = ImGui::GetFont();
    float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str()).x;
    float r = 6 * sc.s, gap = 4 * sc.s;
    float x1 = right, x0 = x1 - tw - 2 * r - gap;
    float cy = sc.p(0, 252).y;
    ImVec2 a{x0 - 4 * sc.s, cy - 10 * sc.s}, b{x1 + 4 * sc.s, cy + 10 * sc.s};
    bool hover = ImGui::IsMouseHoveringRect(a, b);
    if (hover) dl->AddRectFilled(a, b, rgba(255, 255, 255, 0.12f), 4 * sc.s);
    icon(dl, {x0 + r, cy}, r, cross);
    dl->AddText(font, fs, {x0 + 2 * r + gap, cy - fs / 2}, rgba(255, 255, 255, 1), label.c_str());
    right = x0 - 14 * sc.s;
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void draw_dialog(ImDrawList *dl) {
    SystemDialog &d = g_dialog;
    Screen sc = screen_rect();
    ImFont *font = ImGui::GetFont();
    float fs = 14 * sc.s;

    // Fondo: oscurece la escena del juego como el sistema de la PSP.
    dl->AddRectFilled(sc.p(0, 0), sc.p(480, 272), rgba(0, 0, 0, 0.55f));

    std::vector<std::string> lines = wrap(d.text, fs, 380 * sc.s);
    float line_h = fs * 1.35f, text_h = lines.size() * line_h;
    float buttons_h = d.yesno ? 34 * sc.s : 0;
    float block_h = text_h + buttons_h;
    float top = sc.p(0, 126).y - block_h / 2;

    // Líneas horizontales que enmarcan el mensaje
    float margin = 14 * sc.s;
    ImU32 line_col = rgba(255, 255, 255, 0.7f);
    dl->AddLine({sc.p(40, 0).x, top - margin}, {sc.p(440, 0).x, top - margin}, line_col, std::max(1.f, sc.s * 0.6f));
    dl->AddLine({sc.p(40, 0).x, top + block_h + margin}, {sc.p(440, 0).x, top + block_h + margin}, line_col,
                std::max(1.f, sc.s * 0.6f));

    for (size_t i = 0; i < lines.size(); i++) {
        float w = font->CalcTextSizeA(fs, FLT_MAX, 0, lines[i].c_str()).x;
        dl->AddText(font, fs, {sc.p(240, 0).x - w / 2, top + i * line_h}, rgba(255, 255, 255, 1), lines[i].c_str());
    }

    if (d.yesno) {
        const char *labels[2] = {"Sí", "No"};
        float cy = top + text_h + buttons_h * 0.6f;
        for (int i = 0; i < 2; i++) {
            int value = i == 0 ? 1 : 0;
            ImVec2 c{sc.p(240 + (i == 0 ? -40 : 40), 0).x, cy};
            ImVec2 a{c.x - 30 * sc.s, c.y - 10 * sc.s}, b{c.x + 30 * sc.s, c.y + 10 * sc.s};
            if (ImGui::IsMouseHoveringRect(a, b)) {
                if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) d.choice = value;
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) d.clicked = i == 0 ? 1 : 2;
            }
            bool sel = d.choice == value;
            if (sel) {
                float pulse = 0.75f + 0.25f * std::sin(SDL_GetTicks() / 180.f);
                dl->AddRectFilled(a, b, rgba(255, 255, 255, 0.22f * pulse), 5 * sc.s);
                dl->AddRect(a, b, rgba(255, 255, 255, 0.85f * pulse), 5 * sc.s, 0, std::max(1.f, sc.s * 0.6f));
            }
            float w = font->CalcTextSizeA(fs, FLT_MAX, 0, labels[i]).x;
            dl->AddText(font, fs, {c.x - w / 2, c.y - fs / 2}, rgba(255, 255, 255, sel ? 1 : 0.6f), labels[i]);
        }
    }

    // Barra inferior: ✕ Aceptar / ○ Atrás (pulsables con el ratón)
    float hs = 12 * sc.s, right = sc.p(466, 0).x;
    if (d.cancel && hint(dl, sc, right, false, d.cancel_label.empty() ? "Atrás" : d.cancel_label, hs))
        d.clicked = 3;
    if ((d.yesno || d.ok) && hint(dl, sc, right, true, d.ok_label.empty() ? "Aceptar" : d.ok_label, hs))
        d.clicked = d.yesno && d.choice == 0 ? 2 : 1;

    // Teclado: Intro/Espacio aceptan y Escape vuelve, además de los botones de la PSP.
    if (d.visible) {
        if ((d.yesno || d.ok) && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                  ImGui::IsKeyPressed(ImGuiKey_Space, false)))
            d.clicked = d.yesno && d.choice == 0 ? 2 : 1;
        if (d.cancel && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) d.clicked = 3;
    }
}

}  // namespace

bool overlay_init(void *sdl_window, void *gl_context, const char *font) {
    g_window = static_cast<SDL_Window *>(sdl_window);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    if (!ImGui_ImplSDL2_InitForOpenGL(g_window, gl_context) || !ImGui_ImplOpenGL3_Init("#version 330 core")) {
        LOGE("ui", "no se pudo iniciar Dear ImGui");
        return false;
    }
    g_font_path = font && *font ? font : find_font();
    LOGI("ui", "fuente de la interfaz: %s", g_font_path.empty() ? "(la de ImGui)" : g_font_path.c_str());
    g_ready = true;
    return true;
}

void overlay_shutdown() {
    if (!g_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

void overlay_event(const void *sdl_event) {
    if (g_ready && (g_dialog.visible || g_alpha > 0))
        ImGui_ImplSDL2_ProcessEvent(static_cast<const SDL_Event *>(sdl_event));
}

void overlay_render() {
    if (!g_ready) return;
    uint64_t now = SDL_GetTicks64() * 1000;
    float dt = g_last_us ? (now - g_last_us) / 1e6f : 0;
    g_last_us = now;
    float target = g_dialog.visible ? 1.f : 0.f;
    g_alpha = g_alpha < target ? std::min(target, g_alpha + dt / 0.15f) : std::max(target, g_alpha - dt / 0.15f);
    if (g_alpha <= 0) return;

    // El atlas de la fuente se regenera al tamaño real en pantalla para que el texto sea nítido.
    int ww, wh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    float px = std::round(14 * std::min(ww / 480.f, wh / 272.f));
    if (std::fabs(px - g_font_px) >= 1) build_font(px);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    draw_dialog(ImGui::GetForegroundDrawList());
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
