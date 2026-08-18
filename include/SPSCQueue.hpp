#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace itch {

#if defined(__cpp_lib_hardware_interference_size)
    using std::hardware_destructive_interference_size;
#else
    constexpr size_t hardware_destructive_interference_size = 64;
#endif

template <typename T, size_t Capacity = 65536>
class alignas(hardware_destructive_interference_size) SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    SPSCQueue() : head_(0), tail_(0), cached_tail_(0), cached_head_(0) {}

    ~SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    [[nodiscard]] bool push(const T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        if ((current_head - cached_tail_) == Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if ((current_head - cached_tail_) == Capacity) {
                return false; // Queue full
            }
        }

        buffer_[current_head & MASK] = item;
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        if (current_tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail == cached_head_) {
                return false; // Queue empty
            }
        }

        item = buffer_[current_tail & MASK];
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t MASK = Capacity - 1;

    alignas(hardware_destructive_interference_size) std::atomic<size_t> head_;
    size_t cached_tail_;

    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail_;
    size_t cached_head_;

    alignas(hardware_destructive_interference_size) T buffer_[Capacity];
};

} // namespace itch