#include "common.h"
#include "amf_encoder.h"
#include "../common/video_queue.h"
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#include <ffnvcodec/nvEncodeAPI.h>
#include <vector>
#include <cmath>
#include <thread>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define CK(hr, what) do { HRESULT _hr=(hr); if (FAILED(_hr)) { logmsg("HATA %s: 0x%08X", what, _hr); return false; } } while(0)
#define NVCK(st, what) do { NVENCSTATUS _s=(st); if (_s!=NV_ENC_SUCCESS) { logmsg("NVENC HATA %s: %d", what, _s); return false; } } while(0)
static FILE* g_testOutput = nullptr;
static bool g_testSync = false;
static DWORD g_testSendDelay = 0;
static unsigned g_monitorIndex = 0;
static bool g_testMonitor = false;

static const char* g_shaderSrc = R"(
Texture2D tex0 : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut vsMain(uint id : SV_VertexID) {

    VSOut o; float2 corners[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    o.uv = corners[id];
    o.pos = float4(0,0,0,1);
    return o;
}
cbuffer Rect : register(b0) { float4 rect; };
VSOut vsRect(uint id : SV_VertexID) {
    VSOut o; float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    o.uv = c[id];
    float2 p = rect.xy + c[id] * rect.zw;
    o.pos = float4(p.x*2-1, 1-p.y*2, 0, 1);
    return o;
}
float4 psMain(VSOut i) : SV_TARGET { return tex0.Sample(smp, i.uv); }
)";

struct EncodeSlot {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    NV_ENC_REGISTERED_PTR resource = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    NV_ENC_OUTPUT_PTR output = nullptr;
    HANDLE event = nullptr;
    bool eventRegistered = false;
    std::atomic<bool> busy{false};
    uint32_t frameId = 0;
    uint64_t capturedMs = 0;
    LARGE_INTEGER encodeStart{};
};

struct VideoPipeline {
    AmfEncoder amf;
    bool amd = false;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    UINT deskW = 0, deskH = 0;
    ID3D11Texture2D* deskTex = nullptr;
    ID3D11ShaderResourceView* deskSrv = nullptr;
    bool desktopReady = false;
    ID3D11Texture2D* encTex = nullptr;
    ID3D11RenderTargetView* encRtv = nullptr;

    ID3D11Texture2D* curTex = nullptr;
    ID3D11ShaderResourceView* curSrv = nullptr;
    UINT curW = 0, curH = 0;
    POINT curPos{ 0,0 };
    bool curVisible = false;
    std::vector<uint8_t> shapeBuf;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11Buffer* cbRect = nullptr;

    HMODULE nvLib = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fn{};
    void* enc = nullptr;
    EncodeSlot slots[4];
    bool async = false;
    std::mutex pendingMutex;
    std::condition_variable pendingReady;
    std::deque<unsigned> pending;
    bool producerDone = false;
    std::atomic<unsigned> inFlight{0};

    NV_ENC_INITIALIZE_PARAMS initParams{};
    NV_ENC_CONFIG encCfg{};
    uint32_t curBitrateKbps = 0;
    uint64_t adWindowStart = 0, adCleanSince = 0;
};

