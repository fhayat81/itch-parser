#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory_resource>
#include "DenseMap.hpp"

namespace itch {
    
struct PriceLevel;

// Packed to fit cleanly into cache lines (32 bytes aligned)
struct alignas(32) OrderMetaData {
    uint64_t order_id;
    uint32_t price;
    uint32_t shares;
    char side; // 'B' for Buy, 'S' for Sell
    
    // NEW: Stable pointer directly into the std::map's node memory
    PriceLevel* level_ptr; 
};

struct PriceLevel {
    uint32_t volume{0};
    uint32_t order_count{0};
};

template <typename Key, typename Value, typename Compare = std::less<Key>>
using PmrMap = std::map<Key, Value, Compare, std::pmr::polymorphic_allocator<std::pair<const Key, Value>>>;

class OrderBook {
private:
    PmrMap<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;
    PmrMap<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
    DenseMap<uint64_t, OrderMetaData> order_map_;

public:
    explicit OrderBook(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : bids_(mr),
          asks_(mr),
          order_map_(mr) {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    inline void add_order(uint64_t order_id, uint32_t price, uint32_t shares, char side) noexcept {
        PriceLevel* level_ptr = nullptr;

        if (side == 'B') {
            // Find or insert the price level
            auto it = bids_.find(price);
            if (it == bids_.end()) {
                it = bids_.emplace(price, PriceLevel{0, 0}).first;
            }
            it->second.volume += shares;
            it->second.order_count++;
            
            // Capture the stable memory address
            level_ptr = &(it->second);
        } else {
            auto it = asks_.find(price);
            if (it == asks_.end()) {
                it = asks_.emplace(price, PriceLevel{0, 0}).first;
            }
            it->second.volume += shares;
            it->second.order_count++;
            
            level_ptr = &(it->second);
        }

        order_map_.insert(order_id, OrderMetaData{order_id, price, shares, side, level_ptr});
    }

    inline void execute_order(uint64_t order_id, uint32_t executed_shares) noexcept {
        OrderMetaData* order = order_map_.find(order_id);
        if (!order) [[unlikely]] return;

        uint32_t actual_exec = std::min(executed_shares, order->shares);
        order->shares -= actual_exec;
        
        // O(1) Volume Deduction directly inside the std::map!
        order->level_ptr->volume -= actual_exec;

        if (order->shares == 0) {
            order->level_ptr->order_count--;
            
            // Only perform O(log N) tree operation if the price level is dead
            if (order->level_ptr->order_count == 0) {
                if (order->side == 'B') bids_.erase(order->price);
                else asks_.erase(order->price);
            }
            order_map_.erase(order_id);
        }
    }

    inline void cancel_order(uint64_t order_id, uint32_t cancel_shares) noexcept {
        execute_order(order_id, cancel_shares);
    }

    inline void delete_order(uint64_t order_id) noexcept {
        OrderMetaData* order = order_map_.find(order_id);
        if (!order) [[unlikely]] return;

        // O(1) Deduction
        order->level_ptr->volume -= order->shares;
        order->level_ptr->order_count--;

        // Only erase from the map if it's completely empty
        if (order->level_ptr->order_count == 0) {
            if (order->side == 'B') bids_.erase(order->price);
            else asks_.erase(order->price);
        }
        order_map_.erase(order_id);
    }

    inline void replace_order(uint64_t old_order_id, uint64_t new_order_id, uint32_t new_price, uint32_t new_shares) noexcept {
        OrderMetaData* old_order = order_map_.find(old_order_id);
        if (!old_order) [[unlikely]] return;

        char side = old_order->side;
        delete_order(old_order_id);
        add_order(new_order_id, new_price, new_shares, side);
    }

    [[nodiscard]] inline uint32_t best_bid() const noexcept {
        return bids_.empty() ? 0 : bids_.begin()->first;
    }

    [[nodiscard]] inline uint32_t best_ask() const noexcept {
        return asks_.empty() ? 0 : asks_.begin()->first;
    }

    [[nodiscard]] inline size_t get_order_count() const noexcept {
        return order_map_.size();
    }
};

} // namespace itch