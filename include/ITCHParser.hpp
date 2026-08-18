#pragma once

#include "ITCHStructs.hpp"
#include "ByteUtils.hpp"
#include "OrderBook.hpp"
#include "SPSCQueue.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <emmintrin.h>
#include <x86intrin.h>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <iomanip>
#include <memory_resource>
#include <array>
#include <thread>
#include <atomic>
#include <chrono>

namespace itch {

struct QueueTask {
    const uint8_t* msg_ptr{nullptr};
    uint16_t msg_len{0};
};

class ITCHParser {
public:
    static constexpr size_t NUM_WORKERS = 3;
    static constexpr size_t WORKER_ARENA_SIZE = 2ULL * 1024 * 1024 * 1024; // 2 GB per worker
    static constexpr size_t MAX_LOCATES = 15000;
    static constexpr size_t QUEUE_CAPACITY = 65536;

    struct alignas(64) WorkerContext {
        size_t worker_id;
        int cpu_id;
        ITCHParser* parser;

        void* arena_buffer{nullptr};
        std::unique_ptr<std::pmr::monotonic_buffer_resource> pool;

        uint64_t total_messages{0};
        uint64_t add_orders{0};
        uint64_t executed_orders{0};
        uint64_t cancelled_orders{0};
        uint64_t replaced_orders{0};

        std::array<uint64_t, 100000> latency_buckets{};
        uint64_t latency_overflow{0};
        uint64_t max_latency_ns{0};
    };

private:
    int producer_cpu_{3};
    std::array<int, NUM_WORKERS> worker_cpus_{5, 7, 9};

    std::array<WorkerContext, NUM_WORKERS> worker_contexts_;
    std::array<SPSCQueue<QueueTask, QUEUE_CAPACITY>, NUM_WORKERS> queues_;
    std::array<std::thread, NUM_WORKERS> worker_threads_;

    std::vector<OrderBook> order_books_;
    std::vector<std::string> locate_to_symbol_;
    
    size_t file_size_{0};
    double cycles_to_ns_multiplier_{0.0};

    using MessageHandler = void (ITCHParser::*)(WorkerContext&, const uint8_t*, uint16_t) noexcept;
    std::array<MessageHandler, 256> dispatch_table_{};

    static void pin_thread(int cpu_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

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

    void calibrate_tsc() {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t c0 = __rdtsc();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 10) {}
        uint64_t c1 = __rdtsc();
        auto t1 = std::chrono::steady_clock::now();
        double elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        cycles_to_ns_multiplier_ = elapsed_ns / (c1 - c0);
    }

    // --- Message Handlers (Executes inside isolated worker context) ---

    inline void handle_ignore(WorkerContext&, const uint8_t*, uint16_t) noexcept {}

    inline void handle_add_order(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const AddOrderMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].add_order(
            bswap64(msg->order_reference_number),
            bswap32(msg->price),
            bswap32(msg->shares),
            msg->buy_sell_indicator
        );
        ctx.add_orders++;
    }