static bool createDupe(VideoPipeline& p) {
    IDXGIDevice* dxgiDev = nullptr; p.dev->QueryInterface(&dxgiDev);
    IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter); dxgiDev->Release();
    IDXGIOutput* out = nullptr;
    std::vector<UINT> attached;
    for (UINT i = 0;; ++i) {
        IDXGIOutput* candidate = nullptr;
        if (adapter->EnumOutputs(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        if (!candidate) break;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.AttachedToDesktop) attached.push_back(i);
        candidate->Release();
    }
    if (attached.empty()) { adapter->Release(); logmsg("No attached monitor on capture GPU"); return false; }
    g_monitorIndex %= unsigned(attached.size());
    HRESULT selected = adapter->EnumOutputs(attached[g_monitorIndex], &out);
    if (FAILED(selected)) { adapter->Release(); return false; }
    DXGI_OUTPUT_DESC selectedDesc{}; out->GetDesc(&selectedDesc);
    logmsg("Monitor %u/%u: %ls", g_monitorIndex + 1, unsigned(attached.size()), selectedDesc.DeviceName);
    IDXGIOutput1* out1 = nullptr; out->QueryInterface(&out1); out->Release(); adapter->Release();
    HRESULT hr = out1->DuplicateOutput(p.dev, &p.dup);
    out1->Release();
    CK(hr, "DuplicateOutput");
    DXGI_OUTDUPL_DESC dd; p.dup->GetDesc(&dd);
    p.deskW = dd.ModeDesc.Width; p.deskH = dd.ModeDesc.Height;
    return true;
}

static bool initD3D(VideoPipeline& p) {
    D3D_FEATURE_LEVEL fl;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &p.dev, &fl, &p.ctx), "D3D11CreateDevice");
    ID3D10Multithread* mt = nullptr;
    CK(p.dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt), "D3D multithread interface");
    mt->SetMultithreadProtected(TRUE);
    mt->Release();

    typedef LONG(WINAPI* PFN_SetGpuPrio)(HANDLE, int);
    if (HMODULE gdi = LoadLibraryA("gdi32.dll")) {
        auto setPrio = (PFN_SetGpuPrio)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass");
        if (setPrio) {
            if (setPrio(GetCurrentProcess(), 5  ) == 0)
                logmsg("[OK] GPU zamanlama önceliği: Realtime");
            else if (setPrio(GetCurrentProcess(), 4  ) == 0)
                logmsg("[--] GPU önceliği: High (Realtime için host'u YÖNETİCİ olarak çalıştırın)");
            else
                logmsg("[!!] GPU önceliği yükseltilemedi — oyun içinde fps düşer. Yönetici olarak çalıştırın!");
        }
        FreeLibrary(gdi);
    }
    IDXGIDevice* dxdev = nullptr;
    if (SUCCEEDED(p.dev->QueryInterface(&dxdev))) {
        dxdev->SetGPUThreadPriority(7);
        dxdev->Release();
    }

    if (!createDupe(p)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = p.deskW; td.Height = p.deskH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CK(p.dev->CreateTexture2D(&td, nullptr, &p.deskTex), "deskTex");
    CK(p.dev->CreateShaderResourceView(p.deskTex, nullptr, &p.deskSrv), "deskSrv");

    td.Width = g_session.width; td.Height = g_session.height;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& slot : p.slots) {
        CK(p.dev->CreateTexture2D(&td, nullptr, &slot.texture), "encTex");
        CK(p.dev->CreateRenderTargetView(slot.texture, nullptr, &slot.target), "encRtv");
    }
    p.encTex = p.slots[0].texture; p.encRtv = p.slots[0].target;

    ID3DBlob* blob = nullptr, * err = nullptr;
    CK(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "vsRect", "vs_4_0", 0, 0, &blob, &err), "vs derleme");
    CK(p.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &p.vs), "CreateVS");
    blob->Release();
    CK(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "psMain", "ps_4_0", 0, 0, &blob, &err), "ps derleme");
    CK(p.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &p.ps), "CreatePS");
    blob->Release();

    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    CK(p.dev->CreateSamplerState(&sd, &p.sampler), "sampler");

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CK(p.dev->CreateBlendState(&bd, &p.blend), "blend");

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CK(p.dev->CreateBuffer(&cbd, nullptr, &p.cbRect), "cbRect");
    return true;
}

