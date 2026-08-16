#pragma once
#include <vector>
#include <cstddef>

namespace itch {

template <typename T>
class MemoryPool {
private:
    std::vector<T> storage_;
    T* free_head_{nullptr};

public:
    explicit MemoryPool(size_t capacity) : storage_(capacity) {
        for (size_t i = 0; i < capacity; ++i) {
            storage_[i].next_free = free_head_;
            free_head_ = &storage_[i];
        }
    }

    // Disable copy/move to guarantee stable memory addresses
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    [[nodiscard]] inline T* allocate() noexcept {
        if (!free_head_) [[unlikely]] return nullptr;
        T* node = free_head_;
        free_head_ = free_head_->next_free;
        
        // Clear the union pointer so it doesn't corrupt the linked lists
        node->next_free = nullptr; 
        return node;
    }

    inline void deallocate(T* node) noexcept {
        node->next_free = free_head_;
        free_head_ = node;
    }
};

} // namespace itch