    inline void handle_add_order_mpid(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const AddOrderMPIDMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].add_order(
            bswap64(msg->order_reference_number),
            bswap32(msg->price),
            bswap32(msg->shares),
            msg->buy_sell_indicator
        );
        ctx.add_orders++;
    }

    inline void handle_order_executed(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const OrderExecutedMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].execute_order(
            bswap64(msg->order_reference_number),
            bswap32(msg->executed_shares)
        );
        ctx.executed_orders++;
    }

    inline void handle_order_cancel(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const OrderCancelMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].cancel_order(
            bswap64(msg->order_reference_number),
            bswap32(msg->canceled_shares)
        );
        ctx.cancelled_orders++;
    }

    inline void handle_order_delete(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const OrderDeleteMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].delete_order(bswap64(msg->order_reference_number));
        ctx.cancelled_orders++;
    }

    inline void handle_order_replace(WorkerContext& ctx, const uint8_t* msg_ptr, uint16_t) noexcept {
        const auto* msg = reinterpret_cast<const OrderReplaceMessage*>(msg_ptr);
        uint16_t locate = bswap16(msg->header.stock_locate);
        order_books_[locate].replace_order(
            bswap64(msg->original_order_reference_number),
            bswap64(msg->new_order_reference_number),
            bswap32(msg->price),
            bswap32(msg->shares)
        );
        ctx.replaced_orders++;
    }

    inline void handle_stock_directory(WorkerContext&, const uint8_t* msg_ptr, uint16_t msg_len) noexcept {
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

    void worker_loop(WorkerContext& ctx) {
        pin_thread(ctx.cpu_id);

        QueueTask task;
        auto& queue = queues_[ctx.worker_id];

        while (true) {
            if (queue.pop(task)) {
                if (task.msg_ptr == nullptr) [[unlikely]] {
                    break; // Termination signal
                }

                uint8_t msg_type = task.msg_ptr[0];

                uint64_t start_cycles = __rdtsc();
                (this->*dispatch_table_[msg_type])(ctx, task.msg_ptr, task.msg_len);
                uint64_t end_cycles = __rdtsc();

                uint64_t duration_ns = static_cast<uint64_t>((end_cycles - start_cycles) * cycles_to_ns_multiplier_);

                if (duration_ns > ctx.max_latency_ns) {
                    ctx.max_latency_ns = duration_ns;
                }

                if (duration_ns < ctx.latency_buckets.size()) {
                    ctx.latency_buckets[duration_ns]++;
                } else {
                    ctx.latency_overflow++;
                }

                ctx.total_messages++;
            } else {
                _mm_pause();
            }
        }
    }

public:
    explicit ITCHParser(size_t max_locates = MAX_LOCATES)
        : locate_to_symbol_(max_locates) {

        calibrate_tsc();

        // 1. Initialize arenas and pools per consumer worker
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            worker_contexts_[i].worker_id = i;
            worker_contexts_[i].cpu_id = worker_cpus_[i];
            worker_contexts_[i].parser = this;
            worker_contexts_[i].arena_buffer = allocate_arena(WORKER_ARENA_SIZE);
            worker_contexts_[i].pool = std::make_unique<std::pmr::monotonic_buffer_resource>(
                worker_contexts_[i].arena_buffer,
                WORKER_ARENA_SIZE,
                std::pmr::null_memory_resource()
            );
            worker_contexts_[i].latency_buckets.fill(0);
        }

        // 2. Distribute order books to their thread-specific PMR memory pool
        order_books_.reserve(max_locates);
        for (size_t locate = 0; locate < max_locates; ++locate) {
            size_t assigned_worker = locate % NUM_WORKERS;
            order_books_.emplace_back(worker_contexts_[assigned_worker].pool.get());
        }

        // 3. Setup message dispatch table
        dispatch_table_.fill(&ITCHParser::handle_ignore);
        dispatch_table_['A'] = &ITCHParser::handle_add_order;
        dispatch_table_['F'] = &ITCHParser::handle_add_order_mpid;
        dispatch_table_['E'] = &ITCHParser::handle_order_executed;
        dispatch_table_['C'] = &ITCHParser::handle_order_executed;
        dispatch_table_['X'] = &ITCHParser::handle_order_cancel;
        dispatch_table_['D'] = &ITCHParser::handle_order_delete;
        dispatch_table_['U'] = &ITCHParser::handle_order_replace;
        dispatch_table_['R'] = &ITCHParser::handle_stock_directory;

        // 4. Spawn consumer threads on CPUs 5, 7, 9
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            worker_threads_[i] = std::thread(&ITCHParser::worker_loop, this, std::ref(worker_contexts_[i]));
        }
    }

    ~ITCHParser() {
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            if (worker_contexts_[i].arena_buffer && worker_contexts_[i].arena_buffer != MAP_FAILED) {
                munmap(worker_contexts_[i].arena_buffer, WORKER_ARENA_SIZE);
            }
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

        file_size_ = sb.st_size;
        auto* buffer = static_cast<const uint8_t*>(
            mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd, 0)
        );

        if (buffer == MAP_FAILED) {
            close(fd);
            return false;
        }

#ifdef POSIX_MADV_SEQUENTIAL
        madvise(const_cast<uint8_t*>(buffer), file_size_, MADV_SEQUENTIAL);