static bool initNvenc(VideoPipeline& p) {
    p.nvLib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!p.nvLib) { logmsg("nvEncodeAPI64.dll yok — NVIDIA sürücüsü gerekli"); return false; }
    auto maxVer = (NVENCSTATUS(NVENCAPI*)(uint32_t*))
        GetProcAddress(p.nvLib, "NvEncodeAPIGetMaxSupportedVersion");
    uint32_t drvVer = 0;
    if (maxVer && maxVer(&drvVer) == NV_ENC_SUCCESS) {
        logmsg("NVENC: sürücü API %u.%u, başlık API %u.%u",
               drvVer >> 4, drvVer & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        if (drvVer < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
            logmsg("NVENC: sürücü çok eski — NVIDIA sürücüsünü güncelle");
            return false;
        }
    }
    auto create = (NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))
        GetProcAddress(p.nvLib, "NvEncodeAPICreateInstance");
    p.fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVCK(create(&p.fn), "CreateInstance");

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.device = p.dev; sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sp.apiVersion = NVENCAPI_VERSION;
    NVCK(p.fn.nvEncOpenEncodeSessionEx(&sp, &p.enc), "OpenSession");
    NV_ENC_CAPS_PARAM caps{};
    caps.version = NV_ENC_CAPS_PARAM_VER;
    caps.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
    int supported = 0;
    p.async = p.fn.nvEncGetEncodeCaps(p.enc, NV_ENC_CODEC_H264_GUID, &caps, &supported) == NV_ENC_SUCCESS && supported;
    if (g_testSync) p.async = false;

    NV_ENC_PRESET_CONFIG pc{};
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    NVCK(p.fn.nvEncGetEncodePresetConfigEx(p.enc, NV_ENC_CODEC_H264_GUID,
        NV_ENC_PRESET_P3_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc), "PresetConfig");

    NV_ENC_CONFIG& cfg = p.encCfg;
    cfg = pc.presetCfg;
    cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate = g_session.bitrateKbps * 1000;
    cfg.rcParams.maxBitRate = cfg.rcParams.averageBitRate;
    cfg.rcParams.vbvBufferSize = cfg.rcParams.averageBitRate / g_session.fps;
    cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;
    cfg.rcParams.zeroReorderDelay = 1;
    cfg.rcParams.enableLookahead = 0;
    cfg.rcParams.lookaheadDepth = 0;
    cfg.rcParams.enableNonRefP = 1;
    cfg.rcParams.lowDelayKeyFrameScale = 1;
    cfg.rcParams.enableAQ = 1;
    cfg.rcParams.aqStrength = 8;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;

    cfg.encodeCodecConfig.h264Config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;

    cfg.encodeCodecConfig.h264Config.sliceMode = 3;
    cfg.encodeCodecConfig.h264Config.sliceModeData = 4;

    NV_ENC_INITIALIZE_PARAMS& ip = p.initParams;
    ip = {};
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
    ip.presetGUID = NV_ENC_PRESET_P3_GUID;
    ip.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    ip.encodeWidth = g_session.width; ip.encodeHeight = g_session.height;
    ip.darWidth = g_session.width; ip.darHeight = g_session.height;
    ip.maxEncodeWidth = g_session.width; ip.maxEncodeHeight = g_session.height;
    ip.frameRateNum = g_session.fps; ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.enableEncodeAsync = p.async ? 1 : 0;
    ip.encodeConfig = &cfg;
    NVCK(p.fn.nvEncInitializeEncoder(p.enc, &ip), "InitEncoder");
    p.curBitrateKbps = g_session.bitrateKbps;

    for (auto& slot : p.slots) {
    NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
    bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    NVCK(p.fn.nvEncCreateBitstreamBuffer(p.enc, &bb), "CreateBitstream");
    slot.output = bb.bitstreamBuffer;

    NV_ENC_REGISTER_RESOURCE rr{};
    rr.version = NV_ENC_REGISTER_RESOURCE_VER;
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    rr.resourceToRegister = slot.texture;
    rr.width = g_session.width; rr.height = g_session.height;
    rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    rr.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVCK(p.fn.nvEncRegisterResource(p.enc, &rr), "RegisterResource");
    slot.resource = rr.registeredResource;
    if (p.async) {
        slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!slot.event) return false;
        NV_ENC_EVENT_PARAMS ep{}; ep.version = NV_ENC_EVENT_PARAMS_VER;
        ep.completionEvent = slot.event;
        NVCK(p.fn.nvEncRegisterAsyncEvent(p.enc, &ep), "RegisterEvent");
        slot.eventRegistered = true;
    }
    }
    return true;
}

