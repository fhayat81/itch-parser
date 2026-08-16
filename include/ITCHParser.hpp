#pragma once

#include "ITCHStructs.hpp"
#include "ByteUtils.hpp"
#include "OrderBook.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <iomanip>
#include <memory_resource>
#include <array>
#include <algorithm>
#include <chrono> // Added for nanosecond latency tracking
#include <x86intrin.h>

namespace itch {

class ITCHParser {
private:
    static constexpr size_t POOL_SIZE = 4ULL * 1024 * 1024 * 1024;

    void* arena_buffer_;
    std::pmr::monotonic_buffer_resource pool_;

    std::vector<OrderBook> order_books_;
    std::vector<std::string> locate_to_symbol_;
    
    // Per-message latency tracking (in nanoseconds)
    std::vector<uint64_t> latencies_;
    
    uint64_t total_messages_{0};
    uint64_t add_orders_{0};
    uint64_t executed_orders_{0};
    uint64_t cancelled_orders_{0};
    uint64_t replaced_orders_{0};
    size_t file_size_{0};

    using MessageHandler = void (ITCHParser::*)(const uint8_t*, uint16_t) noexcept;
    std::array<MessageHandler, 256> dispatch_table_{};

    static void* allocate_arena(size_t size) {
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_HUGETLB)
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0);
        if (ptr != MAP_FAILED) return ptr;
#endif
        void* fallback = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (fallback == MAP_FAILED) throw std::bad_alloc();
        return fallback;
    }

    // --- Message Handlers ---

    inline void handle_ignore(const uint8_t*, uint16_t) noexcept {}

    inline void handle_add_order(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* add_msg = reinterpret_cast<const AddOrderMessage*>(msg_ptr);
        uint16_t locate = bswap16(add_msg->header.stock_locate);
        uint64_t order_id = bswap64(add_msg->order_reference_number);
        uint32_t shares = bswap32(add_msg->shares);
        uint32_t price = bswap32(add_msg->price);

        order_books_[locate].add_order(order_id, price, shares, add_msg->buy_sell_indicator);
        add_orders_++;
    }

    inline void handle_add_order_mpid(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* add_msg = reinterpret_cast<const AddOrderMessage*>(msg_ptr);
        uint16_t locate = bswap16(add_msg->header.stock_locate);
        uint64_t order_id = bswap64(add_msg->order_reference_number);
        uint32_t shares = bswap32(add_msg->shares);
        uint32_t price = bswap32(add_msg->price);

        order_books_[locate].add_order(order_id, price, shares, add_msg->buy_sell_indicator);
        add_orders_++;
    }

    inline void handle_order_executed(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* exec_msg = reinterpret_cast<const OrderExecutedMessage*>(msg_ptr);
        uint16_t locate = bswap16(exec_msg->header.stock_locate);
        uint64_t order_id = bswap64(exec_msg->order_reference_number);
        uint32_t exec_shares = bswap32(exec_msg->executed_shares);

        order_books_[locate].execute_order(order_id, exec_shares);
        executed_orders_++;
    }

    inline void handle_order_cancel(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* cancel_msg = reinterpret_cast<const OrderCancelMessage*>(msg_ptr);
        uint16_t locate = bswap16(cancel_msg->header.stock_locate);
        uint64_t order_id = bswap64(cancel_msg->order_reference_number);
        uint32_t cancel_shares = bswap32(cancel_msg->canceled_shares);

        order_books_[locate].cancel_order(order_id, cancel_shares);
        cancelled_orders_++;
    }

    inline void handle_order_delete(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* del_msg = reinterpret_cast<const OrderDeleteMessage*>(msg_ptr);
        uint16_t locate = bswap16(del_msg->header.stock_locate);
        uint64_t order_id = bswap64(del_msg->order_reference_number);

        order_books_[locate].delete_order(order_id);
        cancelled_orders_++;
    }

    inline void handle_order_replace(const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* rep_msg = reinterpret_cast<const OrderReplaceMessage*>(msg_ptr);
        uint16_t locate = bswap16(rep_msg->header.stock_locate);
        uint64_t old_id = bswap64(rep_msg->original_order_reference_number);
        uint64_t new_id = bswap64(rep_msg->new_order_reference_number);
        uint32_t shares = bswap32(rep_msg->shares);
        uint32_t price = bswap32(rep_msg->price);

        order_books_[locate].replace_order(old_id, new_id, price, shares);
        replaced_orders_++;
    }

    inline void handle_stock_directory(const uint8_t* msg_ptr, uint16_t msg_len) noexcept {
        if (msg_len >= 29) {
            uint16_t locate = (static_cast<uint16_t>(msg_ptr[1]) << 8) | msg_ptr[2];
            char sym_buf[9] = {0};
            std::memcpy(sym_buf, msg_ptr + 11, 8);
            
            std::string symbol(sym_buf);
            symbol.erase(symbol.find_last_not_of(" \n\r\t") + 1);

            if (locate < locate_to_symbol_.size()) {
                locate_to_symbol_[locate] = symbol;
            }
        }
    }

