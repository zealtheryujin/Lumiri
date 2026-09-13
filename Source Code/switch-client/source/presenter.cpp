#include "presenter.h"
#if RP_EXPERIMENTAL_DIRECT
#include "../../common/video_math.h"
#include <switch.h>
#include <deko3d.h>
#include <SDL2/SDL.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_nvtegra.h>
}
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <cstdio>
#include "rp_vsh_bin.h"
#include "rp_video_fsh_bin.h"
#include "rp_ui_fsh_bin.h"

namespace {
constexpr unsigned Slots = 2, CmdBytes = 64 * 1024, UiW = 1280, UiH = 720;
constexpr unsigned DescBytes = 4096, UniformOffset = 1024, SamplerOffset = 512;
uint32_t alignUp(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }
DkDevice device = nullptr;
DkQueue queue = nullptr;
DkCmdBuf commands = nullptr;
DkSwapchain swapchain = nullptr;
DkMemBlock frameMemory = nullptr, codeMemory = nullptr, commandMemory = nullptr;
DkImage frameImages[Slots];
DkShader vertex, videoShader, uiShader;
unsigned fbW = 0, fbH = 0;
uint64_t serial = 0;
uint32_t uploadUs = 0;
uint32_t videoCopyBytes = 0, presentedFrames = 0;
uint64_t presentedSerial = 0;
FILE* diagnostic = nullptr;
int pendingSlot = -1;
AVFrame* current = nullptr;
void note(const char* message) {
    if (diagnostic) { std::fprintf(diagnostic, "%llu %s\n", (unsigned long long)SDL_GetTicks64(), message); std::fflush(diagnostic); }
}
struct Import {
    AVBufferRef* mapRef = nullptr;
    AVBufferRef* contextRef = nullptr;
    DkMemBlock memory = nullptr;
    DkImage planes[3];
    uint32_t handle = 0;
    int width = 0, height = 0, stride[3] = {};
    float scales[3][4] = {};
    ~Import() {
        if (memory) dkMemBlockDestroy(memory);
        av_buffer_unref(&mapRef);
        av_buffer_unref(&contextRef);
    }
};
std::vector<std::unique_ptr<Import>> imports;
Import* activeImport = nullptr;
struct Slot {
    DkFence fence{};
    bool submitted = false;
    AVFrame* held = nullptr;
    DkMemBlock descriptors = nullptr, uiMemory = nullptr, videoMemory = nullptr;
    DkImage uiImage{}, videoImages[3]{};
    uint32_t videoBytes = 0;
    uint64_t uploadedSerial = 0;
    float scales[3][4] = {};
} slots[Slots];
struct Params { float r[4], g[4], b[4], scale[3][4]; };

DkMemBlock allocate(uint32_t size, uint32_t flags, void* storage = nullptr) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, alignUp(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags; maker.storage = storage;
    return dkMemBlockCreate(&maker);
}
constexpr uint32_t HostMemory = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
void descriptor(Slot& s, unsigned index, const DkImage& image) {
    DkImageView view; dkImageViewDefaults(&view, &image);
    auto* descriptors = static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(s.descriptors));
    dkImageDescriptorInitialize(&descriptors[index], &view, false, false);
}
void imageLayout(DkImageLayout& layout, DkImageFormat format, unsigned w, unsigned h,
                 bool linear, unsigned pitch) {
    DkImageLayoutMaker maker; dkImageLayoutMakerDefaults(&maker, device);
    maker.type = DkImageType_2D; maker.format = format;
    maker.dimensions[0] = w; maker.dimensions[1] = h; maker.dimensions[2] = 1;
    maker.flags = linear ? DkImageFlags_PitchLinear : DkImageFlags_UsageVideo;
    if (linear) maker.pitchStride = pitch;
    dkImageLayoutInitialize(&layout, &maker);
}
void setScale(float (&v)[4], unsigned visibleW, unsigned visibleH, unsigned texW, unsigned texH) {
    rp::textureScale(v, visibleW, visibleH, texW, texH);
}
void waitIdle() { if (queue) dkQueueWaitIdle(queue); }

