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
#include <unordered_map>
#include <string>
#include <cstring>
#include <iomanip>

namespace itch {

class ITCHParser {
private:
    // 1. Define 4 GB buffer size (1024 * 1024 * 1024 bytes)
    static constexpr size_t POOL_SIZE = 4ULL * 1024 * 1024 * 1024;

    // Allocate 4 GB on heap via unique_ptr to avoid overflowing executable stack
    std::unique_ptr<char[]> arena_buffer_;

    // PMR Monotonic Buffer Resource wrapping the 4 GB arena
    std::pmr::monotonic_buffer_resource pool_;

    // Order books indexed by locate ID (0..65535)
    std::vector<OrderBook> order_books_;

    // Lookup table: Ticker Symbol -> Stock Locate ID
    std::unordered_map<std::string, uint16_t> symbol_to_locate_;
    std::unordered_map<uint16_t, std::string> locate_to_symbol_;

    uint64_t count_inside {0};
    uint64_t count_outside {0};

    uint32_t minPrice {2147483647};
    uint32_t maxPrice {0};

    inline void check(uint32_t x){
        // if(x < minPrice) minPrice = x;
        // if(x > maxPrice) maxPrice = x;

        if(x % 100 != 0) {
            count_inside++;
            maxPrice = std::max(maxPrice, x);
            minPrice = std::min(minPrice, x);
        }
        else count_outside++;
    }
    
    uint64_t total_messages_{0};
    uint64_t add_orders_{0};
    uint64_t executed_orders_{0};
    uint64_t cancelled_orders_{0};
    uint64_t replaced_orders_{0};

    // Parse 6-byte big-endian timestamp starting at byte index 5 (nanoseconds since midnight)
    static uint64_t parse_itch_timestamp(const uint8_t* ptr) {
        return (static_cast<uint64_t>(ptr[0]) << 40) |
               (static_cast<uint64_t>(ptr[1]) << 32) |
               (static_cast<uint64_t>(ptr[2]) << 24) |
               (static_cast<uint64_t>(ptr[3]) << 16) |
               (static_cast<uint64_t>(ptr[4]) << 8)  |
                static_cast<uint64_t>(ptr[5]);
    }

    // Format raw nanoseconds since midnight into HH:MM:SS.nanoseconds
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

    // Print active order book snapshot for watchlist tickers
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

        static const std::vector<std::string> watchlist = {
            "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "TSLA", "QQQ", "SPY"
        };

