#include "ui.h"
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

static TTF_Font* g_fonts[FS_COUNT] = {};
static const int g_sizes[FS_COUNT] = { 18, 23, 32, 46 };
#ifndef LUMIRI_UI_PREVIEW
static PlFontData g_fontData;
#endif
struct CachedText {
    std::string text;
    FontSize font;
    uint32_t rgba;
    SDL_Texture* texture;
    int w, h;
    uint64_t used;
};
static std::vector<CachedText> g_textCache;
static uint64_t g_textTick = 0;

bool uiInit() {
    if (TTF_Init() != 0) return false;
#ifndef LUMIRI_UI_PREVIEW
    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    if (R_FAILED(plGetSharedFontByType(&g_fontData, PlSharedFontType_Standard))) return false;
    for (int i = 0; i < FS_COUNT; i++) {
        SDL_RWops* rw = SDL_RWFromMem(g_fontData.address, g_fontData.size);
        g_fonts[i] = TTF_OpenFontRW(rw, 1, g_sizes[i]);
        if (!g_fonts[i]) return false;
    }
#else
    for (int i = 0; i < FS_COUNT; ++i) {
        g_fonts[i] = TTF_OpenFont("C:/Windows/Fonts/segoeui.ttf", g_sizes[i]);
        if (!g_fonts[i]) return false;
    }
#endif
    return true;
}

void uiExit() {
    for (auto& entry : g_textCache) SDL_DestroyTexture(entry.texture);
    g_textCache.clear();
    for (auto& f : g_fonts) if (f) { TTF_CloseFont(f); f = nullptr; }
    TTF_Quit();
#ifndef LUMIRI_UI_PREVIEW
    plExit();
#endif
}

void drawText(int x, int y, FontSize fs, SDL_Color c, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!buf[0]) return;
    uint32_t rgba = uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) | (uint32_t(c.a) << 24);
    for (auto& entry : g_textCache) {
        if (entry.font == fs && entry.rgba == rgba && entry.text == buf) {
            entry.used = ++g_textTick;
            SDL_Rect dst{x, y, entry.w, entry.h};
            SDL_RenderCopy(g_ren, entry.texture, nullptr, &dst);
            return;
        }
    }
    SDL_Surface* s = TTF_RenderUTF8_Blended(g_fonts[fs], buf, c);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_Rect dst{ x, y, s->w, s->h };
    SDL_FreeSurface(s);
    if (t) {
        SDL_RenderCopy(g_ren, t, nullptr, &dst);
        if (g_textCache.size() >= 128) {
            auto oldest = std::min_element(g_textCache.begin(), g_textCache.end(),
                [](const CachedText& a, const CachedText& b) { return a.used < b.used; });
            SDL_DestroyTexture(oldest->texture);
            g_textCache.erase(oldest);
        }
        g_textCache.push_back({buf, fs, rgba, t, dst.w, dst.h, ++g_textTick});
    }
}

int textWidth(FontSize fs, const char* s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(g_fonts[fs], s, &w, &h);
    return w;
}

void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r{ x, y, w, h };
    SDL_RenderFillRect(g_ren, &r);
}

void fillRoundedRect(int x, int y, int w, int h, int rad, SDL_Color c) {
    if (rad * 2 > h) rad = h / 2;
    if (rad * 2 > w) rad = w / 2;
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);

    SDL_Rect mid{ x, y + rad, w, h - 2 * rad };
    SDL_RenderFillRect(g_ren, &mid);

    for (int i = 0; i < rad; i++) {
        int dx = (int)(rad - std::sqrt((double)rad * rad - (double)(rad - i) * (rad - i)));
        SDL_Rect top{ x + dx, y + i, w - 2 * dx, 1 };
        SDL_Rect bot{ x + dx, y + h - 1 - i, w - 2 * dx, 1 };
        SDL_RenderFillRect(g_ren, &top);
        SDL_RenderFillRect(g_ren, &bot);
    }
}


void drawBackground() { fillRect(0, 0, 1280, 720, theme::bgTop); }

