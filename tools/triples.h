#pragma once
// Triples of entity links, 12 bytes as 3 little-endian uint32: property, item
// with rank, target with type. The item is always a Q (the claims of
// properties and lexemes go to their own JSONL). Both other words hold a
// 30-bit number and 2 bits in the low bits, so that sorting by the word sorts
// by the number:
//   item word    Q << 2 | rank   (rank: 0 deprecated, 1 normal, 2 preferred)
//   target word  N << 2 | type   (type: 0 item Q, 1 property P, 2 lexeme L)
// 30 bits reach 1073741823 (today Q is about 140 million).
// (31, 56782 << 2 | normal, 34770 << 2 | item): Q56782 instance of (P31) Q34770.
//
// Sorted for the reverse lookup ("all items with P106 = Q36180, writer") by
// (bit-reversed property, target word, item word). Bit reversal puts
// property % 256 in the top 8 bits, so the triples of property % 256 = k are
// one contiguous run of a sorted file: the indexes are split into 256 files by
// property modulo, whatever the number of properties.
#include <bit>
#include <chrono>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>

// Rank of a statement; "truthy" values: the preferred ones of a property if
// there are any, else the normal ones; deprecated ones are wrong values kept
enum class Rank : uint8_t { deprecated = 0, normal = 1, preferred = 2 };

// Type of the target entity
enum class TargetType : uint8_t { item = 0, property = 1, lexeme = 2 };

struct Triple {
    uint32_t property;
    uint32_t itemRank;   // item << 2 | rank
    uint32_t targetType; // target << 2 | type

    static constexpr uint32_t maxNumber = (1u << 30) - 1;
    static Triple make(uint32_t property, uint32_t item, Rank rank, uint32_t target, TargetType type) {
        if (item > maxNumber || target > maxNumber)
            throw std::runtime_error(std::format("Q{} or target {} does not fit in 30 bits", item, target));
        return {property, item << 2 | static_cast<uint32_t>(rank), target << 2 | static_cast<uint32_t>(type)};
    }
    uint32_t item() const { return itemRank >> 2; }
    Rank rank() const { return static_cast<Rank>(itemRank & 3); }
    uint32_t target() const { return targetType >> 2; }
    TargetType type() const { return static_cast<TargetType>(targetType & 3); }
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

// Order of sorted triples: bit-reversed property, target word, item word
inline bool operator<(const Triple &a, const Triple &b) {
    const auto ra = reverseBits(a.property), rb = reverseBits(b.property);
    if (ra != rb)
        return ra < rb;
    if (a.targetType != b.targetType)
        return a.targetType < b.targetType;
    return a.itemRank < b.itemRank;
}

inline bool operator==(const Triple &a, const Triple &b) {
    return a.property == b.property && a.itemRank == b.itemRank && a.targetType == b.targetType;
}

// The order of an index: by property (triple-index: "all items with P106 =
// Q36180") or by target (target-index: "all that points to Q36180", by
// property). By target the key is (bit-reversed target number, target type,
// property, item word), the file the target number % 256.
enum class Order { byProperty, byTarget };

// The number whose low 8 bits choose the file, and which is bit-reversed first
inline uint32_t primaryOf(Order order, const Triple &t) { return order == Order::byProperty ? t.property : t.target(); }

inline bool less(Order order, const Triple &a, const Triple &b) {
    if (order == Order::byProperty)
        return a < b;
    const auto ra = reverseBits(a.target()), rb = reverseBits(b.target());
    if (ra != rb)
        return ra < rb;
    if (a.type() != b.type())
        return a.type() < b.type();
    if (a.property != b.property)
        return a.property < b.property;
    return a.itemRank < b.itemRank;
}

// --by property | --by target of wdsort and wdmerge
inline Order orderOf(const std::string &name) {
    if (name == "property")
        return Order::byProperty;
    if (name == "target")
        return Order::byTarget;
    throw std::runtime_error("--by property or --by target, not " + name);
}

inline std::string duration(double seconds) {
    const auto s = static_cast<long>(seconds);
    return std::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60);
}

inline double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