public:
    explicit ITCHParser(size_t max_locates = 15000)
        : arena_buffer_(allocate_arena(POOL_SIZE)),
          pool_(arena_buffer_, POOL_SIZE, std::pmr::null_memory_resource()) {
        
        order_books_.reserve(max_locates);
        locate_to_symbol_.resize(max_locates);
        
        // Pre-allocate storage for 400M latency entries to avoid reallocations
        latencies_.reserve(400'000'000);

        for (size_t i = 0; i < max_locates; ++i) {
            order_books_.emplace_back(&pool_);
        }

        dispatch_table_.fill(&ITCHParser::handle_ignore);
        
        dispatch_table_['A'] = &ITCHParser::handle_add_order;
        dispatch_table_['F'] = &ITCHParser::handle_add_order_mpid;
        dispatch_table_['E'] = &ITCHParser::handle_order_executed;
        dispatch_table_['C'] = &ITCHParser::handle_order_executed;
        dispatch_table_['X'] = &ITCHParser::handle_order_cancel;
        dispatch_table_['D'] = &ITCHParser::handle_order_delete;
        dispatch_table_['U'] = &ITCHParser::handle_order_replace;
        dispatch_table_['R'] = &ITCHParser::handle_stock_directory;
    }

    ~ITCHParser() {
        if (arena_buffer_ && arena_buffer_ != MAP_FAILED) {
            munmap(arena_buffer_, POOL_SIZE);
        }
    }

    bool process_file(const char* filepath) {
        int fd = open(filepath, O_RDONLY);
        if (fd == -1) {
            std::cerr << "Error opening binary file: " << filepath << std::endl;
            return false;
        }

        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            close(fd);
            return false;
        }

        size_t filesize = sb.st_size;
        file_size_ = filesize;
        auto* buffer = static_cast<const uint8_t*>(
            mmap(nullptr, filesize, PROT_READ, MAP_PRIVATE, fd, 0)
        );

        if (buffer == MAP_FAILED) {
            close(fd);
            return false;
        }

        #ifdef POSIX_MADV_SEQUENTIAL
        madvise(const_cast<uint8_t*>(buffer), filesize, MADV_SEQUENTIAL);
        #endif

        size_t offset = 0;

        while (offset + 2 < filesize) {
            uint16_t msg_len = (static_cast<uint16_t>(buffer[offset]) << 8) | buffer[offset + 1];
            offset += 2;

            if (offset + msg_len > filesize) break;

            const uint8_t* msg_ptr = buffer + offset;
            uint8_t msg_type = msg_ptr[0];
            
            // Replaced __rdtsc() with std::chrono for nanosecond timing
            auto start_ns = std::chrono::steady_clock::now();
            (this->*dispatch_table_[msg_type])(msg_ptr, msg_len);
            auto end_ns = std::chrono::steady_clock::now();

            uint64_t duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ns - start_ns).count();
            latencies_.push_back(duration_ns);

            total_messages_++;
            offset += msg_len;
        }

        munmap(const_cast<uint8_t*>(buffer), filesize);
        close(fd);
        return true;
    }

    void print_latency_metrics() {
        if (latencies_.empty()) return;

        size_t n = latencies_.size();
        size_t p50_index = n * 0.50;
        size_t p99_index = n * 0.99;
        size_t p999_index = n * 0.999;

        // Calculate Average
        uint64_t total_ns = 0;
        for (uint64_t ns : latencies_) {
            total_ns += ns;
        }
        uint64_t avg_ns = total_ns / n;

        // Calculate Percentiles
        std::nth_element(latencies_.begin(), latencies_.begin() + p50_index, latencies_.end());
        uint64_t p50 = latencies_[p50_index];

        std::nth_element(latencies_.begin(), latencies_.begin() + p99_index, latencies_.end());
        uint64_t p99 = latencies_[p99_index];

        std::nth_element(latencies_.begin(), latencies_.begin() + p999_index, latencies_.end());
        uint64_t p999 = latencies_[p999_index];

        auto max_it = std::max_element(latencies_.begin(), latencies_.end());
        uint64_t max_ns = *max_it;

        std::cout << "\n========================================\n";
        std::cout << "    TAIL LATENCY METRICS (Nanoseconds)    \n";
        std::cout << "========================================\n";
        std::cout << std::left << std::setw(15) << "Average"      << ": " << avg_ns << " ns\n";
        std::cout << std::left << std::setw(15) << "p50 (Median)" << ": " << p50 << " ns\n";
        std::cout << std::left << std::setw(15) << "p99"          << ": " << p99 << " ns\n";
        std::cout << std::left << std::setw(15) << "p99.9"        << ": " << p999 << " ns\n";
        std::cout << std::left << std::setw(15) << "Maximum"      << ": " << max_ns << " ns\n";
        std::cout << "========================================\n\n";
    }

    std::string get_symbol_by_locate(uint16_t locate_id) const {
        if (locate_id < locate_to_symbol_.size() && !locate_to_symbol_[locate_id].empty()) {
            return locate_to_symbol_[locate_id];
        }
        return "UNKNOWN";
    }

    const OrderBook& get_order_book(uint16_t locate_id) const noexcept {
        return order_books_[locate_id];
    }

    [[nodiscard]] uint64_t total_messages() const noexcept { return total_messages_; }
    [[nodiscard]] uint64_t add_orders() const noexcept { return add_orders_; }
    [[nodiscard]] uint64_t executed_orders() const noexcept { return executed_orders_; }
    [[nodiscard]] uint64_t cancelled_orders() const noexcept { return cancelled_orders_; }
    [[nodiscard]] uint64_t replaced_orders() const noexcept { return replaced_orders_; }
    [[nodiscard]] size_t file_size() const noexcept { return file_size_; }
};

} // namespace itch