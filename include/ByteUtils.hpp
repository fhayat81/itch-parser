#pragma once
#include <cstdint>
#include <cstring>

namespace itch {

// Modern GCC/Clang intrinsic byte swapping for x86_64 architecture
inline uint16_t bswap16(uint16_t val) noexcept {
    return __builtin_bswap16(val);
}

inline uint32_t bswap32(uint32_t val) noexcept {
    return __builtin_bswap32(val);
}

inline uint64_t bswap64(uint64_t val) noexcept {
    return __builtin_bswap64(val);
}

// Decodes NASDAQ ITCH 48-bit Big-Endian timestamps into uint64_t
inline uint64_t bswap48(const uint8_t* ptr) noexcept {
    uint64_t val;
    std::memcpy(&val, ptr, sizeof(uint64_t));
    return __builtin_bswap64(val) >> 16;
}

} // namespace itch