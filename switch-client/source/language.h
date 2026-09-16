#pragma once
#include <atomic>

inline std::atomic<bool> g_english{true};
inline const char* tr(const char* turkish, const char* english) {
    return g_english.load(std::memory_order_relaxed) ? english : turkish;
}