bool rebuildSwapchain() {
    unsigned w = appletGetOperationMode() == AppletOperationMode_Console ? 1920 : 1280;
    unsigned h = w == 1920 ? 1080 : 720;
    if (swapchain && fbW == w && fbH == h) return true;
    waitIdle();
    if (swapchain) { dkSwapchainDestroy(swapchain); swapchain = nullptr; }
    if (frameMemory) { dkMemBlockDestroy(frameMemory); frameMemory = nullptr; }
    DkImageLayoutMaker maker; dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent;
    maker.format = DkImageFormat_RGBA8_Unorm;
    maker.dimensions[0] = w; maker.dimensions[1] = h;
    DkImageLayout layout; dkImageLayoutInitialize(&layout, &maker);
    uint32_t bytes = alignUp(dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));
    frameMemory = allocate(bytes * Slots, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!frameMemory) return false;
    const DkImage* images[Slots];
    for (unsigned i = 0; i < Slots; ++i) {
        dkImageInitialize(&frameImages[i], &layout, frameMemory, bytes * i);
        images[i] = &frameImages[i];
    }
    DkSwapchainMaker sm; dkSwapchainMakerDefaults(&sm, device, nwindowGetDefault(), images, Slots);
    swapchain = dkSwapchainCreate(&sm);
    if (!swapchain) return false;
    dkSwapchainSetSwapInterval(swapchain, 1);
    fbW = w; fbH = h;
    note(w == 1920 ? "swapchain 1920x1080 (2 buffers, vsync)" : "swapchain 1280x720 (2 buffers, vsync)");
    return true;
}

Import* importFrame(const AVFrame* f) {
    if (!f->buf[0] || !f->hw_frames_ctx) return nullptr;
    auto* hw = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    if (hw->sw_format != AV_PIX_FMT_NV12) return nullptr;
    auto* nf = reinterpret_cast<AVNVTegraFrame*>(f->buf[0]->data);
    if (!nf->map_ref) return nullptr;
    AVNVTegraMap* map = av_nvtegra_frame_get_fbuf_map(f);
    uint32_t handle = av_nvtegra_map_get_handle(map);
    for (auto& entry : imports) {
        if (entry->handle == handle && entry->contextRef->data == f->hw_frames_ctx->data &&
            entry->width == f->width && entry->height == f->height &&
            entry->stride[0] == f->linesize[0] && entry->stride[1] == f->linesize[1])
            return entry.get();
    }

    if (imports.size() >= 32 || (!imports.empty() && imports[0]->contextRef->data != f->hw_frames_ctx->data)) {
        waitIdle(); imports.clear(); activeImport = nullptr; av_frame_free(&current);
    }
    auto entry = std::make_unique<Import>();
    entry->mapRef = av_buffer_ref(nf->map_ref);
    entry->contextRef = av_buffer_ref(f->hw_frames_ctx);
    if (!entry->mapRef || !entry->contextRef) return nullptr;
    uint32_t bytes = av_nvtegra_map_get_size(map);
    void* base = av_nvtegra_map_get_addr(map);
    if (!base || (uintptr_t(base) & (DK_MEMBLOCK_ALIGNMENT - 1)) ||
        !bytes || (bytes & (DK_MEMBLOCK_ALIGNMENT - 1))) return nullptr;
    entry->memory = allocate(bytes, HostMemory | DkMemBlockFlags_Image, base);
    if (!entry->memory) return nullptr;
    entry->handle = handle; entry->width = f->width; entry->height = f->height;
    for (unsigned p = 0; p < 2; ++p) {
        unsigned bpp = p ? 2 : 1, w = p ? (f->width + 1) / 2 : f->width;
        unsigned h = p ? (f->height + 1) / 2 : f->height;
        if (f->linesize[p] <= 0 || unsigned(f->linesize[p]) < w * bpp || !f->data[p]) return nullptr;
        unsigned texW = f->linesize[p] / bpp;
        DkImageLayout layout;
        imageLayout(layout, p ? DkImageFormat_RG8_Unorm : DkImageFormat_R8_Unorm,
                    texW, h, map->is_linear, f->linesize[p]);
        uintptr_t offset = uintptr_t(f->data[p]) - uintptr_t(base);
        if (!rp::planeRangeValid(offset, dkImageLayoutGetSize(&layout), bytes,
                                dkImageLayoutGetAlignment(&layout))) return nullptr;
        dkImageInitialize(&entry->planes[p], &layout, entry->memory, uint32_t(offset));
        entry->stride[p] = f->linesize[p];
        setScale(entry->scales[p], w, h, texW, h);
    }
    Import* result = entry.get(); imports.push_back(std::move(entry));
    return result;
}

