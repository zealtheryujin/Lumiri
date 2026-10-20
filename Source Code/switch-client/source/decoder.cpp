#include "decoder.h"
#include "net.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

static AVCodecContext* g_ctx = nullptr;
static AVBufferRef* g_hwDevice = nullptr;
static AVPixelFormat g_hwPixFmt = AV_PIX_FMT_NONE;
static std::atomic<bool> g_hwActive{false};
static std::thread g_thread;
static std::atomic<bool> g_run{false};
static std::atomic<uint32_t> g_frames{0};
static std::atomic<uint32_t> g_lastDecodeUs{0};

static std::mutex g_qMtx;
static std::condition_variable g_qCv;
static std::deque<std::vector<uint8_t>> g_queue;

static std::mutex g_fMtx;
static AVFrame* g_latest = nullptr;

static AVFrame* g_display = nullptr;
static AVFrame* g_transfer = nullptr;
static bool g_directFrames = false;
static bool g_importFailed = false;
static bool g_fresh = false;
static std::atomic<bool> g_wantFlush{false};
static bool openDecoder(const AVCodec* codec, int width, int height, bool hardware);

static AVPixelFormat chooseHwFormat(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == g_hwPixFmt) return *f;
    return AV_PIX_FMT_NONE;
}

static void decodeLoop() {

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    AVFrame* sw = av_frame_alloc();
    if (!pkt || !frm || !sw) {
        av_packet_free(&pkt); av_frame_free(&frm); av_frame_free(&sw);
        g_run = false;
        sessionResync();
        return;
    }
    uint64_t lastResync = 0;
    auto recover = [&](bool hardwareFailure) {

        if (hardwareFailure && g_hwActive) {
            int width = g_ctx->width, height = g_ctx->height;
            av_frame_unref(frm); av_frame_unref(sw);
            avcodec_free_context(&g_ctx);
            av_buffer_unref(&g_hwDevice);
            g_hwActive = false;
            g_hwPixFmt = AV_PIX_FMT_NONE;
            if (!openDecoder(avcodec_find_decoder(AV_CODEC_ID_H264), width, height, false))
                g_run = false;
        }
        uint64_t tick = nowMs();
        if (!lastResync || tick - lastResync >= 250) {
            lastResync = tick;
            sessionResync();
        }
    };
    while (g_run) {
        std::vector<uint8_t> data;
        bool flush = false;
        {
            std::unique_lock<std::mutex> lk(g_qMtx);
            g_qCv.wait(lk, [] { return !g_queue.empty() || !g_run || g_wantFlush.load(); });
            if (!g_run) break;
            flush = g_wantFlush.exchange(false);
            if (!g_queue.empty()) {
                data = std::move(g_queue.front());
                g_queue.pop_front();
            }
        }
        if (flush) avcodec_flush_buffers(g_ctx);
        if (data.empty()) continue;

        av_packet_unref(pkt);
        if (av_new_packet(pkt, (int)data.size()) < 0) { recover(false); continue; }
        memcpy(pkt->data, data.data(), data.size());

        auto started = std::chrono::steady_clock::now();
        int sendResult = avcodec_send_packet(g_ctx, pkt);
        if (sendResult < 0) {
            recover(sendResult != AVERROR_INVALIDDATA && sendResult != AVERROR(EAGAIN));
            continue;
        }
        int receiveResult;
        while ((receiveResult = avcodec_receive_frame(g_ctx, frm)) == 0) {
            AVFrame* ready = frm;
            if (!g_directFrames && g_hwActive && frm->format == g_hwPixFmt) {

                av_frame_unref(sw);
                if (av_hwframe_transfer_data(sw, frm, 0) < 0) { recover(true); break; }
                ready = sw;
            }
            {
                std::lock_guard<std::mutex> lk(g_fMtx);
                if (!g_latest) g_latest = av_frame_alloc();
                if (!g_latest) { g_run = false; av_frame_unref(frm); break; }
                av_frame_unref(g_latest);
                av_frame_move_ref(g_latest, ready);
                g_fresh = true;
                g_frames++;
            }
            av_frame_unref(frm);
        }
        if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF)
            recover(receiveResult != AVERROR_INVALIDDATA);
        g_lastDecodeUs = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
    }
    av_packet_free(&pkt);
    av_frame_free(&frm);
    av_frame_free(&sw);
}

