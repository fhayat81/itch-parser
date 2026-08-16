#pragma once

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include "DenseMap.hpp"
#include "MemoryPool.hpp"

namespace itch {

struct PriceLevel;

// 64-byte aligned for perfect L1 cache line utilization
struct alignas(64) OrderNode {
    uint64_t order_id{0};
    uint32_t shares{0};
    uint32_t price{0};
    char side{'B'};

    PriceLevel* level{nullptr};
    OrderNode* prev_order{nullptr};

    union {
        OrderNode* next_order;
        OrderNode* next_free;
    };
};

struct PriceLevel {
    uint32_t price{0};
    uint32_t order_count{0};
    uint64_t total_volume{0};

    OrderNode* head_order{nullptr};
    OrderNode* tail_order{nullptr};

    PriceLevel* prev_level{nullptr};
    union {
        PriceLevel* next_level;
        PriceLevel* next_free;
    };

    inline void append_order(OrderNode* node) noexcept {
        node->level = this;
        node->next_order = nullptr;
        node->prev_order = tail_order;
        if (tail_order) [[likely]] {
            tail_order->next_order = node;
        } else {
            head_order = node;
        }
        tail_order = node;
        total_volume += node->shares;
        order_count++;
    }

    inline void detach_order(OrderNode* node) noexcept {
        if (node->prev_order) {
            node->prev_order->next_order = node->next_order;
        } else {
            head_order = node->next_order;
        }
        if (node->next_order) {
            node->next_order->prev_order = node->prev_order;
        } else {
            tail_order = node->prev_order;
        }
        total_volume -= node->shares;
        order_count--;
        node->prev_order = nullptr;
        node->next_order = nullptr;
    }
};

class OrderBookSide {
private:
    PriceLevel* head_{nullptr};
    PriceLevel* tail_{nullptr};
    bool is_bid_;

public:
    explicit OrderBookSide(bool is_bid) : is_bid_(is_bid) {}

    [[nodiscard]] inline uint32_t best_price() const noexcept {
        return head_ ? head_->price : 0;
    }

    // Combined Linear Search and Insertion with Rolling Prefetch
    inline PriceLevel* get_or_create(uint32_t price, MemoryPool<PriceLevel>* pool) noexcept {
        if (!head_) [[unlikely]] {
            PriceLevel* new_lvl = pool->allocate();
            new_lvl->price = price;
            new_lvl->order_count = 0;
            new_lvl->total_volume = 0;
            new_lvl->head_order = new_lvl->tail_order = nullptr;
            new_lvl->prev_level = new_lvl->next_level = nullptr;
            head_ = tail_ = new_lvl;
            return new_lvl;
        }

        PriceLevel* curr = head_;
        
        while (curr) {
            if (curr->price == price) return curr;

            // Rolling Prefetch: Fetch the next node while processing the current one
            if (curr->next_level) {
                __builtin_prefetch(curr->next_level, 0, 1);
            }

            // Bids descend (highest first). If curr < price, we passed it.
            // Asks ascend (lowest first). If curr > price, we passed it.
            bool passed_insertion_point = is_bid_ ? (curr->price < price) : (curr->price > price);
            if (passed_insertion_point) break;

            
            curr = curr->next_level;
        }

        // Not found. We must allocate and insert it before 'curr' (or at the tail if curr is null)
        PriceLevel* new_lvl = pool->allocate();
        new_lvl->price = price;
        new_lvl->order_count = 0;
        new_lvl->total_volume = 0;
        new_lvl->head_order = new_lvl->tail_order = nullptr;

        if (curr) {
            // Insert before curr
            new_lvl->prev_level = curr->prev_level;
            new_lvl->next_level = curr;
            if (curr->prev_level) {
                curr->prev_level->next_level = new_lvl;
            } else {
                head_ = new_lvl; // We are the new best price
            }
            curr->prev_level = new_lvl;
        } else {
            // Reached the end of the list, append to tail
            new_lvl->prev_level = tail_;
            new_lvl->next_level = nullptr;
            tail_->next_level = new_lvl;
            tail_ = new_lvl;
        }

        return new_lvl;
    }

