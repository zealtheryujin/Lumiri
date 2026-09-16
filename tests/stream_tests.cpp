#include "../common/stream_utils.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    for (unsigned bits : {16u, 24u, 32u}) {
        uint8_t sample[4] = {};
        assert(rp::pcmSample(sample, bits) == 0.f);
        sample[bits / 8 - 1] = 0x80;
        assert(rp::pcmSample(sample, bits) == -1.f);
        sample[bits / 8 - 1] = 0x40;
        assert(rp::pcmSample(sample, bits) == 0.5f);
        sample[bits / 8 - 1] = 0xc0;
        assert(rp::pcmSample(sample, bits) == -0.5f);
    }
    for (uint32_t rate : {8000u, 44100u, 48000u, 88200u, 96000u, 192000u, 384000u}) {
        uint32_t frames = rp::audioChunkFrames(rate);
        assert(frames > 0 && frames * 4 <= RP_MAX_PAYLOAD);
        assert(frames * 200 <= rate);

        uint32_t recovered = 0;
        for (uint32_t offset = 0; offset < rate; offset += frames)
            recovered += std::min(frames, rate - offset);
        assert(recovered == rate);
    }
    assert(rp::newerSequence(0, UINT32_MAX));
    assert(rp::newerSequence(8, UINT32_MAX - 8));
    assert(!rp::newerSequence(UINT32_MAX, 0));
    assert(!rp::newerSequence(5, 5));
    assert(!rp::newerSequence(5, 6));
    assert(!rp::validVideoLayout(0, 0));
    assert(!rp::validVideoLayout(1400, 2));
    assert(!rp::validVideoLayout(UINT32_MAX, UINT16_MAX));

    unsigned repaired = 0;
    for (uint32_t bytes : {1u, 1399u, 1400u, 1401u, 5600u, 5601u, 12701u, 65536u}) {
        uint16_t parts = (uint16_t)((bytes + RP_MAX_PAYLOAD - 1) / RP_MAX_PAYLOAD);
        assert(rp::validVideoLayout(bytes, parts));
        std::vector<uint8_t> original((size_t)parts * RP_MAX_PAYLOAD, 0);
        for (size_t i = 0; i < bytes; ++i) original[i] = (uint8_t)(i * 73 + i / 17);
        for (size_t group = 0; group * RP_FEC_GROUP < parts; ++group) {
            size_t lo = group * RP_FEC_GROUP, hi = std::min(lo + RP_FEC_GROUP, (size_t)parts);
            std::vector<uint8_t> parity(RP_MAX_PAYLOAD, 0);
            for (size_t i = lo; i < hi; ++i)
                for (size_t k = 0; k < RP_MAX_PAYLOAD; ++k)
                    parity[k] ^= original[i * RP_MAX_PAYLOAD + k];
            for (size_t loss = lo; loss < hi; ++loss) {
                auto frame = original;
                std::vector<bool> have(parts, true);
                have[loss] = false;
                std::fill_n(frame.data() + loss * RP_MAX_PAYLOAD, RP_MAX_PAYLOAD, uint8_t{0});
                assert(rp::recoverFecGroup(frame.data(), parity.data(), have, parts, group) == (int)loss);
                assert(frame == original);
                have[loss] = true;
                assert(rp::recoverFecGroup(frame.data(), parity.data(), have, parts, group) == -1);
                ++repaired;
            }
            if (hi - lo >= 2) {
                auto frame = original;
                std::vector<bool> have(parts, true);
                have[lo] = have[lo + 1] = false;
                assert(rp::recoverFecGroup(frame.data(), parity.data(), have, parts, group) == -1);
                assert(frame == original);
            }
        }
    }
    std::printf("PASS: %u FEC loss scenarios; multi-loss refusal; PCM 16/24/32; audio rates; sequence rollover; layout validation\n", repaired);
}
