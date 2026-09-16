#include "../common/video_queue.h"
#include <cassert>
#include <future>
#include <cstdio>
#include <thread>
static rp::EncodedVideo frame(uint32_t id, bool key, uint64_t timestamp = 1000) {
    rp::EncodedVideo f; f.id = id; f.key = key; f.capturedMs = timestamp;
    f.bytes.assign(100, uint8_t(id)); return f;
}
int main() {
    rp::VideoQueue q;
    assert(!q.push(frame(0, false), 1000));
    assert(q.push(frame(1, true), 1000));
    assert(q.push(frame(2, false), 1000));
    assert(!q.push(frame(3, false), 1000));
    assert(!q.push(frame(4, false), 1000));
    assert(q.push(frame(5, true), 1000));
    rp::EncodedVideo out;
    assert(q.pop(out) && out.id == 5 && out.bytes[0] == 5);
    assert(q.push(frame(6, false), 1000));
    assert(!q.push(frame(7, false, 1101), 1101));
    assert(!q.push(frame(8, true, 1000), 1101));
    assert(q.push(frame(9, true, 1101), 1101));
    q.resync();
    assert(!q.push(frame(10, false, 1101), 1101));
    assert(q.push(frame(11, true, 1101), 1101));
    assert(q.push(frame(12, false, 1101), 1101));
    assert(q.push(frame(13, true, 1101), 1101));
    assert(q.pop(out) && out.id == 13);
    auto waiter = std::async(std::launch::async, [&] { return q.pop(out); });
    q.close();
    assert(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(!waiter.get());
    assert(!q.push(frame(14, true), 1000));

    rp::VideoQueue concurrent;
    std::promise<void> firstConsumed;
    size_t consumed = 0;
    std::thread consumer([&] {
        rp::EncodedVideo item;
        uint32_t previous = 0;
        while (concurrent.pop(item)) {
            assert(item.key || item.id == previous + 1);
            assert(item.bytes.size() == 100 && item.bytes[0] == uint8_t(item.id));
            previous = item.id;
            if (++consumed == 1) firstConsumed.set_value();
            if (consumed % 17 == 0) std::this_thread::yield();
        }
    });
    assert(concurrent.push(frame(1, true), 1000));
    firstConsumed.get_future().wait();
    bool key = false;
    for (uint32_t i = 2; i < 10002; ++i)
        key = !concurrent.push(frame(i, key), 1000);
    concurrent.close(); consumer.join();
    assert(consumed > 0);
    puts("PASS: video queue bounded overflow, reference recovery, expiry, payload ownership, close wakeup");
    puts("PASS: 10000 concurrent submissions retain decodable frame ordering");
}
