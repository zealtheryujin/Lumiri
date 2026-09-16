#pragma once
#include "app.h"

bool uiInit();
void uiExit();

enum FontSize { FS_SMALL = 0, FS_BODY, FS_TITLE, FS_BIG, FS_COUNT };

void drawText(int x, int y, FontSize fs, SDL_Color c, const char* fmt, ...);
int  textWidth(FontSize fs, const char* s);
void fillRect(int x, int y, int w, int h, SDL_Color c);
void fillRoundedRect(int x, int y, int w, int h, int radius, SDL_Color c);
void drawBackground();
void drawHeader(const char* subtitle);
void drawHints(const char* hints);
void drawFocus(int channel, int x, int y, int w, int h, int radius = 20);
void drawClippedText(int x, int y, int width, FontSize fs, SDL_Color color, const char* text);
void drawWrappedText(int x, int y, int width, int maxLines, FontSize fs, SDL_Color color, const char* text);