static void updateCursorShape(VideoPipeline& p, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si) {
    std::vector<uint8_t> bgra;
    UINT w = si.Width, h = si.Height;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
        si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        bgra.assign(p.shapeBuf.begin(), p.shapeBuf.begin() + (size_t)si.Pitch * h);

        if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR)
            for (UINT i = 3; i < bgra.size(); i += 4) bgra[i] = bgra[i] ? 0 : 255;
    } else {
        h /= 2;
        bgra.resize((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
            uint8_t andBit = (p.shapeBuf[(size_t)y * si.Pitch + x / 8] >> (7 - x % 8)) & 1;
            uint8_t xorBit = (p.shapeBuf[(size_t)(y + h) * si.Pitch + x / 8] >> (7 - x % 8)) & 1;
            uint8_t* px = &bgra[((size_t)y * w + x) * 4];

            uint8_t v = xorBit ? 255 : 0, a = andBit && !xorBit ? 0 : 255;
            px[0] = px[1] = px[2] = v; px[3] = a;
        }
    }
    if (p.curTex && (p.curW != w || p.curH != h)) {
        p.curSrv->Release(); p.curTex->Release(); p.curTex = nullptr; p.curSrv = nullptr;
    }
    if (!p.curTex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(p.dev->CreateTexture2D(&td, nullptr, &p.curTex))) return;
        p.dev->CreateShaderResourceView(p.curTex, nullptr, &p.curSrv);
        p.curW = w; p.curH = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(p.ctx->Map(p.curTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        UINT srcPitch = (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) ? w * 4 : si.Pitch;
        for (UINT y = 0; y < h; y++)
            memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, bgra.data() + (size_t)y * srcPitch, (size_t)w * 4);
        p.ctx->Unmap(p.curTex, 0);
    }
}

