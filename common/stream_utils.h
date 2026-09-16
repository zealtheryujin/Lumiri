#pragma once
#include "protocol.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace rp {
inline float pcmSample(const uint8_t* p, unsigned bits) {
    if (bits == 16) { int16_t v; std::memcpy(&v, p, 2); return v / 32768.f; }
    if (bits == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        return v / 8388608.f;
    }
    if (bits == 32) { int32_t v; std::memcpy(&v, p, 4); return v / 2147483648.f; }
    return 0.f;
}

inline uint32_t audioChunkFrames(uint32_t rate) {
    return std::max(1u, std::min(rate / 200, (uint32_t)RP_MAX_PAYLOAD / 4));
}

inline bool validVideoLayout(uint32_t bytes, uint16_t parts) {
    return bytes && bytes <= 8u * 1024 * 1024 &&
           parts == (bytes + RP_MAX_PAYLOAD - 1) / RP_MAX_PAYLOAD;
}

inline bool newerSequence(uint32_t value, uint32_t previous) {
    uint32_t delta = value - previous;
    return delta != 0 && delta < 0x80000000u;
}

template<class Presence>
inline int recoverFecGroup(uint8_t* frame, const uint8_t* parity,
                           const Presence& have, uint16_t parts, size_t group) {
    size_t lo = group * RP_FEC_GROUP;
    size_t hi = std::min(lo + RP_FEC_GROUP, (size_t)parts);
    int missing = -1;
    for (size_t i = lo; i < hi; ++i) {
        if (!have[i]) {
            if (missing >= 0) return -1;
            missing = (int)i;
        }
    }
    if (missing < 0) return -1;
    uint8_t* dst = frame + (size_t)missing * RP_MAX_PAYLOAD;
    std::copy_n(parity, RP_MAX_PAYLOAD, dst);
    for (size_t i = lo; i < hi; ++i) {
        if ((int)i == missing) continue;
        const uint8_t* src = frame + i * RP_MAX_PAYLOAD;
        for (size_t k = 0; k < RP_MAX_PAYLOAD; ++k) dst[k] ^= src[k];
    }
    return missing;
}
}