void drawHeader(const char* subtitle) {
    fillRoundedRect(48, 30, 42, 42, 15, theme::accent);
    fillRoundedRect(60, 40, 6, 23, 3, theme::card);
    fillRoundedRect(60, 57, 18, 6, 3, theme::card);
    drawText(103, 30, FS_TITLE, theme::text, "lumiri");
    if (subtitle) drawText(1224 - textWidth(FS_SMALL, subtitle), 43, FS_SMALL, theme::dim, "%s", subtitle);
}

void drawHints(const char* hints) {
    int w = textWidth(FS_SMALL, hints);
    fillRoundedRect((1280 - w - 48) / 2, 664, w + 48, 40, 20, theme::card);
    drawText((1280 - w) / 2, 674, FS_SMALL, theme::dim, "%s", hints);
}

void drawClippedText(int x, int y, int width, FontSize fs, SDL_Color color, const char* text) {
    std::string s = text ? text : "";
    if (textWidth(fs, s.c_str()) > width) {
        while (!s.empty() && textWidth(fs, (s + "…").c_str()) > width) {
            size_t end = s.size() - 1;
            while (end && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
            s.resize(end);
        }
        s += "…";
    }
    drawText(x, y, fs, color, "%s", s.c_str());
}

void drawWrappedText(int x, int y, int width, int maxLines, FontSize fs, SDL_Color color, const char* text) {
    std::string rest = text ? text : "";
    for (int line = 0; line < maxLines && !rest.empty(); ++line) {
        if (line == maxLines - 1 || textWidth(fs, rest.c_str()) <= width) {
            drawClippedText(x, y, width, fs, color, rest.c_str()); break;
        }
        size_t end = rest.size(), split = rest.rfind(' ');
        while (split != std::string::npos) {
            if (textWidth(fs, rest.substr(0, split).c_str()) <= width) { end = split; break; }
            if (!split) break;
            split = rest.rfind(' ', split - 1);
        }
        if (end == rest.size()) {
            while (end > 0 && textWidth(fs, rest.substr(0, end).c_str()) > width) {
                --end;
                while (end && (static_cast<unsigned char>(rest[end]) & 0xC0) == 0x80) --end;
            }
        }
        if (!end) break;
        drawText(x, y, fs, color, "%s", rest.substr(0, end).c_str());
        rest.erase(0, end);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        y += g_sizes[fs] + 9;
    }
}

void drawFocus(int channel, int x, int y, int w, int h, int radius) {
    struct Focus { float x=0, y=0; uint64_t last=0; };
    static Focus states[4];
    auto& state = states[channel & 3];
    uint64_t now = nowMs();
    if (!state.last || now - state.last > 100) { state.x = float(x); state.y = float(y); }
    float t = 1.f - std::exp(-float(std::min<uint64_t>(now - state.last, 50)) / 42.f);
    state.x += (x - state.x) * t; state.y += (y - state.y) * t; state.last = now;
    int left = int(std::round(state.x)) - 3, top = int(std::round(state.y)) - 3;
    SDL_SetRenderDrawColor(g_ren, theme::accent.r, theme::accent.g, theme::accent.b, 255);
    for (int thickness = 0; thickness < 2; ++thickness) {
        int r = radius + 3 - thickness;
        int l = left + thickness, t0 = top + thickness, right = left + w + 6 - thickness, bottom = top + h + 6 - thickness;
        SDL_Point points[37];
        for (int corner = 0; corner < 4; ++corner) {
            float cx = float(corner == 0 || corner == 3 ? l + r : right - r);
            float cy = float(corner < 2 ? t0 + r : bottom - r);
            for (int i = 0; i <= 8; ++i) {
                float angle = (180.f + corner * 90.f + i * 90.f / 8.f) * 3.14159265f / 180.f;
                points[corner * 9 + i] = {int(std::round(cx + r * std::cos(angle))), int(std::round(cy + r * std::sin(angle)))};
            }
        }
        points[36] = points[0]; SDL_RenderDrawLines(g_ren, points, 37);
    }
}
