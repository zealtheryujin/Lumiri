#include "../common/video_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

static float sample(const float (&row)[4], float y, float u, float v) {
    return row[0] * y + row[1] * u + row[2] * v + row[3];
}
int main() {

    unsigned colors = 0;
    for (bool full : {false, true}) for (bool bt709 : {false, true}) {
        auto m = rp::yuvMatrix(full, bt709);
        float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f;
        for (float r : {0.f, 0.25f, 1.f}) for (float g : {0.f, 0.5f, 1.f}) for (float b : {0.f, 0.75f, 1.f}) {
            float y = kr * r + (1 - kr - kb) * g + kb * b;
            float u = (b - y) / (2 * (1 - kb)), v = (r - y) / (2 * (1 - kr));
            float yy = full ? y : (16 + y * 219) / 255;
            float uu = 128.f / 255 + u * (full ? 1 : 224.f / 255);
            float vv = 128.f / 255 + v * (full ? 1 : 224.f / 255);
            assert(std::abs(sample(m.r, yy, uu, vv) - r) < 0.000002f);
            assert(std::abs(sample(m.g, yy, uu, vv) - g) < 0.000002f);
            assert(std::abs(sample(m.b, yy, uu, vv) - b) < 0.000002f);
            ++colors;
        }
    }

    const unsigned sizes[][2] = {{854, 480}, {960, 540}, {1280, 720}, {1920, 1080}};
    for (auto& size : sizes) {
        unsigned pitch = (size[0] + 63) & ~63u, allocationH = (size[1] + 31) & ~31u;
        uint64_t uvOffset = uint64_t(pitch) * allocationH, bytes = uvOffset * 3 / 2;
        assert(rp::planeRangeValid(0, uvOffset, bytes, 512));
        assert(rp::planeRangeValid(uvOffset, uvOffset / 2, bytes, 512));
        float scales[4]; rp::textureScale(scales, size[0], size[1], pitch, size[1]);
        assert(scales[0] <= 1 && scales[1] == 1);

        assert(std::abs((scales[0] - scales[2]) * pitch - (size[0] - 0.5f)) < 0.0002f);
    }
    assert(!rp::planeRangeValid(UINT64_MAX - 3, 8, UINT64_MAX, 1));
    assert(!rp::planeRangeValid(1024, 1, 1024, 512));
    assert(!rp::planeRangeValid(1, 512, 4096, 512));
    assert(!rp::planeRangeValid(0, 4097, 4096, 512));
    assert(!rp::planeRangeValid(0, 0, 4096, 512));
    std::printf("PASS: %u YUV color scenarios; four NV12 layouts; padded sampling; map bounds/overflow\n", colors);
}
