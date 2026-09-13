#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <timeapi.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "../common/protocol.h"

struct Session {
    std::atomic<bool> nextMonitor{false};
    std::atomic<bool> active{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> wantIdr{false};
    std::atomic<uint32_t> idrReqs{0};
    sockaddr_in mediaAddr{};

    uint16_t width = 1280, height = 720, fps = 60;
    uint32_t bitrateKbps = 8000;
    bool audioEnabled = true;

    std::atomic<uint64_t> videoBytes{0}, videoFrames{0}, inputPkts{0}, sendErrors{0};
    std::atomic<uint32_t> captureUs{0}, encodeSendUs{0};
    std::atomic<uint32_t> sendUs{0}, videoQueueDrops{0}, captureSkips{0};
};

extern Session g_session;
extern SOCKET g_mediaSock;

void logmsg(const char* fmt, ...);

void sendMedia(uint8_t channel, uint8_t flags, uint32_t frameId,
               const uint8_t* data, size_t len);

void videoThread();
void audioThread();
void inputThread();
