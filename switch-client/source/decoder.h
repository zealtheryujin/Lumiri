#pragma once
#include <vector>
#include <cstdint>

bool decoderInit(int width, int height, bool tryHardware, bool directFrames = false);
void decoderExit();

bool decoderPush(std::vector<uint8_t>&& annexb, bool isKey);
void decoderFlush();

enum DecodedPixelFormat {
    DECODED_YUV420P,
    DECODED_NV12,
};

typedef void (*FrameSink)(int w, int h, DecodedPixelFormat format,
    const uint8_t* y, int yPitch,
    const uint8_t* u, int uPitch,
    const uint8_t* v, int vPitch, void* user);
bool decoderTakeFrame(FrameSink sink, void* user);
struct AVFrame;

typedef bool (*NativeFrameSink)(const AVFrame*, void*);
bool decoderTakeNativeFrame(NativeFrameSink sink, void* user);

uint32_t decoderFrameCount();
uint32_t decoderQueueDepth();
bool decoderUsingHardware();
bool decoderAlive();
uint32_t decoderLastDecodeUs();
