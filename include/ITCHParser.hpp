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

namespace itch {

class ITCHParser {
private:
    // 1. Define 4 GB buffer size (4 * 1024 * 1024 * 1024 bytes)
    static constexpr size_t POOL_SIZE = 4ULL * 1024 * 1024 * 1024;

    // Raw pointer for the mmap arena
    void* arena_buffer_;

    // PMR Monotonic Buffer Resource wrapping the 4 GB arena
    std::pmr::monotonic_buffer_resource pool_;

    // Order books indexed by locate ID (0..15000)
    std::vector<OrderBook> order_books_;

    // Lookup table
    std::vector<std::string> locate_to_symbol_;
    
    uint64_t total_messages_{0};
    uint64_t add_orders_{0};
    uint64_t executed_orders_{0};
    uint64_t cancelled_orders_{0};
    uint64_t replaced_orders_{0};

    // --- Jump Table Typedef and Array ---
    using MessageHandler = void (ITCHParser::*)(const uint8_t*, uint16_t) noexcept;
    std::array<MessageHandler, 256> dispatch_table_{};

    // --- Memory Allocation Helper ---
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

    // --- Utility Methods ---
    static uint64_t parse_itch_timestamp(const uint8_t* ptr) {
        uint64_t raw;
        std::memcpy(&raw, ptr, sizeof(raw)); 
        return __builtin_bswap64(raw) >> 16; 
    }

    static std::string format_raw_time(uint64_t nanos) {
        uint64_t total_sec = nanos / 1'000'000'000ULL;
        uint64_t rem_nanos = nanos % 1'000'000'000ULL;
        
        uint32_t hours   = (total_sec / 3600) % 24;
        uint32_t minutes = (total_sec / 60) % 60;
        uint32_t seconds = total_sec % 60;

        std::ostringstream ss;
        ss << std::setfill('0') 
           << std::setw(2) << hours << ":"
           << std::setw(2) << minutes << ":"
           << std::setw(2) << seconds << "."
           << std::setw(9) << rem_nanos;
        return ss.str();
    }

    void print_order_book_snapshot(const std::string& label) const {
        std::cout << "\n\n===================================================================\n";
        std::cout << "          ORDER BOOK SNAPSHOT: " << label << "\n";
        std::cout << "===================================================================\n";
        std::cout << std::left 
                  << std::setw(10) << "TICKER"
                  << std::setw(12) << "LOCATE ID"
                  << std::setw(16) << "ACTIVE ORDERS"
                  << std::setw(14) << "BEST BID"
                  << std::setw(14) << "BEST ASK" << "\n";
        std::cout << "-------------------------------------------------------------------\n";

        static const std::vector<uint16_t> watchlist_ids = {
            1, 2, 5, 10, 25, 50, 100, 500
        };

        for (uint16_t locate : watchlist_ids) {
            if (locate >= order_books_.size()) continue;

            const auto& book = get_order_book(locate);
            uint64_t active_orders = book.get_order_count();
            
            std::string sym = get_symbol_by_locate(locate);
            std::string bid_str = (book.best_bid() > 0) ? std::to_string(book.best_bid() / 10000.0) : "N/A";
            std::string ask_str = (book.best_ask() > 0) ? std::to_string(book.best_ask() / 10000.0) : "N/A";

            std::cout << std::left 
                      << std::setw(10) << sym
                      << std::setw(12) << locate
                      << std::setw(16) << active_orders
                      << std::setw(14) << bid_str
                      << std::setw(14) << ask_str << "\n";
        }
        std::cout << "===================================================================\n\n";
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

    inline void handle_system_event(const uint8_t* msg_ptr, uint16_t msg_len) noexcept {
        if (msg_len >= 12) {
            uint64_t raw_nanos = parse_itch_timestamp(msg_ptr + 5);
            char event_code = static_cast<char>(msg_ptr[11]);
            std::string raw_time = format_raw_time(raw_nanos);

            if (event_code == 'Q') {
                std::cout << "\n\n[SYSTEM EVENT 'Q'] Start of Market Hours | Timestamp: " 
                          << raw_time << " (" << raw_nanos << " ns)\n" << std::flush;
            } else if (event_code == 'M') {
                std::cout << "\n\n[SYSTEM EVENT 'M'] End of Market Hours | Timestamp: " 
                          << raw_time << " (" << raw_nanos << " ns)" << std::flush;
                print_order_book_snapshot("Market Close ('M')");
            } else if (event_code == 'E') {
                std::cout << "\n\n[SYSTEM EVENT 'E'] End of System Hours | Timestamp: " 
                          << raw_time << " (" << raw_nanos << " ns)\n" << std::flush;
            }
        }
    }

public:
    explicit ITCHParser(size_t max_locates = 15000)
        : arena_buffer_(allocate_arena(POOL_SIZE)),
          pool_(arena_buffer_, POOL_SIZE, std::pmr::null_memory_resource()) {
        
        order_books_.reserve(max_locates);
        locate_to_symbol_.resize(max_locates);

        for (size_t i = 0; i < max_locates; ++i) {
            order_books_.emplace_back(&pool_);
        }

        // Initialize jump table with ignore handlers
        dispatch_table_.fill(&ITCHParser::handle_ignore);
        
        // Map specific message types
        dispatch_table_['A'] = &ITCHParser::handle_add_order;
        dispatch_table_['F'] = &ITCHParser::handle_add_order_mpid;
        dispatch_table_['E'] = &ITCHParser::handle_order_executed;
        dispatch_table_['C'] = &ITCHParser::handle_order_executed;
        dispatch_table_['X'] = &ITCHParser::handle_order_cancel;
        dispatch_table_['D'] = &ITCHParser::handle_order_delete;
        dispatch_table_['U'] = &ITCHParser::handle_order_replace;
        dispatch_table_['R'] = &ITCHParser::handle_stock_directory;
        dispatch_table_['S'] = &ITCHParser::handle_system_event;
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
        uint32_t progress_counter = 10'000'000;

        while (offset + 2 < filesize) {
            uint16_t msg_len = (static_cast<uint16_t>(buffer[offset]) << 8) | buffer[offset + 1];
            offset += 2;

            if (offset + msg_len > filesize) break;

            const uint8_t* msg_ptr = buffer + offset;
            
            // O(1) Jump Table Dispatch
            uint8_t msg_type = msg_ptr[0];
            (this->*dispatch_table_[msg_type])(msg_ptr, msg_len);

            total_messages_++;

            if (--progress_counter == 0) {
                progress_counter = 10'000'000;
                double pct = (static_cast<double>(offset) / filesize) * 100.0;
                std::cout << "\r[Processing] " << (total_messages_ / 1'000'000) 
                          << "M messages | " << std::fixed << std::setprecision(1) 
                          << pct << "% complete" << std::flush;
            }

            offset += msg_len;
        }

        std::cout << "\r[Complete] Processed " << total_messages_ 
                  << " total messages (100.0%).        \n" << std::flush;

        munmap(const_cast<uint8_t*>(buffer), filesize);
        close(fd);
        return true;
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
};

} // namespace itch