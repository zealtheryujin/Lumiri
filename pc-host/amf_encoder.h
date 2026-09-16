#pragma once
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <vector>

class AmfEncoder {
public:
    AmfEncoder();
    ~AmfEncoder();
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    unsigned width, unsigned height, unsigned fps, unsigned bitrateKbps);
    bool submit(ID3D11Texture2D* texture, uint32_t frameId, bool key);
    bool receive(uint32_t frameId, std::vector<uint8_t>& bytes, bool& key);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
