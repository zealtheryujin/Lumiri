#pragma once
#include <d3d11.h>
#include <wrl/client.h>

void logmsg(const char* fmt, ...);

inline bool d3dOk(HRESULT result, const char* stage) {
    if (SUCCEEDED(result)) return true;
    logmsg("AMD conversion %s failed: 0x%08lX", stage, result);
    return false;
}

class GpuNv12Converter {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
public:
    bool initialize(ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                    unsigned width, unsigned height, unsigned fps) {
        auto& p = *this;
        p.immediate = deviceContext;
        if (!d3dOk(device->QueryInterface(IID_PPV_ARGS(&p.videoDevice)), "video device") ||
            !d3dOk(deviceContext->QueryInterface(IID_PPV_ARGS(&p.videoContext)), "video context")) return false;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {fps, 1}; desc.OutputFrameRate = {fps, 1};
        desc.InputWidth = desc.OutputWidth = width;
        desc.InputHeight = desc.OutputHeight = height;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (!d3dOk(p.videoDevice->CreateVideoProcessorEnumerator(&desc, &p.enumerator), "enumerator")) return false;
        UINT inputSupport = 0, outputSupport = 0;
        if (!d3dOk(p.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inputSupport), "BGRA support") ||
            !d3dOk(p.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputSupport), "NV12 support") ||
            !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            logmsg("AMD GPU does not support the required BGRA to NV12 video conversion"); return false;
        }
        if (!d3dOk(p.videoDevice->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor), "processor")) return false;
        RECT rect{0, 0, LONG(width), LONG(height)};
        p.videoContext->VideoProcessorSetStreamFrameFormat(p.processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        p.videoContext->VideoProcessorSetStreamSourceRect(p.processor.Get(), 0, TRUE, &rect);
        p.videoContext->VideoProcessorSetStreamDestRect(p.processor.Get(), 0, TRUE, &rect);
        p.videoContext->VideoProcessorSetOutputTargetRect(p.processor.Get(), TRUE, &rect);
        p.videoContext->VideoProcessorSetStreamAutoProcessingMode(p.processor.Get(), 0, FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
        rgb.RGB_Range = 0; rgb.YCbCr_Matrix = 1; rgb.Nominal_Range = 2;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
        yuv.YCbCr_Matrix = 1; yuv.Nominal_Range = 1;
        p.videoContext->VideoProcessorSetStreamColorSpace(p.processor.Get(), 0, &rgb);
        p.videoContext->VideoProcessorSetOutputColorSpace(p.processor.Get(), &yuv);
        return true;
    }
    bool convert(ID3D11Texture2D* texture, ID3D11Texture2D* nv12) {
        auto& p = *this;
        ComPtr<ID3D11VideoProcessorInputView> input;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (!d3dOk(p.videoDevice->CreateVideoProcessorInputView(texture, p.enumerator.Get(), &inDesc, &input), "input view") ||
            !d3dOk(p.videoDevice->CreateVideoProcessorOutputView(nv12, p.enumerator.Get(), &outDesc, &output), "output view")) return false;
        p.immediate->OMSetRenderTargets(0, nullptr, nullptr);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = input.Get();
        if (!d3dOk(p.videoContext->VideoProcessorBlt(p.processor.Get(), output.Get(), 0, 1, &stream), "BGRA to NV12")) return false;
        p.immediate->Flush();
        return true;
    }
};
