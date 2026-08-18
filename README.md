# High-Performance Market Data Parser

A production-grade, low-latency C++ market data engine for NASDAQ ITCH 5.0 feeds. It uses a core-pinned producer-consumer pipeline to parse raw exchange messages, route them by stock locate, and maintain a consistent per-symbol in-memory book state with high throughput and minimal allocations.

## Current Benchmarks

**Hardware Environment**: AMD Ryzen™ 5 5625U (Linux, core-pinned multithreaded run: producer on core 3, consumers on cores 5/7/9)

| Dataset | Total Messages | Processing Time | Message Throughput | Data Throughput | Avg Latency (Wall) | Average (Tail Metrics) | p50 | p99 | p99.9 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `01302019.NASDAQ_ITCH50.bin` | 368,366,646 | 34.32 s | 10.73M msgs/sec | 312.50 MB/sec | 93.17 ns | 221 ns | 130 ns | 1,312 ns | 3,125 ns |
| `01302020.NASDAQ_ITCH50.bin` | 423,285,721 | 42.03 s | 10.07M msgs/sec | 293.88 MB/sec | 99.30 ns | 234 ns | 140 ns | 1,342 ns | 3,236 ns |
| `07302019.NASDAQ_ITCH50.bin` | 282,229,696 | 26.43 s | 10.68M msgs/sec | 312.58 MB/sec | 93.64 ns | 228 ns | 130 ns | 1,352 ns | 3,185 ns |
| `08302019.NASDAQ_ITCH50.bin` | 310,317,369 | 28.11 s | 11.04M msgs/sec | 322.31 MB/sec | 90.60 ns | 229 ns | 140 ns | 1,342 ns | 3,195 ns |
| `10302019.NASDAQ_ITCH50.bin` | 293,989,091 | 27.90 s | 10.54M msgs/sec | 308.82 MB/sec | 94.91 ns | 230 ns | 140 ns | 1,322 ns | 3,185 ns |
| `12302019.NASDAQ_ITCH50.bin` | 268,744,792 | 25.65 s | 10.48M msgs/sec | 306.75 MB/sec | 95.46 ns | 230 ns | 140 ns | 1,322 ns | 3,165 ns |

- **`Avg Latency (Wall)`**: End-to-end wall-clock average (`processing_time / total_messages`) reported in the main engine summary.
- **`Average (Tail Metrics)`**: Mean per-message handler latency measured inside consumer workers and reported under `TAIL LATENCY METRICS`.


## Why This Parser Is Fast

### 1. High-Performance Architecture
- Core-pinned producer-consumer architecture with one producer and three dedicated consumer workers
- Lock-free, cache-line-aligned SPSC queues for low-overhead handoff from parser thread to workers
- Zero-copy parsing using `mmap` and raw pointer traversal
- Per-worker memory preallocation via large `std::pmr::monotonic_buffer_resource` arenas to reduce allocator overhead
- Locate-based routing (`stock_locate % worker_count`) that preserves per-symbol locality and avoids lock contention
- Branch-light message dispatch using a prebuilt function-pointer dispatch table

### 2. NASDAQ ITCH 5.0 Support
- Parses trading lifecycle messages including:
  - System Event (`S`)
  - Stock Directory (`R`)
  - Add Order (`A` / `F`)
  - Executed (`E` / `C`)
  - Cancel/Delete (`X` / `D`)
  - Replace (`U`)
- Maintains best bid/ask state and active order tracking per symbol in worker-local order books
- Broadcasts system-event messages across all workers while routing symbol-linked messages deterministically to a single consumer

### 3. Designed for HFT Workloads
- Focused on throughput and low tail latency
- Optimized for large historical feed replay and high message volumes
- Built for direct, deterministic parsing of binary market data without excessive abstraction overhead
- Reports both engine-level wall-clock performance and per-message tail-latency distribution

## Project Structure

- `src/` — parser entry point and application logic
- `include/` — core parser, order book, message struct, and utility headers
- `CMakeLists.txt` — build configuration and optimization flags
- `build/` — generated build artifacts

## Requirements

- Linux (uses POSIX APIs such as `mmap`, `fstat`, and `madvise`)
- CMake 3.20+
- C++20 compatible compiler (GCC or Clang)

## Build and Run

### Quick Build

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

### Run the Parser

```bash
./ITCH-PARSER path/to/01302020.NASDAQ_ITCH50.bin
```

Example:

```bash
./ITCH-PARSER ../01302020.NASDAQ_ITCH50.bin
```

### CPU Isolation for Benchmark Runs (`isolcpus`)

For stable low-latency benchmarking, isolate the producer/consumer cores used by the engine (`3,5,7,9`) from the general scheduler.

1. Add kernel boot parameters (GRUB):

```bash
sudo nano /etc/default/grub
```

Set or extend `GRUB_CMDLINE_LINUX_DEFAULT` to include:

```bash
isolcpus=3,5,7,9 nohz_full=3,5,7,9 rcu_nocbs=3,5,7,9
```

2. Apply and reboot:

```bash
sudo update-grub
sudo reboot
```

3. Run the parser:

```bash
./ITCH-PARSER ../01302020.NASDAQ_ITCH50.bin
```

4. (Optional) Verify isolation after reboot:

```bash
cat /proc/cmdline
```

The executable prints a main engine summary (throughput, data rate, and wall-clock average latency) followed by a dedicated tail-latency report for per-message processing (`Average`, `p50`, `p99`, `p99.9`, `Maximum`, and `Overflows`).

## Sample Data

### Real NASDAQ ITCH Data

Historical full-day NASDAQ ITCH 5.0 files can be downloaded from the official NASDAQ source:

- Official NASDAQ ITCH archive: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

This creates a compact sample file under the project data directory that can be used with the parser.

## Example Performance Output

```text
========================================
  NASDAQ ITCH 5.0 Multithreaded Engine 
  Producer Core: 3 | Consumer Cores: 5, 7, 9
========================================
Total Messages Scanned: 368366646
Add Orders ('A'/'F'):   164696353
Executions ('E'/'C'):   8255881
Cancels/Deletes ('X/D'):162943235
Replaces ('U'):         27222746
----------------------------------------
Processing Time:        34319.17 ms
Message Throughput:     10.73 Million msgs/sec
Data Throughput:        312.50 MB/sec
Avg Latency (Wall):     93.17 ns / msg
========================================

========================================
   TAIL LATENCY METRICS (Nanoseconds)    
========================================
Average        : 221 ns
p50 (Median)   : 130 ns
p99            : 1312 ns
p99.9          : 3125 ns
Maximum        : 4889669 ns
Overflows      : 1261 (>100us)
========================================
```

## Notes

- The parser is intentionally optimized for feed replay and high-throughput analysis rather than general-purpose market logic.
- It assumes well-formed ITCH binary payloads and focuses on minimal overhead and deterministic execution.
- Benchmark output contains two latency views: wall-clock engine average and in-handler tail-latency statistics. Both are useful and should be interpreted together.
- The build defaults include aggressive optimization flags suitable for performance testing on supported x86_64 Linux systems.

## Contributing

Pull requests and improvements are welcome, especially around:
- parser correctness and validation
- additional message handling
- lower-latency order book optimization
- benchmarking and profiling tooling
- portability and CI coverage
