# ITCH-Parser

A high-performance NASDAQ ITCH 5.0 feed handler and order-book engine written in modern C++ (C++20).

## Overview

This project parses NASDAQ ITCH 5.0 binary feeds (moldUDP-style framing) and maintains per-symbol order books in memory. It is designed for high throughput and low-latency analysis using memory-mapped files, a preallocated 4GB polymorphic memory resource, and optimized build flags.

Key features:
- Parses common ITCH message types: System Event (`S`), Stock Directory (`R`), Add Order (`A`/`F`), Executed (`E`/`C`), Cancel/Delete (`X`/`D`), Replace (`U`).
- Maintains per-locate order books with best bid/ask and active order counts.
- Prints market lifecycle events and a watchlist snapshot at market close.
- Uses `mmap` for zero-copy file access and `std::pmr` with a 4GB arena for allocation predictability.

## Requirements
- Linux (POSIX APIs used: `mmap`, `madvise`, `fstat`)
- CMake >= 3.20
- A C++20-capable compiler (GCC/Clang)

## Build

From the repository root:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

Notes:
- The default `CMakeLists.txt` adds aggressive compiler flags (`-O3 -march=znver3 -flto`). If you are not building on an AMD Zen 3 system, remove or adjust `-march=znver3` in `CMakeLists.txt`.

## Usage

Run the produced executable with a path to an ITCH binary file (moldUDP framing expected):

```bash
./ITCH-PARSER path/to/01302020.NASDAQ_ITCH50.bin
```

Example (from `build/`):

```bash
./ITCH-PARSER ../01302020.NASDAQ_ITCH50.bin
```

Output includes a summary with total messages scanned and counts of add/execute/cancel/replace events. The program will also print system events (`Q`, `M`, `E`) and an order-book snapshot for a small watchlist (`AAPL`, `MSFT`, `NVDA`, `AMZN`, `GOOGL`, `TSLA`, `QQQ`, `SPY`) at market close.

## Project Structure

- `src/` — main application (`main.cpp`) and entry point
- `include/` — headers for the parser and helpers (`ITCHParser.hpp`, `ITCHStructs.hpp`, `OrderBook.hpp`, `ByteUtils.hpp`, `DenseMap.hpp`)
- `CMakeLists.txt` — build configuration

## Design Notes

- Memory: the parser preallocates a 4GB arena and constructs `std::pmr`-backed containers to avoid repeated heap allocations.
- IO: input files are `mmap`-ed and optionally `madvise`d with `MADV_SEQUENTIAL` for streaming access.
- Performance: optimized compile flags are applied in `CMakeLists.txt` for high throughput workloads.

## Limitations & Safety

- The code assumes well-formed ITCH binary framing and does minimal validation of malformed messages.
- The parser reserves a large (4GB) arena — ensure your system has sufficient memory before running on large input sets.

## Contributing

Feel free to open issues or pull requests with improvements (platform portability, additional message types, CSV/JSON exporters, tests, etc.).