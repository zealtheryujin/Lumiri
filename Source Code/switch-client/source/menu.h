#pragma once
#include "ui.h"

struct MenuTile {
    const char* name;
    const char* badge;
    SDL_Texture* icon;
    int kind;
};

void drawLibrary(const std::vector<MenuTile>& tiles, int selected, int top,
                 const char* host, const char* ip, bool loading, int hostCount);
void drawPreferences(const char* const* names, const char values[][32], int selected, bool streaming);
void drawFolder(const std::vector<RpBrowseEntry>& entries, int selected, int top, const char* path, bool failed);
void drawConnectionState(const char* title, const char* detail, bool error);
void drawSessionMenu(const char* const* labels, int selected, int count);
