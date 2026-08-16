# High-Performance Market Data Parser

A production-grade, low-latency C++ market data engine for NASDAQ ITCH 5.0 feeds. It parses raw exchange messages, normalizes them into a consistent in-memory book state, and tracks per-symbol order flow with high throughput and minimal allocations.

## Current Benchmarks

**Hardware Environment**: AMD Ryzen™ 5 5625U (Linux, single-core taskset execution)

| Dataset | Total Messages | Processing Time | Message Throughput | Data Throughput | Average | p50 | p99 | p99.9 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `01302019.NASDAQ_ITCH50.bin` | 368,366,634 | 84.69 s | 4.37M msgs/sec | 127.12 MB/sec | 208 ns | 120 ns | 1,232 ns | 3,165 ns |
| `01302020.NASDAQ_ITCH50.bin` | 423,285,709 | 112.96 s | 3.76M msgs/sec | 109.70 MB/sec | 245 ns | 150 ns | 1,302 ns | 3,386 ns |
| `03272019.NASDAQ_ITCH50.bin` | 422,264,305 | 110.92 s | 3.82M msgs/sec | 111.01 MB/sec | 240 ns | 140 ns | 1,352 ns | 3,436 ns |
| `07302019.NASDAQ_ITCH50.bin` | 282,229,684 | 69.67 s | 4.07M msgs/sec | 119.00 MB/sec | 207 ns | 130 ns | 1,132 ns | 3,256 ns |


## Why This Parser Is Fast

### 1. High-Performance Architecture
- Zero-copy parsing using `mmap` and raw pointer traversal
- Per-symbol book handling with efficient in-memory state representation
- Memory preallocation via `std::pmr` arena to reduce heap churn
- Branch-light parsing logic for high-throughput message processing

### 2. NASDAQ ITCH 5.0 Support
- Parses trading lifecycle messages including:
  - System Event (`S`)
  - Stock Directory (`R`)
  - Add Order (`A` / `F`)
  - Executed (`E` / `C`)
  - Cancel/Delete (`X` / `D`)
  - Replace (`U`)
- Maintains best bid/ask state and active order tracking per symbol
- Prints market lifecycle events and watchlist snapshots near close

### 3. Designed for HFT Workloads
- Focused on throughput and low tail latency
- Optimized for large historical feed replay and high message volumes
- Built for direct, deterministic parsing of binary market data without excessive abstraction overhead

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

The executable prints a summary of the feed, including total messages scanned and counts of add, execution, cancel, and replace events, followed by market-state output for the configured watchlist when the session reaches close.

## Sample Data

### Real NASDAQ ITCH Data

Historical full-day NASDAQ ITCH 5.0 files can be downloaded from the official NASDAQ source:

- Official NASDAQ ITCH archive: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/

This creates a compact sample file under the project data directory that can be used with the parser.

## Example Performance Output

```text
========================================
   NASDAQ ITCH 5.0 Feed Handler Engine   
========================================
Total Messages Scanned: 368366634
Add Orders ('A'/'F'):   164696353
Executions ('E'/'C'):   8255881
Cancels/Deletes ('X/D'):162943235
Replaces ('U'):        27222746
----------------------------------------
Processing Time:        84369.41 ms
Message Throughput:     4.37 Million msgs/sec
Data Throughput:        127.12 MB/sec
Avg Latency (Wall):     229.04 ns / msg
========================================
```

## Notes

- The parser is intentionally optimized for feed replay and high-throughput analysis rather than general-purpose market logic.
- It assumes well-formed ITCH binary payloads and focuses on minimal overhead and deterministic execution.
- The build defaults include aggressive optimization flags suitable for performance testing on supported x86_64 Linux systems.

## Contributing

Pull requests and improvements are welcome, especially around:
- parser correctness and validation
- additional message handling
- lower-latency order book optimization
- benchmarking and profiling tooling
- portability and CI coverage
