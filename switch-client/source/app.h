#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#ifndef LUMIRI_UI_PREVIEW
#include <switch.h>
#endif
#include <vector>
#include <string>
#include <atomic>
#include "../../common/protocol.h"
#include "language.h"

struct Settings {
    bool english = true;
    bool darkMode = false;
    int resIdx = 2;
    int fps = 60;
    int bitrateMbps = 8;
    bool audio = true;
    bool maxPerformance = true;
    bool hwDecode = true;
    bool directVideo = false;
    int mouseSens = 5;
    bool clocks = true;
    int cpuMax = -1;
    int gpuMax = -1;
    int ramMHz = 2133;
    void load();
    void save() const;
    uint16_t width() const {
        static const uint16_t w[] = { 854, 960, 1280, 1920 };
        return w[resIdx & 3];
    }
    uint16_t height() const {
        static const uint16_t h[] = { 480, 540, 720, 1080 };
        return h[resIdx & 3];
    }
};

struct HostInfo {
    char name[33];
    char ip[16];
    uint64_t lastSeenMs;
};

namespace theme {
    inline SDL_Color bgTop    = { 239, 243, 244, 255 };
    inline SDL_Color bgBottom = { 239, 243, 244, 255 };
    inline SDL_Color card     = { 253, 254, 254, 255 };
    inline SDL_Color cardSel  = { 219, 242, 234, 255 };
    inline SDL_Color accent   = { 20, 126, 103, 255 };
    inline SDL_Color text     = { 30, 44, 46, 255 };
    inline SDL_Color dim      = { 91, 108, 110, 255 };
    inline SDL_Color danger   = { 183, 58, 66, 255 };
    inline SDL_Color shadow = {222,230,230,255};
    inline SDL_Color errorBg = {251,230,229,255};
    inline void apply(bool dark) {
        bgTop = bgBottom = dark ? SDL_Color{19,25,29,255} : SDL_Color{239,243,244,255};
        card = dark ? SDL_Color{29,38,43,255} : SDL_Color{253,254,254,255};
        cardSel = dark ? SDL_Color{35,66,60,255} : SDL_Color{219,242,234,255};
        accent = dark ? SDL_Color{111,222,187,255} : SDL_Color{20,126,103,255};
        text = dark ? SDL_Color{234,242,241,255} : SDL_Color{30,44,46,255};
        dim = dark ? SDL_Color{163,181,184,255} : SDL_Color{91,108,110,255};
        danger = dark ? SDL_Color{255,137,143,255} : SDL_Color{183,58,66,255};
        shadow = dark ? SDL_Color{12,17,20,255} : SDL_Color{222,230,230,255};
        errorBg = dark ? SDL_Color{75,39,44,255} : SDL_Color{251,230,229,255};
    }

}

extern SDL_Renderer* g_ren;
extern Settings g_cfg;

inline uint64_t nowMs() { return SDL_GetTicks64(); }