bool uploadSoftware(Slot& s) {
    if (s.uploadedSerial == serial) return true;
    bool nv12 = current->format == AV_PIX_FMT_NV12;
    unsigned count = nv12 ? 2 : 3;
    DkImageLayout layouts[3];
    unsigned offsets[3], pitches[3], widths[3], heights[3], bytes = 0;
    for (unsigned p = 0; p < count; ++p) {
        widths[p] = p ? (current->width + 1) / 2 : current->width;
        heights[p] = p ? (current->height + 1) / 2 : current->height;
        unsigned bpp = p && nv12 ? 2 : 1;
        pitches[p] = alignUp(widths[p] * bpp, 32);
        imageLayout(layouts[p], bpp == 2 ? DkImageFormat_RG8_Unorm : DkImageFormat_R8_Unorm,
                    widths[p], heights[p], true, pitches[p]);
        offsets[p] = alignUp(bytes, dkImageLayoutGetAlignment(&layouts[p]));
        bytes = offsets[p] + dkImageLayoutGetSize(&layouts[p]);
    }
    if (s.videoBytes < bytes) {
        if (s.videoMemory) dkMemBlockDestroy(s.videoMemory);
        s.videoMemory = allocate(bytes, HostMemory);
        s.videoBytes = s.videoMemory ? alignUp(bytes, DK_MEMBLOCK_ALIGNMENT) : 0;
    }
    if (!s.videoMemory) return false;
    auto* base = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(s.videoMemory));
    std::memset(s.scales, 0, sizeof(s.scales));
    for (unsigned p = 0; p < count; ++p) {
        unsigned rowBytes = widths[p] * (p && nv12 ? 2 : 1);
        if (!current->data[p] || std::abs(current->linesize[p]) < int(rowBytes)) return false;
        for (unsigned y = 0; y < heights[p]; ++y)
            std::memcpy(base + offsets[p] + y * pitches[p],
                        current->data[p] + ptrdiff_t(y) * current->linesize[p], rowBytes);
        videoCopyBytes += rowBytes * heights[p];
        dkImageInitialize(&s.videoImages[p], &layouts[p], s.videoMemory, offsets[p]);
        setScale(s.scales[p], widths[p], heights[p], widths[p], heights[p]);
    }
    s.uploadedSerial = serial;
    return true;
}

Params parameters(const float (&scales)[3][4]) {
    Params p{};
    bool full = current->color_range == AVCOL_RANGE_JPEG || current->format == AV_PIX_FMT_YUVJ420P;
    bool bt709 = current->colorspace == AVCOL_SPC_BT709 ||
                 (current->colorspace == AVCOL_SPC_UNSPECIFIED && current->height >= 720);
    rp::YuvMatrix matrix = rp::yuvMatrix(full, bt709);
    std::memcpy(p.r, matrix.r, sizeof(p.r));
    std::memcpy(p.g, matrix.g, sizeof(p.g));
    std::memcpy(p.b, matrix.b, sizeof(p.b));
    std::memcpy(p.scale, scales, sizeof(p.scale));
    return p;
}
}