        for (const auto& sym : watchlist) {
            uint16_t locate = get_locate_by_symbol(sym);
            if (locate == 0) continue;

            const auto& book = get_order_book(locate);
            uint64_t active_orders = book.get_order_count();
            
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

        std::cout << "MAX PRICE: " << maxPrice << "\n";
        std::cout << "MIN PRICE: " << minPrice << "\n";
        std::cout << "RANGE (inner): " << count_inside << "\n";
        std::cout << "RANGE (outer): " << count_outside << "\n\n";

        std::cout << "===================================================================\n\n";

    }

public:
    explicit ITCHParser(size_t max_locates = 15000)
        : arena_buffer_(std::make_unique<char[]>(POOL_SIZE)),
          // Initialize pool with arena_buffer_.
          // std::pmr::null_memory_resource() causes throw on depletion instead of silent heap fallback.
          pool_(arena_buffer_.get(), POOL_SIZE, std::pmr::null_memory_resource()) {
        
        order_books_.reserve(max_locates);
        
        // Construct each OrderBook explicitly with a pointer to our PMR pool
        for (size_t i = 0; i < max_locates; ++i) {
            order_books_.emplace_back(&pool_);
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

        // NASDAQ ITCH binary file format uses 2-byte big-endian framing per message
        while (offset + 2 < filesize) {
            // Read 2-byte length header prefix
            uint16_t msg_len = (static_cast<uint16_t>(buffer[offset]) << 8) | buffer[offset + 1];
            offset += 2;

            if (offset + msg_len > filesize) break;

            const uint8_t* msg_ptr = buffer + offset;
            char msg_type = static_cast<char>(msg_ptr[0]);

            switch (msg_type) {
                case 'S': { // System Event Message
                    if (msg_len >= 12) {
                        uint64_t raw_nanos = parse_itch_timestamp(msg_ptr + 5);
                        char event_code = static_cast<char>(msg_ptr[11]);
                        std::string raw_time = format_raw_time(raw_nanos);

                        if (event_code == 'Q') { // Start of Market Hours
                            std::cout << "\n\n[SYSTEM EVENT 'Q'] Start of Market Hours | Timestamp: " 
                                      << raw_time << " (" << raw_nanos << " ns)\n" << std::flush;
                        } 
                        else if (event_code == 'M') { // End of Market Hours
                            std::cout << "\n\n[SYSTEM EVENT 'M'] End of Market Hours | Timestamp: " 
                                      << raw_time << " (" << raw_nanos << " ns)" << std::flush;
                            
                            // Snapshot active order book right at Market Close
                            print_order_book_snapshot("Market Close ('M')");
                        } 
                        else if (event_code == 'E') { // End of System Hours
                            std::cout << "\n\n[SYSTEM EVENT 'E'] End of System Hours | Timestamp: " 
                                      << raw_time << " (" << raw_nanos << " ns)\n" << std::flush;
                        }
                    }
                    break;
                }
                case 'R': { // Stock Directory (Maps Locate ID to Symbol string)
                    if (msg_len >= 29) {
                        uint16_t locate = (static_cast<uint16_t>(msg_ptr[1]) << 8) | msg_ptr[2];
                        char sym_buf[9] = {0};
                        std::memcpy(sym_buf, msg_ptr + 11, 8);
                        
                        // Trim trailing spaces
                        std::string symbol(sym_buf);
                        symbol.erase(symbol.find_last_not_of(" \n\r\t") + 1);

                        symbol_to_locate_[symbol] = locate;
                        locate_to_symbol_[locate] = symbol;
                    }
                    break;
                }
                case 'A': { // Add Order (Short Form)
                    const auto* add_msg = reinterpret_cast<const AddOrderMessage*>(msg_ptr);
                    uint16_t locate = bswap16(add_msg->header.stock_locate);
                    uint64_t order_id = bswap64(add_msg->order_reference_number);
                    uint32_t shares = bswap32(add_msg->shares);
                    uint32_t price = bswap32(add_msg->price);
                    check(price);

                    order_books_[locate].add_order(order_id, price, shares, add_msg->buy_sell_indicator);
                    add_orders_++;
                    break;
                }
                case 'F': { // Add Order with MPID (Long Form)
                    const auto* add_msg = reinterpret_cast<const AddOrderMessage*>(msg_ptr);
                    uint16_t locate = bswap16(add_msg->header.stock_locate);
                    uint64_t order_id = bswap64(add_msg->order_reference_number);
                    uint32_t shares = bswap32(add_msg->shares);
                    uint32_t price = bswap32(add_msg->price);
                    check(price);

                    order_books_[locate].add_order(order_id, price, shares, add_msg->buy_sell_indicator);
                    add_orders_++;
                    break;
                }
                case 'E':   // Order Executed
                case 'C': { // Order Executed with Price
                    const auto* exec_msg = reinterpret_cast<const OrderExecutedMessage*>(msg_ptr);
                    uint16_t locate = bswap16(exec_msg->header.stock_locate);
                    uint64_t order_id = bswap64(exec_msg->order_reference_number);
                    uint32_t exec_shares = bswap32(exec_msg->executed_shares);

                    order_books_[locate].execute_order(order_id, exec_shares);
                    executed_orders_++;
                    break;
                }
                case 'X': { // Order Cancel
                    const auto* cancel_msg = reinterpret_cast<const OrderCancelMessage*>(msg_ptr);
                    uint16_t locate = bswap16(cancel_msg->header.stock_locate);
                    uint64_t order_id = bswap64(cancel_msg->order_reference_number);
                    uint32_t cancel_shares = bswap32(cancel_msg->canceled_shares);

                    order_books_[locate].cancel_order(order_id, cancel_shares);
                    cancelled_orders_++;
                    break;
                }
                case 'D': { // Order Delete
                    const auto* del_msg = reinterpret_cast<const OrderDeleteMessage*>(msg_ptr);
                    uint16_t locate = bswap16(del_msg->header.stock_locate);
                    uint64_t order_id = bswap64(del_msg->order_reference_number);

                    order_books_[locate].delete_order(order_id);
                    cancelled_orders_++;
                    break;
                }
                case 'U': { // Order Replace
                    const auto* rep_msg = reinterpret_cast<const OrderReplaceMessage*>(msg_ptr);
                    uint16_t locate = bswap16(rep_msg->header.stock_locate);
                    uint64_t old_id = bswap64(rep_msg->original_order_reference_number);
                    uint64_t new_id = bswap64(rep_msg->new_order_reference_number);
                    uint32_t shares = bswap32(rep_msg->shares);
                    uint32_t price = bswap32(rep_msg->price);
                    check(price);

                    order_books_[locate].replace_order(old_id, new_id, price, shares);
                    replaced_orders_++;
                    break;
                }
                default:
                    break;
            }

            total_messages_++;

            // Print progress line every 10,000,000 messages
            if (total_messages_ % 10'000'000 == 0) {
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

    uint16_t get_locate_by_symbol(const std::string& symbol) const {
        auto it = symbol_to_locate_.find(symbol);
        return (it != symbol_to_locate_.end()) ? it->second : 0;
    }

    std::string get_symbol_by_locate(uint16_t locate_id) const {
        auto it = locate_to_symbol_.find(locate_id);
        return (it != locate_to_symbol_.end()) ? it->second : "UNKNOWN";
    }

    const OrderBook& get_order_book(uint16_t locate_id) const noexcept {
        return order_books_[locate_id];
    }

    const OrderBook* get_order_book(const std::string& symbol) const {
        uint16_t locate = get_locate_by_symbol(symbol);
        return (locate != 0) ? &order_books_[locate] : nullptr;
    }

    [[nodiscard]] uint64_t total_messages() const noexcept { return total_messages_; }
    [[nodiscard]] uint64_t add_orders() const noexcept { return add_orders_; }
    [[nodiscard]] uint64_t executed_orders() const noexcept { return executed_orders_; }
    [[nodiscard]] uint64_t cancelled_orders() const noexcept { return cancelled_orders_; }
    [[nodiscard]] uint64_t replaced_orders() const noexcept { return replaced_orders_; }
};

} // namespace itch