static bool drawQuad(VideoPipeline& p, ID3D11ShaderResourceView* srv,
                     float x, float y, float w, float h, bool blendOn) {
    D3D11_MAPPED_SUBRESOURCE m;
    CK(p.ctx->Map(p.cbRect, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map quad");
    float* r = (float*)m.pData; r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    p.ctx->Unmap(p.cbRect, 0);
    p.ctx->OMSetBlendState(blendOn ? p.blend : nullptr, nullptr, 0xffffffff);
    p.ctx->PSSetShaderResources(0, 1, &srv);
    p.ctx->Draw(4, 0);
    return true;
}

static bool renderFrame(VideoPipeline& p, UINT waitMs) {

    DXGI_OUTDUPL_FRAME_INFO fi{};
    IDXGIResource* res = nullptr;
    const bool changeMonitor = g_session.nextMonitor.exchange(false);
    if (changeMonitor) ++g_monitorIndex;
    HRESULT hr = changeMonitor ? DXGI_ERROR_ACCESS_LOST : p.dup->AcquireNextFrame(waitMs, &fi, &res);
    if (SUCCEEDED(hr)) {
        ID3D11Texture2D* tex = nullptr;
        HRESULT textureHr = res->QueryInterface(&tex); res->Release();
        if (FAILED(textureHr)) { p.dup->ReleaseFrame(); return false; }
        if (!p.desktopReady || fi.LastPresentTime.QuadPart) {
            p.ctx->CopyResource(p.deskTex, tex);
            p.desktopReady = true;
        }
        tex->Release();
        if (fi.LastMouseUpdateTime.QuadPart) {
            p.curVisible = fi.PointerPosition.Visible != 0;
            if (fi.PointerPosition.Visible) p.curPos = fi.PointerPosition.Position;
        }
        if (fi.PointerShapeBufferSize) {
            p.shapeBuf.resize(fi.PointerShapeBufferSize);
            UINT req; DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
            if (SUCCEEDED(p.dup->GetFramePointerShape(fi.PointerShapeBufferSize,
                p.shapeBuf.data(), &req, &si)))
                updateCursorShape(p, si);
        }
        p.dup->ReleaseFrame();
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {

        if (p.dup) { p.dup->Release(); p.dup = nullptr; }
        if (!createDupe(p)) return false;
        p.desktopReady = false;
        p.curVisible = false;
        D3D11_TEXTURE2D_DESC td{};
        p.deskTex->GetDesc(&td);
        if (td.Width != p.deskW || td.Height != p.deskH) {
            td.Width = p.deskW; td.Height = p.deskH;
            ID3D11Texture2D* replacement = nullptr;
            CK(p.dev->CreateTexture2D(&td, nullptr, &replacement), "Resize desktop");
            ID3D11ShaderResourceView* view = nullptr;
            HRESULT viewHr = p.dev->CreateShaderResourceView(replacement, nullptr, &view);
            if (FAILED(viewHr)) { replacement->Release(); return false; }
            p.deskSrv->Release(); p.deskTex->Release();
            p.deskTex = replacement; p.deskSrv = view;
            p.desktopReady = false;
        }
        g_session.wantIdr = true;

        const float black[4] = {0, 0, 0, 1};
        p.ctx->ClearRenderTargetView(p.encRtv, black);
        return true;
    }

    if (!p.desktopReady) {
        const float black[4] = {0, 0, 0, 1};
        p.ctx->ClearRenderTargetView(p.encRtv, black);
        return true;
    }

    if (p.deskW == g_session.width && p.deskH == g_session.height &&
        !(p.curVisible && p.curSrv)) {
        p.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        p.ctx->CopyResource(p.encTex, p.deskTex);
        return true;
    }

    p.ctx->OMSetRenderTargets(1, &p.encRtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)g_session.width, (float)g_session.height, 0, 1 };
    p.ctx->RSSetViewports(1, &vp);
    p.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    p.ctx->VSSetShader(p.vs, nullptr, 0);
    p.ctx->PSSetShader(p.ps, nullptr, 0);
    p.ctx->VSSetConstantBuffers(0, 1, &p.cbRect);
    p.ctx->PSSetSamplers(0, 1, &p.sampler);

    if (!drawQuad(p, p.deskSrv, 0, 0, 1, 1, false)) return false;
    if (p.curVisible && p.curSrv) {
        float sx = 1.0f / p.deskW, sy = 1.0f / p.deskH;
        if (!drawQuad(p, p.curSrv, p.curPos.x * sx, p.curPos.y * sy,
                      p.curW * sx, p.curH * sy, true)) return false;
    }
    return true;
}

static bool submitEncode(VideoPipeline& p, EncodeSlot& slot, uint32_t frameId) {
    if (p.amd) {
        slot.frameId = frameId;
        QueryPerformanceCounter(&slot.encodeStart);
        return p.amf.submit(slot.texture, frameId, g_session.wantIdr.exchange(false));
    }
    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = slot.resource;
    NVCK(p.fn.nvEncMapInputResource(p.enc, &map), "MapInput");
    slot.mapped = map.mappedResource;
    slot.frameId = frameId;
    QueryPerformanceCounter(&slot.encodeStart);

    NV_ENC_PIC_PARAMS pp{};
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputBuffer = map.mappedResource;
    pp.bufferFmt = map.mappedBufferFmt;
    pp.inputWidth = g_session.width; pp.inputHeight = g_session.height;
    pp.outputBitstream = slot.output;
    pp.completionEvent = slot.event;
    pp.inputTimeStamp = frameId;
    pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    if (g_session.wantIdr.exchange(false))
        pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    NVENCSTATUS st = p.fn.nvEncEncodePicture(p.enc, &pp);
    if (st != NV_ENC_SUCCESS) {
        p.fn.nvEncUnmapInputResource(p.enc, slot.mapped); slot.mapped = nullptr;
        logmsg("NVENC submit failed: %d", st); return false;
    }
    return true;
}

static bool collectEncode(VideoPipeline& p, EncodeSlot& slot, rp::VideoQueue& outgoing) {
    rp::EncodedVideo packet;
    packet.id = slot.frameId; packet.capturedMs = slot.capturedMs;
    if (p.amd) {
        if (!p.amf.receive(slot.frameId, packet.bytes, packet.key)) return false;
    } else {
    if (p.async) {

        const DWORD result = WaitForSingleObject(slot.event, 2000);
        if (result != WAIT_OBJECT_0) {
            logmsg("NVENC completion timeout/error: %lu", result);
            return false;
        }
    }
    NV_ENC_LOCK_BITSTREAM lb{};
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = slot.output;
    lb.doNotWait = 0;
    NVENCSTATUS lockStatus = p.fn.nvEncLockBitstream(p.enc, &lb);
    if (lockStatus != NV_ENC_SUCCESS) {
        logmsg("NVENC HATA LockBitstream: %d", lockStatus);
        return false;
    }
    packet.key = lb.pictureType == NV_ENC_PIC_TYPE_IDR;
    auto* data = static_cast<const uint8_t*>(lb.bitstreamBufferPtr);
    packet.bytes.assign(data, data + lb.bitstreamSizeInBytes);

    p.fn.nvEncUnlockBitstream(p.enc, slot.output);
    p.fn.nvEncUnmapInputResource(p.enc, slot.mapped); slot.mapped = nullptr;
    }
    LARGE_INTEGER completed, frequency; QueryPerformanceCounter(&completed); QueryPerformanceFrequency(&frequency);
    g_session.encodeSendUs = uint32_t((completed.QuadPart - slot.encodeStart.QuadPart) * 1000000 / frequency.QuadPart);
    if (!g_session.stop && !outgoing.push(std::move(packet), GetTickCount64())) {
        g_session.wantIdr = true;
        g_session.videoQueueDrops++;
    }
    return true;
}

static void destroyPipeline(VideoPipeline& p) {
    p.amf.shutdown();
    if (p.enc) {
        for (auto& slot : p.slots) {
            if (slot.mapped) p.fn.nvEncUnmapInputResource(p.enc, slot.mapped);
            if (slot.eventRegistered) {
                NV_ENC_EVENT_PARAMS ep{}; ep.version = NV_ENC_EVENT_PARAMS_VER;
                ep.completionEvent = slot.event;
                p.fn.nvEncUnregisterAsyncEvent(p.enc, &ep);
            }
            if (slot.resource) p.fn.nvEncUnregisterResource(p.enc, slot.resource);
            if (slot.output) p.fn.nvEncDestroyBitstreamBuffer(p.enc, slot.output);
        }
        p.fn.nvEncDestroyEncoder(p.enc);
    }
    if (p.nvLib) FreeLibrary(p.nvLib);
    auto rel = [](auto*& x) { if (x) { x->Release(); x = nullptr; } };
    rel(p.cbRect); rel(p.blend); rel(p.sampler); rel(p.ps); rel(p.vs);
    for (auto& slot : p.slots) {
        if (slot.event) CloseHandle(slot.event);
        rel(slot.target); rel(slot.texture);
    }
    rel(p.curSrv); rel(p.curTex);
    rel(p.deskSrv); rel(p.deskTex); rel(p.dup); rel(p.ctx); rel(p.dev);
}

static bool initEncoder(VideoPipeline& p) {
    IDXGIDevice* device = nullptr;
    CK(p.dev->QueryInterface(&device), "capture DXGI device");
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = device->GetAdapter(&adapter); device->Release();
    CK(hr, "capture adapter");
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc); adapter->Release();
    CK(hr, "capture adapter description");
    logmsg("Capture GPU: %ls (vendor 0x%04X)", desc.Description, desc.VendorId);
    if (desc.VendorId == 0x10DE) return initNvenc(p);
    if (desc.VendorId == 0x1002) {
        p.amd = true; p.async = true;
        return p.amf.initialize(p.dev, p.ctx, g_session.width, g_session.height,
                                g_session.fps, g_session.bitrateKbps);
    }
    logmsg("Unsupported capture GPU: Lumiri requires NVIDIA NVENC or AMD AMF H.264 hardware encoding");
    return false;
}

void videoThread() {

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD mmIdx = 0;
    HANDLE mmTask = AvSetMmThreadCharacteristicsW(L"Games", &mmIdx);
    if (mmTask) AvSetMmThreadPriority(mmTask, AVRT_PRIORITY_CRITICAL);

    VideoPipeline p;
    if (!initD3D(p) || !initEncoder(p)) {
        logmsg("Video hattı başlatılamadı, oturum kapatılıyor");
        destroyPipeline(p);
        if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
        g_session.stop = true;
        return;
    }
    logmsg("Video: %ux%u@%u %ukbps (desktop %ux%u)",
        g_session.width, g_session.height, g_session.fps, g_session.bitrateKbps, p.deskW, p.deskH);
    logmsg("Pipeline: %s; 4 GPU buffers, max 2 in-flight; separate UDP sender, queue max 2",
           p.amd ? "AMD AMF async output" : (p.async ? "NVENC async output" : "synchronous NVENC fallback"));
    g_session.videoQueueDrops = 0; g_session.captureSkips = 0; g_session.sendUs = 0;
    rp::VideoQueue outgoing;
    std::thread sender([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        rp::EncodedVideo packet;
        while (outgoing.pop(packet)) {
            if (g_session.stop) break;
            if (GetTickCount64() - packet.capturedMs > 100) {
                outgoing.resync(); g_session.wantIdr = true; g_session.videoQueueDrops++;
                continue;
            }
            LARGE_INTEGER start, end, hz; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&start);
            if (g_testOutput) {
                fwrite(packet.bytes.data(), 1, packet.bytes.size(), g_testOutput);
                if (g_testSendDelay) Sleep(g_testSendDelay);
            } else {
                sendMedia(RP_CH_VIDEO, packet.key ? RP_FLAG_KEYFRAME : 0, packet.id,
                          packet.bytes.data(), packet.bytes.size());
            }
            QueryPerformanceCounter(&end);
            g_session.sendUs = uint32_t((end.QuadPart - start.QuadPart) * 1000000 / hz.QuadPart);
            g_session.videoBytes += packet.bytes.size(); g_session.videoFrames++;
        }
    });
    std::thread output;
    if (p.async) output = std::thread([&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        for (;;) {
            unsigned index;
            {
                std::unique_lock<std::mutex> lock(p.pendingMutex);
                p.pendingReady.wait(lock, [&] { return p.producerDone || !p.pending.empty(); });
                if (p.pending.empty()) break;
                index = p.pending.front(); p.pending.pop_front();
            }
            bool ok = collectEncode(p, p.slots[index], outgoing);
            p.slots[index].busy = false;
            --p.inFlight;
            if (!ok) { g_session.stop = true; break; }
        }
    });

    timeBeginPeriod(1);
    LARGE_INTEGER freq, now; QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const int64_t interval = g_session.unlimitedFps ? 0 : freq.QuadPart / g_session.fps;
    int64_t next = now.QuadPart + interval;
    uint32_t frameId = 0;

    while (!g_session.stop) {

        if (p.inFlight >= 2) { ++g_session.captureSkips; Sleep(2); continue; }
        unsigned index = 0;
        while (index < 4 && p.slots[index].busy.load()) ++index;
        if (index == 4) { Sleep(1); continue; }
        EncodeSlot& slot = p.slots[index];
        p.encTex = slot.texture; p.encRtv = slot.target;

        QueryPerformanceCounter(&now);
        const int64_t periodStart = next - interval;
        while (now.QuadPart < periodStart) {
            int64_t ms = (periodStart - now.QuadPart) * 1000 / freq.QuadPart;
            if (ms > 2) Sleep((DWORD)(ms - 1));
            QueryPerformanceCounter(&now);
        }
        int64_t waitMs = (next - now.QuadPart) * 1000 / freq.QuadPart;
        LARGE_INTEGER captureStart, captureEnd;
        QueryPerformanceCounter(&captureStart);
        if (!renderFrame(p, g_session.unlimitedFps ? 100 : (waitMs > 0 ? (UINT)waitMs : 0))) break;
        QueryPerformanceCounter(&captureEnd);
        slot.capturedMs = GetTickCount64();
        slot.busy = true;
        if (!submitEncode(p, slot, frameId++)) { slot.busy = false; break; }
        if (p.async) {
            ++p.inFlight;
            std::lock_guard<std::mutex> lock(p.pendingMutex);
            p.pending.push_back(index); p.pendingReady.notify_one();
        } else {

            if (!collectEncode(p, slot, outgoing)) { slot.busy = false; break; }
            slot.busy = false;
        }
        g_session.captureUs = (uint32_t)((captureEnd.QuadPart - captureStart.QuadPart) *
                                         1000000 / freq.QuadPart);
        next += interval;
        QueryPerformanceCounter(&now);
        if (now.QuadPart > next) next = now.QuadPart + interval;
    }
    g_session.stop = true;
    if (!p.amd) {
        NV_ENC_PIC_PARAMS eos{}; eos.version = NV_ENC_PIC_PARAMS_VER; eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        p.fn.nvEncEncodePicture(p.enc, &eos);
    }
    {
        std::lock_guard<std::mutex> lock(p.pendingMutex);
        p.producerDone = true; p.pendingReady.notify_all();
    }
    if (output.joinable()) output.join();
    outgoing.close(); sender.join();
    timeEndPeriod(1);
    destroyPipeline(p);
    if (mmTask) AvRevertMmThreadCharacteristics(mmTask);
    g_session.stop = true;
}

