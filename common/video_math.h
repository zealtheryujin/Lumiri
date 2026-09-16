#pragma once
#include <cstdint>

namespace rp {
struct YuvMatrix { float r[4], g[4], b[4]; };
inline YuvMatrix yuvMatrix(bool full, bool bt709) {
    YuvMatrix p{};
    float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f;
    float kg = 1 - kr - kb, ys = full ? 1.f : 255.f / 219.f, cs = full ? 1.f : 255.f / 224.f;
    float yo = full ? 0.f : 16.f / 255.f, co = 128.f / 255.f;
    p.r[0] = p.g[0] = p.b[0] = ys;
    p.r[2] = 2 * (1 - kr) * cs;
    p.g[1] = -2 * kb * (1 - kb) / kg * cs;
    p.g[2] = -2 * kr * (1 - kr) / kg * cs;
    p.b[1] = 2 * (1 - kb) * cs;
    p.r[3] = -ys * yo - p.r[2] * co;
    p.g[3] = -ys * yo - (p.g[1] + p.g[2]) * co;
    p.b[3] = -ys * yo - p.b[1] * co;
    return p;
}
inline bool planeRangeValid(uint64_t offset, uint64_t planeBytes, uint64_t mapBytes, uint32_t alignment) {
    return alignment && planeBytes && offset < mapBytes && offset % alignment == 0 &&
           planeBytes <= mapBytes - offset;
}
inline void textureScale(float (&v)[4], unsigned visibleW, unsigned visibleH, unsigned texW, unsigned texH) {
    v[0] = float(visibleW) / texW; v[1] = float(visibleH) / texH;
    v[2] = 0.5f / texW; v[3] = 0.5f / texH;
}
}
