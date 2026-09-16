#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../pc-host/gpu_nv12.h"
#include "../pc-host/amf_encoder.h"
#include "../pc-host/common.h"
#include <cstdarg>
#include <cstdlib>
#include <vector>

Session g_session;
void logmsg(const char* format, ...) {
    va_list args; va_start(args, format); vprintf(format, args); va_end(args); puts("");
}

int main(int argc, char**) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context))) return 1;
    if (argc > 1) {
        AmfEncoder encoder;
        bool initialized = encoder.initialize(device.Get(), context.Get(), 1280, 720, 60, 8000);
        encoder.shutdown();
        if (initialized) { puts("Unexpected AMF initialization; this check requires an unsupported adapter/runtime"); return 1; }
        puts("PASS: unsupported AMF initialization and cleanup"); return 0;
    }
    GpuNv12Converter converter;
    const unsigned w = 1280, h = 720;
    if (!converter.initialize(device.Get(), context.Get(), w, h, 60)) return 1;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> input, output, readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &input))) return 1;
    desc.Format = DXGI_FORMAT_NV12;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &output))) return 1;
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return 1;
    struct Color { uint32_t bgra; int y, u, v; };
    Color colors[] = {{0xFF000000,16,128,128},{0xFFFFFFFF,235,128,128},
        {0xFFFF0000,63,102,240},{0xFF00FF00,173,42,26},{0xFF0000FF,32,240,118}};
    for (auto color : colors) {
        std::vector<uint32_t> pixels(w * h, color.bgra);
        context->UpdateSubresource(input.Get(), 0, nullptr, pixels.data(), w * 4, 0);
        if (!converter.convert(input.Get(), output.Get())) return 1;
        context->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map))) return 1;
        auto* data = static_cast<uint8_t*>(map.pData);
        int y = data[(h / 2) * map.RowPitch + w / 2];
        int u = data[h * map.RowPitch + (h / 4) * map.RowPitch + w / 2];
        int v = data[h * map.RowPitch + (h / 4) * map.RowPitch + w / 2 + 1];
        context->Unmap(readback.Get(), 0);
        if (abs(y-color.y)>3 || abs(u-color.u)>3 || abs(v-color.v)>3) {
            printf("FAIL: %08X got YUV %d,%d,%d expected %d,%d,%d\n", color.bgra,y,u,v,color.y,color.u,color.v); return 1;
        }
    }
    puts("PASS: production GPU BGRA -> NV12 conversion, BT.709 limited range, black/white/R/G/B");
    return 0;
}