bool presenterInit() {
    diagnostic = std::fopen("/switch/lumiri-render.log", "w");
    note("Deko3D initialization");
    DkDeviceMaker dm; dkDeviceMakerDefaults(&dm); device = dkDeviceCreate(&dm);
    if (!device) { note("device creation failed; using legacy SDL"); presenterExit(); return false; }
    DkQueueMaker qm; dkQueueMakerDefaults(&qm, device); qm.flags = DkQueueFlags_Graphics;
    queue = dkQueueCreate(&qm);
    if (!queue || !rebuildSwapchain()) { note("queue/swapchain failed; using legacy SDL"); presenterExit(); return false; }
    commandMemory = allocate(CmdBytes * Slots, HostMemory);
    codeMemory = allocate(64 * 1024, HostMemory | DkMemBlockFlags_Code);
    DkCmdBufMaker cm; dkCmdBufMakerDefaults(&cm, device); commands = dkCmdBufCreate(&cm);
    if (!commandMemory || !codeMemory || !commands) { presenterExit(); return false; }
    unsigned codeOffset = 0;
    auto shader = [&](DkShader& dst, const void* data, unsigned size) {
        std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(codeMemory)) + codeOffset, data, size);
        DkShaderMaker sm; dkShaderMakerDefaults(&sm, codeMemory, codeOffset); dkShaderInitialize(&dst, &sm);
        codeOffset += alignUp(size, DK_SHADER_CODE_ALIGNMENT);
    };
    shader(vertex, rp_vsh_bin, rp_vsh_bin_size);
    shader(videoShader, rp_video_fsh_bin, rp_video_fsh_bin_size);
    shader(uiShader, rp_ui_fsh_bin, rp_ui_fsh_bin_size);
    for (auto& s : slots) {
        s.descriptors = allocate(DescBytes, HostMemory);
        s.uiMemory = allocate(UiW * UiH * 4, HostMemory);
        if (!s.descriptors || !s.uiMemory) { presenterExit(); return false; }
        DkImageLayout layout; imageLayout(layout, DkImageFormat_RGBA8_Unorm, UiW, UiH, true, UiW * 4);
        dkImageInitialize(&s.uiImage, &layout, s.uiMemory, 0);
        DkSampler sampler; dkSamplerDefaults(&sampler);
        sampler.minFilter = sampler.magFilter = DkFilter_Linear;
        for (auto& wrap : sampler.wrapMode) wrap = DkWrapMode_ClampToEdge;
        auto* dest = reinterpret_cast<DkSamplerDescriptor*>(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(s.descriptors)) + SamplerOffset);
        dkSamplerDescriptorInitialize(dest, &sampler);
    }
    note("Deko3D ready");
    return true;
}

bool presenterSubmit(const AVFrame* frame, void*) {
    if (!device || !frame || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 4096 || frame->height > 2160) return false;
    Import* imported = nullptr;
    if (frame->format == AV_PIX_FMT_NVTEGRA) {
        imported = importFrame(frame);
        if (!imported) { note("NVDEC import rejected; transferring latest frame via VIC/CPU fallback"); return false; }
    } else if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P &&
               frame->format != AV_PIX_FMT_YUVJ420P) return false;
    bool hadFrame = current && current->data[0];
    if (!current) current = av_frame_alloc();
    if (!current) return false;
    av_frame_unref(current);
    if (av_frame_ref(current, frame) < 0) { av_frame_free(&current); activeImport = nullptr; return false; }
    if (!hadFrame || bool(activeImport) != bool(imported))
        note(imported ? "video path: NVDEC direct (no VIC transfer, no CPU video copy)" : "video path: planar GPU upload");
    activeImport = imported; ++serial;
    return true;
}

bool presenterBegin() {
    if (pendingSlot >= 0) return true;
    if (!queue || !rebuildSwapchain()) return false;
    int index = dkQueueAcquireImage(queue, swapchain);
    if (index < 0 || index >= int(Slots)) return false;
    Slot& s = slots[index];
    if (s.submitted && dkFenceWait(&s.fence, -1) != DkResult_Success) return false;
    if (s.held) av_frame_unref(s.held);
    pendingSlot = index;
    return true;
}

