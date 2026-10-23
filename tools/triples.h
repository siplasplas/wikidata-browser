#pragma once
// Triples of entity links: (property, item, target) as 3 x little-endian uint32,
// e.g. (31, 56782, 34770): Q56782 instance of (P31) Q34770.
//
// Sorted for the reverse lookup ("all items with P106 = Q36180, writer") by
// (bit-reversed property, target, item). Bit reversal puts property % 256 in
// the top 8 bits, so the triples of property % 256 = k are one contiguous run
// of a sorted file: the indexes are split into 256 files by property modulo,
// whatever the number of properties.
#include <bit>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

struct Triple {
    uint32_t property;
    uint32_t item;   // the entity with the claim
    uint32_t target; // the entity it points to
};
static_assert(sizeof(Triple) == 12);

// The 32 bits in reverse order: bit 0 becomes bit 31
constexpr uint32_t reverseBits(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    return std::byteswap(x);
}
static_assert(reverseBits(1) == 0x80000000u && reverseBits(0x80000000u) == 1 && reverseBits(0x12345678u) == 0x1E6A2C48u);

// Index file of a triple: property % 256
constexpr uint32_t bucketOf(uint32_t property) { return property & 0xFF; }

// Order of sorted triples: bit-reversed property, target, item
inline bool operator<(const Triple &a, const Triple &b) {
    const auto ra = reverseBits(a.property), rb = reverseBits(b.property);
    if (ra != rb)
        return ra < rb;
    if (a.target != b.target)
        return a.target < b.target;
    return a.item < b.item;
}

inline bool operator==(const Triple &a, const Triple &b) {
    return a.property == b.property && a.item == b.item && a.target == b.target;
}

inline std::string duration(double seconds) {
    const auto s = static_cast<long>(seconds);
    return std::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60);
}

inline double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
