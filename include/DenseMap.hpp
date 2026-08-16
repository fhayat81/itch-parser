#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory_resource>
#include <utility>
#include <vector>

namespace itch {

template <typename KeyType, typename ValueType, typename Hash = std::hash<KeyType>>
class DenseMap {
public:
    enum Status : int8_t { EMPTY = 0, OCCUPIED = 1, DELETED = 2 };

    // Status is now packed directly alongside the key and value
    struct Entry {
        KeyType key{};
        ValueType value{};
        int8_t status{EMPTY}; 
    };

    using EntryVector = std::vector<Entry, std::pmr::polymorphic_allocator<Entry>>;

    explicit DenseMap(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : entries_(mr) {
        reserve(32);
    }

    void reserve(size_t capacity) {
        if (capacity <= capacity_) return;

        size_t new_cap = 1;
        while (new_cap < capacity) new_cap <<= 1;

        // Elements are automatically initialized with status = EMPTY
        EntryVector new_entries(new_cap, entries_.get_allocator());

        size_t new_mask = new_cap - 1;
        size_t active_count = 0;

        for (size_t i = 0; i < capacity_; ++i) {
            if (entries_[i].status == OCCUPIED) {
                size_t hash_val = Hash{}(entries_[i].key);
                size_t idx = hash_val & new_mask;

                while (new_entries[idx].status == OCCUPIED) {
                    idx = (idx + 1) & new_mask;
                }

                new_entries[idx].key = std::move(entries_[i].key);
                new_entries[idx].value = std::move(entries_[i].value);
                new_entries[idx].status = OCCUPIED;
                active_count++;
            }
        }

        entries_ = std::move(new_entries);
        capacity_ = new_cap;
        mask_ = new_mask;
        size_ = active_count;
        slots_used_ = active_count;
    }

    inline bool insert(KeyType key, const ValueType& value) noexcept {
        if (slots_used_ * 4 >= capacity_ * 3) {
            reserve(capacity_ * 2);
        }

        size_t hash_val = Hash{}(key);
        size_t idx = hash_val & mask_;
        size_t first_deleted = static_cast<size_t>(-1);

        while (entries_[idx].status != EMPTY) {
            if (entries_[idx].status == OCCUPIED && entries_[idx].key == key) {
                entries_[idx].value = value;
                return true;
            }
            if (entries_[idx].status == DELETED && first_deleted == static_cast<size_t>(-1)) {
                first_deleted = idx;
            }
            idx = (idx + 1) & mask_;
        }

        if (first_deleted != static_cast<size_t>(-1)) {
            idx = first_deleted;
        } else {
            slots_used_++;
        }

        entries_[idx].key = key;
        entries_[idx].value = value;
        entries_[idx].status = OCCUPIED;
        size_++;
        return true;
    }

    [[nodiscard]] inline ValueType* find(KeyType key) noexcept {
        if (size_ == 0) return nullptr;

        size_t hash_val = Hash{}(key);
        size_t idx = hash_val & mask_;

        while (entries_[idx].status != EMPTY) {
            if (entries_[idx].status == OCCUPIED && entries_[idx].key == key) {
                return &entries_[idx].value;
            }
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    inline bool erase(KeyType key) noexcept {
        if (size_ == 0) return false;

        size_t hash_val = Hash{}(key);
        size_t idx = hash_val & mask_;

        while (entries_[idx].status != EMPTY) {
            if (entries_[idx].status == OCCUPIED && entries_[idx].key == key) {
                entries_[idx].status = DELETED;
                size_--;
                return true;
            }
            idx = (idx + 1) & mask_;
        }
        return false;
    }

    [[nodiscard]] inline size_t size() const noexcept { return size_; }
    [[nodiscard]] inline size_t capacity() const noexcept { return capacity_; }

private:
    EntryVector entries_;
    size_t capacity_{0};
    size_t mask_{0};
    size_t size_{0};
    size_t slots_used_{0};
};

} // namespace itch