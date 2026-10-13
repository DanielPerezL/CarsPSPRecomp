#include "window.h"

#include <SDL2/SDL.h>

#include <cmath>

#include "../core.h"
#include "../gpu/gl_funcs.h"

#define CARS_GL_DEFINE(type, name) type name = nullptr;
CARS_GL_FUNCS(CARS_GL_DEFINE)
#undef CARS_GL_DEFINE

bool gl_load_functions() {
    bool ok = true;
#define CARS_GL_LOAD(type, name)                                          \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));          \
    if (!name) { LOGE("gl", "falta la función %s", #name); ok = false; }
    CARS_GL_FUNCS(CARS_GL_LOAD)
#undef CARS_GL_LOAD
    return ok;
}

static SDL_Window *g_window = nullptr;
static SDL_GLContext g_gl = nullptr;
static SDL_GameController *g_pad = nullptr;

enum : uint32_t {
    BTN_SELECT = 0x1, BTN_START = 0x8, BTN_UP = 0x10, BTN_RIGHT = 0x20, BTN_DOWN = 0x40, BTN_LEFT = 0x80,
    BTN_L = 0x100, BTN_R = 0x200, BTN_TRIANGLE = 0x1000, BTN_CIRCLE = 0x2000, BTN_CROSS = 0x4000,
    BTN_SQUARE = 0x8000,
};

bool window_init(const WindowConfig &cfg) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOGE("window", "SDL_Init: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;
    g_window = SDL_CreateWindow("Cars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height, flags);
    if (!g_window) {
        LOGE("window", "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    g_gl = SDL_GL_CreateContext(g_window);
    if (!g_gl) {
        LOGE("window", "contexto OpenGL 3.3 core: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);
    if (!gl_load_functions()) return false;
    LOGI("window", "OpenGL %s (%s)", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (g_pad = SDL_GameControllerOpen(i))) {
            LOGI("window", "mando: %s", SDL_GameControllerName(g_pad));
            break;
        }
    return true;
}

void window_shutdown() {
    if (g_gl) SDL_GL_DeleteContext(g_gl);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

bool window_poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
            bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    return true;
}

void window_swap() { SDL_GL_SwapWindow(g_window); }

void window_drawable_size(int *w, int *h) { SDL_GL_GetDrawableSize(g_window, w, h); }

static uint8_t axis_to_psp(int v) {
    int x = (v + 32768) >> 8;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

PadState window_pad() {
    PadState p;
    const uint8_t *k = SDL_GetKeyboardState(nullptr);
    static const struct { SDL_Scancode sc; uint32_t btn; } keys[] = {
        {SDL_SCANCODE_UP, BTN_UP}, {SDL_SCANCODE_DOWN, BTN_DOWN}, {SDL_SCANCODE_LEFT, BTN_LEFT},
        {SDL_SCANCODE_RIGHT, BTN_RIGHT}, {SDL_SCANCODE_Z, BTN_CROSS}, {SDL_SCANCODE_X, BTN_CIRCLE},
        {SDL_SCANCODE_A, BTN_SQUARE}, {SDL_SCANCODE_S, BTN_TRIANGLE}, {SDL_SCANCODE_Q, BTN_L},
        {SDL_SCANCODE_W, BTN_R}, {SDL_SCANCODE_RETURN, BTN_START}, {SDL_SCANCODE_RSHIFT, BTN_SELECT},
        {SDL_SCANCODE_BACKSPACE, BTN_SELECT}};
    for (auto &m : keys)
        if (k[m.sc]) p.buttons |= m.btn;
    // stick con IJKL
    if (k[SDL_SCANCODE_J]) p.lx = 0;
    if (k[SDL_SCANCODE_L]) p.lx = 255;
    if (k[SDL_SCANCODE_I]) p.ly = 0;
    if (k[SDL_SCANCODE_K]) p.ly = 255;
    if (g_pad) {
        static const struct { SDL_GameControllerButton b; uint32_t btn; } pad[] = {
            {SDL_CONTROLLER_BUTTON_A, BTN_CROSS}, {SDL_CONTROLLER_BUTTON_B, BTN_CIRCLE},
            {SDL_CONTROLLER_BUTTON_X, BTN_SQUARE}, {SDL_CONTROLLER_BUTTON_Y, BTN_TRIANGLE},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BTN_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R},
            {SDL_CONTROLLER_BUTTON_START, BTN_START}, {SDL_CONTROLLER_BUTTON_BACK, BTN_SELECT},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, BTN_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BTN_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BTN_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BTN_RIGHT}};
        for (auto &m : pad)
            if (SDL_GameControllerGetButton(g_pad, m.b)) p.buttons |= m.btn;
        int lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (std::abs(lx) > 4000) p.lx = axis_to_psp(lx);
        if (std::abs(ly) > 4000) p.ly = axis_to_psp(ly);
        // gatillos analógicos como L/R
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) p.buttons |= BTN_L;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) p.buttons |= BTN_R;
    }
    return p;
}
