#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
#include <utility>

namespace rp {
struct EncodedVideo {
    std::vector<uint8_t> bytes;
    uint32_t id = 0;
    uint64_t capturedMs = 0;
    bool key = false;
};

class VideoQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<EncodedVideo> frames;
    bool closed = false, waitingKey = true;
public:
    bool push(EncodedVideo frame, uint64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return false;
        if (frames.size() >= 2 || (!frames.empty() && nowMs - frames.front().capturedMs > 100)) {
            frames.clear(); waitingKey = true;
        }
        if (nowMs - frame.capturedMs > 100) { frames.clear(); waitingKey = true; return false; }
        if (waitingKey && !frame.key) return false;
        waitingKey = false;
        frames.push_back(std::move(frame)); ready.notify_one();
        return true;
    }
    bool pop(EncodedVideo& frame) {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return closed || !frames.empty(); });
        if (frames.empty()) return false;
        frame = std::move(frames.front()); frames.pop_front();
        return true;
    }

    void resync() {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear(); waitingKey = true;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true; frames.clear(); ready.notify_all();
    }
};
}
