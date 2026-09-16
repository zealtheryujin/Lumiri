#pragma once
#include <cstdint>
struct AVFrame;
struct SDL_Surface;
struct SDL_Rect;

bool presenterInit();
void presenterExit();
bool presenterSubmit(const AVFrame* frame, void* unused);
bool presenterHasFrame();
void presenterClearVideo();
bool presenterBegin();
bool presenterPresent(SDL_Surface* ui, bool showUi, bool showVideo, const SDL_Rect* uiRegion = nullptr);
const char* presenterPath();
uint32_t presenterUploadUs();
uint32_t presenterVideoCopyBytes();
uint32_t presenterFrameCount();