    inline void remove_level(PriceLevel* lvl) noexcept {
        if (lvl->prev_level) {
            lvl->prev_level->next_level = lvl->next_level;
        } else {
            head_ = lvl->next_level;
        }

        if (lvl->next_level) {
            lvl->next_level->prev_level = lvl->prev_level;
        } else {
            tail_ = lvl->prev_level;
        }
        lvl->prev_level = nullptr;
        lvl->next_level = nullptr;
    }
};

class OrderBook {
private:
    OrderBookSide bids_{true};
    OrderBookSide asks_{false};

    // We only keep the order map; price levels are fully managed by the linked lists
    DenseMap<uint64_t, OrderNode*> order_map_;

    MemoryPool<OrderNode>* order_pool_;
    MemoryPool<PriceLevel>* level_pool_;

public:
    explicit OrderBook(MemoryPool<OrderNode>* order_pool, 
                       MemoryPool<PriceLevel>* level_pool, 
                       std::pmr::memory_resource* pmr)
        : order_map_(pmr),
          order_pool_(order_pool),
          level_pool_(level_pool) {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    inline void add_order(uint64_t order_id, uint32_t price, uint32_t shares, char side) noexcept {
        OrderNode* node = order_pool_->allocate();
        if (!node) [[unlikely]] return;

        node->order_id = order_id;
        node->price = price;
        node->shares = shares;
        node->side = side;

        // Directly query the linked list for the level
        PriceLevel* level = (side == 'B') ? bids_.get_or_create(price, level_pool_) 
                                          : asks_.get_or_create(price, level_pool_);
        level->append_order(node);
        order_map_.insert(order_id, node);
    }

    inline void execute_order(uint64_t order_id, uint32_t executed_shares) noexcept {
        OrderNode** node_ptr = order_map_.find(order_id);
        if (!node_ptr || !*node_ptr) [[unlikely]] return;

        OrderNode* node = *node_ptr;
        uint32_t actual_exec = std::min(executed_shares, node->shares);
        
        node->shares -= actual_exec;
        node->level->total_volume -= actual_exec;

        if (node->shares == 0) {
            delete_order_internal(node);
        }
    }

    inline void cancel_order(uint64_t order_id, uint32_t cancel_shares) noexcept {
        execute_order(order_id, cancel_shares);
    }

    inline void delete_order(uint64_t order_id) noexcept {
        OrderNode** node_ptr = order_map_.find(order_id);
        if (!node_ptr || !*node_ptr) [[unlikely]] return;
        delete_order_internal(*node_ptr);
    }

    inline void replace_order(uint64_t old_order_id, uint64_t new_order_id, uint32_t new_price, uint32_t new_shares) noexcept {
        OrderNode** node_ptr = order_map_.find(old_order_id);
        if (!node_ptr || !*node_ptr) [[unlikely]] return;

        char side = (*node_ptr)->side;
        delete_order_internal(*node_ptr);
        add_order(new_order_id, new_price, new_shares, side);
    }

    [[nodiscard]] inline uint32_t best_bid() const noexcept { return bids_.best_price(); }
    [[nodiscard]] inline uint32_t best_ask() const noexcept { return asks_.best_price(); }
    [[nodiscard]] inline size_t get_order_count() const noexcept { return order_map_.size(); }

private:
    inline void delete_order_internal(OrderNode* node) noexcept {
        PriceLevel* level = node->level;
        uint64_t order_id = node->order_id;
        char side = node->side;

        level->detach_order(node);
        order_map_.erase(order_id);
        order_pool_->deallocate(node);

        // If the price level is empty, remove it from the linked list and return to pool
        if (level->order_count == 0) {
            if (side == 'B') bids_.remove_level(level);
            else asks_.remove_level(level);
            
            level_pool_->deallocate(level);
        }
    }
};

} // namespace itch