bool presenterPresent(SDL_Surface* ui, bool showUi, bool showVideo, const SDL_Rect* uiRegion) {
    if (!presenterBegin()) return false;
    int index = pendingSlot;
    Slot& s = slots[index];
    SDL_Rect region = uiRegion ? *uiRegion : SDL_Rect{0, 0, int(UiW), int(UiH)};
    if (region.x < 0 || region.y < 0 || region.w <= 0 || region.h <= 0 ||
        region.x + region.w > int(UiW) || region.y + region.h > int(UiH)) return false;
    uint64_t started = armGetSystemTick();
    videoCopyBytes = 0;
    bool drawVideo = showVideo && current;
    if (drawVideo) {
        if (!s.held) s.held = av_frame_alloc();
        if (!s.held || av_frame_ref(s.held, current) < 0) return false;
        if (activeImport) {
            descriptor(s, 0, activeImport->planes[0]); descriptor(s, 1, activeImport->planes[1]);
            descriptor(s, 2, activeImport->planes[1]);
        } else {
            if (!uploadSoftware(s)) return false;
            descriptor(s, 0, s.videoImages[0]); descriptor(s, 1, s.videoImages[1]);
            descriptor(s, 2, s.videoImages[current->format == AV_PIX_FMT_NV12 ? 1 : 2]);
        }
    }
    if (showUi && ui) {
        if (ui->w != int(UiW) || ui->h != int(UiH) || ui->format->format != SDL_PIXELFORMAT_ABGR8888) return false;
        auto* dest = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(s.uiMemory));
        unsigned pitch = alignUp(region.w * 4, 32);
        for (int y = 0; y < region.h; ++y)
            std::memcpy(dest + y * pitch, static_cast<uint8_t*>(ui->pixels) +
                        (region.y + y) * ui->pitch + region.x * 4, region.w * 4);
        DkImageLayout layout; imageLayout(layout, DkImageFormat_RGBA8_Unorm,
                                         region.w, region.h, true, pitch);
        dkImageInitialize(&s.uiImage, &layout, s.uiMemory, 0);
        descriptor(s, 3, s.uiImage);
    }
    uploadUs = armTicksToNs(armGetSystemTick() - started) / 1000;
    dkCmdBufClear(commands); dkCmdBufAddMemory(commands, commandMemory, index * CmdBytes, CmdBytes);
    dkCmdBufBarrier(commands, DkBarrier_None, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    DkImageView target; dkImageViewDefaults(&target, &frameImages[index]);
    dkCmdBufBindRenderTarget(commands, &target, nullptr);
    DkScissor scissor{0, 0, fbW, fbH}; dkCmdBufSetScissors(commands, 0, &scissor, 1);
    dkCmdBufClearColorFloat(commands, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    DkRasterizerState raster; dkRasterizerStateDefaults(&raster); raster.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(commands, &raster);
    DkColorWriteState writes; dkColorWriteStateDefaults(&writes); dkCmdBufBindColorWriteState(commands, &writes);
    DkColorState color; dkColorStateDefaults(&color); dkCmdBufBindColorState(commands, &color);
    DkGpuAddr desc = dkMemBlockGetGpuAddr(s.descriptors);
    dkCmdBufBindImageDescriptorSet(commands, desc, 4);
    dkCmdBufBindSamplerDescriptorSet(commands, desc + SamplerOffset, 1);
    if (drawVideo) {
        float scale = std::min(float(fbW) / current->width, float(fbH) / current->height);
        float w = current->width * scale, h = current->height * scale;
        DkViewport vp{(fbW - w) / 2, (fbH - h) / 2, w, h, 0, 1};
        dkCmdBufSetViewports(commands, 0, &vp, 1);
        const DkShader* shaders[] = {&vertex, &videoShader};
        dkCmdBufBindShaders(commands, DkStageFlag_GraphicsMask, shaders, 2);
        Params params = parameters(activeImport ? activeImport->scales : s.scales);
        dkCmdBufPushConstants(commands, desc + UniformOffset, sizeof(params), 0, sizeof(params), &params);
        dkCmdBufBindUniformBuffer(commands, DkStage_Fragment, 0, desc + UniformOffset, sizeof(params));
        DkResHandle textures[] = {dkMakeTextureHandle(0, 0), dkMakeTextureHandle(1, 0), dkMakeTextureHandle(2, 0)};
        dkCmdBufBindTextures(commands, DkStage_Fragment, 0, textures, 3);
        dkCmdBufDraw(commands, DkPrimitive_Triangles, 3, 1, 0, 0);
    }
    if (showUi && ui) {
        float sx = float(fbW) / UiW, sy = float(fbH) / UiH;
        DkViewport vp{region.x * sx, region.y * sy, region.w * sx, region.h * sy, 0, 1};
        dkCmdBufSetViewports(commands, 0, &vp, 1);
        const DkShader* shaders[] = {&vertex, &uiShader}; dkCmdBufBindShaders(commands, DkStageFlag_GraphicsMask, shaders, 2);
        dkColorStateSetBlendEnable(&color, 0, true); dkCmdBufBindColorState(commands, &color);
        DkBlendState blend; dkBlendStateDefaults(&blend);
        dkBlendStateSetFactors(&blend, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha, DkBlendFactor_One, DkBlendFactor_InvSrcAlpha);
        dkCmdBufBindBlendStates(commands, 0, &blend, 1);
        dkCmdBufBindTexture(commands, DkStage_Fragment, 0, dkMakeTextureHandle(3, 0));
        dkCmdBufDraw(commands, DkPrimitive_Triangles, 3, 1, 0, 0);
    }
    dkCmdBufSignalFence(commands, &s.fence, true);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(commands)); s.submitted = true;
    dkQueuePresentImage(queue, swapchain, index);
    if (drawVideo && presentedSerial != serial) { presentedSerial = serial; ++presentedFrames; }
    pendingSlot = -1;
    return true;
}

