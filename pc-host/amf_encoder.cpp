#include "common.h"
#include "amf_encoder.h"
#include "gpu_nv12.h"
#include "../third_party/AMF/amf/public/include/core/Factory.h"
#include "../third_party/AMF/amf/public/include/core/Buffer.h"
#include "../third_party/AMF/amf/public/include/components/VideoEncoderVCE.h"

struct AmfEncoder::Impl {
    HMODULE library = nullptr;
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;
    GpuNv12Converter converter;
    unsigned width = 0, height = 0;
    amf_pts duration = 0;

    ~Impl() {
        if (encoder) { encoder->Terminate(); encoder = nullptr; }
        if (context) { context->Terminate(); context = nullptr; }
        if (library) FreeLibrary(library);
    }
};

static bool amfOk(AMF_RESULT result, const char* stage) {
    if (result == AMF_OK) return true;
    logmsg("AMD AMF %s failed: %d", stage, int(result));
    return false;
}

AmfEncoder::AmfEncoder() = default;
AmfEncoder::~AmfEncoder() = default;
void AmfEncoder::shutdown() { impl.reset(); }

bool AmfEncoder::initialize(ID3D11Device* device, ID3D11DeviceContext* immediate,
                            unsigned width, unsigned height, unsigned fps, unsigned bitrateKbps) {
    impl = std::make_unique<Impl>();
    auto& p = *impl;
    p.width = width; p.height = height; p.duration = 10000000 / fps;
    p.library = LoadLibraryW(AMF_DLL_NAME);
    if (!p.library) {
        logmsg("AMD AMF runtime missing (amfrt64.dll). Install the AMD graphics driver. Error: %lu", GetLastError());
        return false;
    }
    auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(p.library, AMF_INIT_FUNCTION_NAME));
    amf::AMFFactory* factory = nullptr;
    if (!init) { logmsg("AMD AMF runtime has no AMFInit entry point; reinstall the AMD driver"); return false; }
    if (!amfOk(init(AMF_FULL_VERSION, &factory), "runtime initialization")) return false;
    if (!amfOk(factory->CreateContext(&p.context), "context") ||
        !amfOk(p.context->InitDX11(device), "DX11 context") ||
        !amfOk(factory->CreateComponent(p.context, AMFVideoEncoderVCE_AVC, &p.encoder), "H.264 encoder (hardware/driver support required)")) return false;
    auto set = [&](const wchar_t* name, amf_int64 value) {
        AMF_RESULT result = p.encoder->SetProperty(name, value);
        if (result != AMF_OK) logmsg("AMD AMF property %ls failed: %d", name, int(result));
        return result == AMF_OK;
    };
    if (!set(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY) ||
        !set(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_MAIN) ||
        !set(AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED) ||
        !set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR) ||
        !set(AMF_VIDEO_ENCODER_TARGET_BITRATE, amf_int64(bitrateKbps) * 1000) ||
        !set(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, amf_int64(bitrateKbps) * 1000 / fps) ||
        !set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0) ||
        !set(AMF_VIDEO_ENCODER_IDR_PERIOD, 0) ||
        !set(AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709) ||
        !set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709) ||
        !amfOk(p.encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_SKIP_FRAME_ENABLE, false), "disable frame skipping") ||
        !amfOk(p.encoder->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, false), "limited range") ||
        !amfOk(p.encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width, height)), "frame size") ||
        !amfOk(p.encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1)), "frame rate") ||
        !amfOk(p.encoder->Init(amf::AMF_SURFACE_NV12, width, height), "encoder initialization")) return false;
    if (!p.converter.initialize(device, immediate, width, height, fps)) return false;
    logmsg("AMD AMF H.264: GPU BGRA -> NV12, CBR, no B-frames, asynchronous output");
    return true;
}

bool AmfEncoder::submit(ID3D11Texture2D* texture, uint32_t frameId, bool key) {
    auto& p = *impl;
    amf::AMFSurfacePtr surface;
    if (!amfOk(p.context->AllocSurface(amf::AMF_MEMORY_DX11, amf::AMF_SURFACE_NV12,
                                     p.width, p.height, &surface), "NV12 surface")) return false;
    auto* nv12 = static_cast<ID3D11Texture2D*>(surface->GetPlaneAt(0)->GetNative());
    if (!p.converter.convert(texture, nv12)) return false;
    surface->SetPts(amf_pts(frameId) * p.duration);
    surface->SetDuration(p.duration);
    if (key && (!amfOk(surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf_int64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR)), "force IDR") ||
                !amfOk(surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true), "insert SPS") ||
                !amfOk(surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true), "insert PPS"))) return false;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    AMF_RESULT result;
    do {
        result = p.encoder->SubmitInput(surface);
        if (result != AMF_INPUT_FULL) return amfOk(result, "submit");
        Sleep(1);
    } while (!g_session.stop && GetTickCount64() < deadline);
    logmsg("AMD AMF input queue did not become available");
    return false;
}

bool AmfEncoder::receive(uint32_t frameId, std::vector<uint8_t>& bytes, bool& key) {
    auto& p = *impl;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    do {
        amf::AMFDataPtr data;
        AMF_RESULT result = p.encoder->QueryOutput(&data);
        if (result != AMF_OK && result != AMF_REPEAT) return amfOk(result, "output");
        if (data) {
            amf::AMFBufferPtr buffer(data);
            if (!buffer || !buffer->GetNative() || !buffer->GetSize() || data->GetPts() != amf_pts(frameId) * p.duration) {
                logmsg("AMD AMF invalid or reordered output frame"); return false;
            }
            amf_int64 type = -1;
            if (!amfOk(data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type), "output picture type")) return false;
            key = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
            auto* begin = static_cast<const uint8_t*>(buffer->GetNative());
            bytes.assign(begin, begin + buffer->GetSize());
            return true;
        }
        Sleep(1);
    } while (!g_session.stop && GetTickCount64() < deadline);
    if (!g_session.stop) logmsg("AMD AMF output timed out");
    return false;
}