#endif

        // Pin Producer to CPU 3
        pin_thread(producer_cpu_);

        size_t offset = 0;

        while (offset + 2 < file_size_) {
            uint16_t msg_len = (static_cast<uint16_t>(buffer[offset]) << 8) | buffer[offset + 1];
            offset += 2;

            if (offset + msg_len > file_size_) break;

            const uint8_t* msg_ptr = buffer + offset;
            uint8_t msg_type = msg_ptr[0];

            if (msg_type == 'S') {
                // Broadcast system events across all worker queues
                for (size_t i = 0; i < NUM_WORKERS; ++i) {
                    while (!queues_[i].push({msg_ptr, msg_len})) {
                        _mm_pause();
                    }
                }
            } else {
                // Extract locate ID and route strictly to assigned worker
                uint16_t locate = (static_cast<uint16_t>(msg_ptr[1]) << 8) | msg_ptr[2];
                size_t target_worker = locate % NUM_WORKERS;

                while (!queues_[target_worker].push({msg_ptr, msg_len})) {
                    _mm_pause();
                }
            }

            offset += msg_len;
        }

        // Send termination sentinel (nullptr)
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            while (!queues_[i].push({nullptr, 0})) {
                _mm_pause();
            }
        }

        // Join consumer workers
        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            if (worker_threads_[i].joinable()) {
                worker_threads_[i].join();
            }
        }

        munmap(const_cast<uint8_t*>(buffer), file_size_);
        close(fd);
        return true;
    }

    void print_latency_metrics() const {
        uint64_t total_messages = total_messages_count();
        if (total_messages == 0) return;

        std::array<uint64_t, 100000> agg_buckets{};
        agg_buckets.fill(0);
        uint64_t agg_overflow = 0;
        uint64_t max_lat = 0;
        uint64_t sum_ns = 0;

        for (const auto& ctx : worker_contexts_) {
            agg_overflow += ctx.latency_overflow;
            if (ctx.max_latency_ns > max_lat) max_lat = ctx.max_latency_ns;

            for (size_t i = 0; i < ctx.latency_buckets.size(); ++i) {
                agg_buckets[i] += ctx.latency_buckets[i];
                sum_ns += (i * ctx.latency_buckets[i]);
            }
        }

        uint64_t avg_ns = sum_ns / total_messages;

        auto get_percentile = [&](double p) -> uint64_t {
            uint64_t threshold = static_cast<uint64_t>(total_messages * (p / 100.0));
            uint64_t current = 0;
            for (size_t i = 0; i < agg_buckets.size(); ++i) {
                current += agg_buckets[i];
                if (current >= threshold) return i;
            }
            return agg_buckets.size();
        };

        std::cout << "\n========================================\n";
        std::cout << "    TAIL LATENCY METRICS (Nanoseconds)    \n";
        std::cout << "========================================\n";
        std::cout << std::left << std::setw(15) << "Average"      << ": " << avg_ns << " ns\n";
        std::cout << std::left << std::setw(15) << "p50 (Median)" << ": " << get_percentile(50.0) << " ns\n";
        std::cout << std::left << std::setw(15) << "p99"          << ": " << get_percentile(99.0) << " ns\n";
        std::cout << std::left << std::setw(15) << "p99.9"        << ": " << get_percentile(99.9) << " ns\n";
        std::cout << std::left << std::setw(15) << "Maximum"      << ": " << max_lat << " ns\n";

        if (agg_overflow > 0) {
            std::cout << std::left << std::setw(15) << "Overflows"    << ": " << agg_overflow << " (>100us)\n";
        }
        std::cout << "========================================\n\n";
    }

    [[nodiscard]] uint64_t total_messages_count() const noexcept {
        uint64_t total = 0;
        for (const auto& ctx : worker_contexts_) total += ctx.total_messages;
        return total;
    }

    [[nodiscard]] uint64_t add_orders_count() const noexcept {
        uint64_t total = 0;
        for (const auto& ctx : worker_contexts_) total += ctx.add_orders;
        return total;
    }

    [[nodiscard]] uint64_t executed_orders_count() const noexcept {
        uint64_t total = 0;
        for (const auto& ctx : worker_contexts_) total += ctx.executed_orders;
        return total;
    }

    [[nodiscard]] uint64_t cancelled_orders_count() const noexcept {
        uint64_t total = 0;
        for (const auto& ctx : worker_contexts_) total += ctx.cancelled_orders;
        return total;
    }

    [[nodiscard]] uint64_t replaced_orders_count() const noexcept {
        uint64_t total = 0;
        for (const auto& ctx : worker_contexts_) total += ctx.replaced_orders;
        return total;
    }

    [[nodiscard]] size_t file_size() const noexcept { return file_size_; }
    const OrderBook& get_order_book(uint16_t locate_id) const noexcept { return order_books_[locate_id]; }
};

} // namespace itch