void presenterClearVideo() {
    waitIdle();
    for (auto& s : slots) { av_frame_free(&s.held); s.uploadedSerial = 0; }
    av_frame_free(&current); activeImport = nullptr; imports.clear(); uploadUs = 0;
    presentedFrames = 0; presentedSerial = 0; videoCopyBytes = 0;
}
void presenterExit() {
    presenterClearVideo();
    if (queue) dkQueueDestroy(queue);
    queue = nullptr;
    if (commands) dkCmdBufDestroy(commands);
    commands = nullptr;
    if (swapchain) dkSwapchainDestroy(swapchain);
    swapchain = nullptr;
    for (auto& s : slots) {
        if (s.descriptors) dkMemBlockDestroy(s.descriptors);
        if (s.uiMemory) dkMemBlockDestroy(s.uiMemory);
        if (s.videoMemory) dkMemBlockDestroy(s.videoMemory);
        s = {};
    }
    if (frameMemory) dkMemBlockDestroy(frameMemory);
    frameMemory = nullptr;
    if (commandMemory) dkMemBlockDestroy(commandMemory);
    commandMemory = nullptr;
    if (codeMemory) dkMemBlockDestroy(codeMemory);
    codeMemory = nullptr;
    if (device) dkDeviceDestroy(device);
    device = nullptr;
    pendingSlot = -1;
    note("Deko3D stopped");
    if (diagnostic) std::fclose(diagnostic);
    diagnostic = nullptr;
    fbW = fbH = 0;
}
bool presenterHasFrame() { return current; }
const char* presenterPath() { return activeImport ? "NVDEC direct" : "GPU upload"; }
uint32_t presenterUploadUs() { return uploadUs; }
uint32_t presenterVideoCopyBytes() { return videoCopyBytes; }
uint32_t presenterFrameCount() { return presentedFrames; }
#else
bool presenterInit() { return false; }
void presenterExit() {}
bool presenterSubmit(const AVFrame*, void*) { return false; }
bool presenterHasFrame() { return false; }
void presenterClearVideo() {}
bool presenterBegin() { return false; }
bool presenterPresent(SDL_Surface*, bool, bool, const SDL_Rect*) { return false; }
const char* presenterPath() { return "SDL compatibility"; }
uint32_t presenterUploadUs() { return 0; }
uint32_t presenterVideoCopyBytes() { return 0; }
uint32_t presenterFrameCount() { return 0; }
#endif