static bool openDecoder(const AVCodec* codec, int width, int height, bool hardware) {
    g_ctx = avcodec_alloc_context3(codec);
    if (!g_ctx) return false;
    g_ctx->width = width;
    g_ctx->height = height;
    g_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (hardware) {
        g_hwPixFmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* c = avcodec_get_hw_config(codec, i);
            if (!c) break;
            if (c->device_type == AV_HWDEVICE_TYPE_NVTEGRA &&
                (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                g_hwPixFmt = c->pix_fmt;
                break;
            }
        }
        if (g_hwPixFmt == AV_PIX_FMT_NONE ||
            av_hwdevice_ctx_create(&g_hwDevice, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) < 0) {
            avcodec_free_context(&g_ctx);
            return false;
        }
        g_ctx->get_format = chooseHwFormat;
        g_ctx->hw_device_ctx = av_buffer_ref(g_hwDevice);
        g_ctx->thread_count = 1;
    } else {
        g_ctx->thread_count = 4;
        g_ctx->thread_type = FF_THREAD_SLICE;
    }

    if (avcodec_open2(g_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&g_ctx);
        av_buffer_unref(&g_hwDevice);
        g_hwPixFmt = AV_PIX_FMT_NONE;
        return false;
    }
    g_hwActive = hardware;
    return true;
}

bool decoderInit(int width, int height, bool tryHardware, bool directFrames) {
    g_directFrames = directFrames;
    g_importFailed = false;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    g_hwActive = false;
    g_hwPixFmt = AV_PIX_FMT_NONE;

    if (!(tryHardware && openDecoder(codec, width, height, true)) &&
        !openDecoder(codec, width, height, false))
        return false;
    g_run = true;
    g_wantFlush = false;
    g_frames = 0;
    g_lastDecodeUs = 0;
    g_thread = std::thread(decodeLoop);
    return true;
}

void decoderExit() {
    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        g_run = false;
    }
    g_qCv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_fMtx);
        if (g_latest) av_frame_free(&g_latest);
        if (g_display) av_frame_free(&g_display);
        if (g_transfer) av_frame_free(&g_transfer);
        g_fresh = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        g_queue.clear();
    }
    if (g_ctx) avcodec_free_context(&g_ctx);
    av_buffer_unref(&g_hwDevice);
    g_hwPixFmt = AV_PIX_FMT_NONE;
    g_hwActive = false;
}

bool decoderPush(std::vector<uint8_t>&& annexb, bool isKey) {
    std::lock_guard<std::mutex> lk(g_qMtx);
    if (!g_run) return false;

    if (g_queue.size() >= 2) {

        if (!isKey) return false;
        g_queue.clear();
        g_wantFlush = true;
    }
    g_queue.push_back(std::move(annexb));
    g_qCv.notify_one();
    return true;
}

bool decoderTakeFrame(FrameSink sink, void* user) {
    {
        std::lock_guard<std::mutex> lk(g_fMtx);
        if (!g_fresh || !g_latest || !g_latest->data[0]) return false;
        if (!g_display) g_display = av_frame_alloc();
        if (!g_display) return false;
        av_frame_unref(g_display);
        av_frame_move_ref(g_display, g_latest);
        g_fresh = false;
    }
    DecodedPixelFormat fmt = g_display->format == AV_PIX_FMT_NV12 ? DECODED_NV12 : DECODED_YUV420P;
    sink(g_display->width, g_display->height, fmt,
         g_display->data[0], g_display->linesize[0],
         g_display->data[1], g_display->linesize[1],
         g_display->data[2], g_display->linesize[2], user);
    return true;
}

bool decoderTakeNativeFrame(NativeFrameSink sink, void* user) {
    {
        std::lock_guard<std::mutex> lk(g_fMtx);
        if (!g_fresh || !g_latest || !g_latest->data[0]) return false;
        if (!g_display) g_display = av_frame_alloc();
        if (!g_display) return false;
        av_frame_unref(g_display);
        av_frame_move_ref(g_display, g_latest);
        g_fresh = false;
    }
    if (g_display->format != AV_PIX_FMT_NVTEGRA) return sink(g_display, user);
    if (!g_importFailed && sink(g_display, user)) return true;
    g_importFailed = true;

    if (!g_transfer) g_transfer = av_frame_alloc();
    if (!g_transfer) return false;
    av_frame_unref(g_transfer);
    if (av_hwframe_transfer_data(g_transfer, g_display, 0) < 0) { sessionResync(); return false; }
    av_frame_copy_props(g_transfer, g_display);
    return sink(g_transfer, user);
}

uint32_t decoderFrameCount() { return g_frames; }

uint32_t decoderQueueDepth() {
    std::lock_guard<std::mutex> lk(g_qMtx);
    return (uint32_t)g_queue.size();
}

void decoderFlush() {
    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        g_queue.clear();
        g_wantFlush = true;
    }
    g_qCv.notify_one();
}

bool decoderUsingHardware() { return g_hwActive; }
bool decoderAlive() { return g_run; }
uint32_t decoderLastDecodeUs() { return g_lastDecodeUs; }