int videoSelfTest(const char* path, bool sync, bool slow, bool monitor, unsigned fps) {
    if (fopen_s(&g_testOutput, path, "wb") || !g_testOutput) return 2;
    g_testSync = sync; g_testSendDelay = slow ? 80 : 0;
    g_testMonitor = monitor;
    g_session.width = 1280; g_session.height = 720; g_session.fps = fps ? fps : 120;
    g_session.unlimitedFps = fps == 0;
    g_session.bitrateKbps = 8000; g_session.audioEnabled = false;
    g_session.active = true; g_session.stop = false; g_session.wantIdr = true;
    g_session.videoFrames = 0; g_session.videoBytes = 0;
    std::thread deadline([] {
        Sleep(1000);
        if (g_testMonitor) g_session.nextMonitor = true;
        Sleep(1000);
        if (g_testMonitor) g_session.nextMonitor = true;
        Sleep(1000); g_session.stop = true;
    });
    videoThread(); deadline.join();
    fclose(g_testOutput); g_testOutput = nullptr; g_session.active = false;
    logmsg("GPU SELFTEST: frames=%llu bytes=%llu qdrop=%u", g_session.videoFrames.load(),
           g_session.videoBytes.load(), g_session.videoQueueDrops.load());
    return g_session.videoFrames >= 5 ? 0 : 1;
}
