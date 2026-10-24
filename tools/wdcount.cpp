// wdcount: what points to each entity, counted by property, from the triple
// index (wdmerge, wdfilter).
//
// wdcount [-j 20] [-i triple-index-slim] [-o target-counts]
//
// In the triple index the items of one (property, target) are one contiguous
// run; their number (statements not deprecated) is a record of 12 bytes, 3
// little-endian uint32: target word (number << 2 | type, as in triples.h),
// property, count. Records go to 256 files by target number % 256, sorted by
// (bit-reversed target number, type, property): all that points to Q34770 is
// one contiguous run of file 34770 % 256, e.g. P31 7812, P279 312. The items
// of a property are then the run (P, Q34770) of the triple index.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "triples.h"

namespace fs = std::filesystem;

namespace {

struct Count {
    uint32_t target; // number << 2 | type
    uint32_t property;
    uint32_t count;
};
static_assert(sizeof(Count) == 12);

// Order: bit-reversed target number, type, property
bool before(const Count &a, const Count &b) {
    const auto ra = reverseBits(a.target >> 2), rb = reverseBits(b.target >> 2);
    if (ra != rb)
        return ra < rb;
    if ((a.target & 3) != (b.target & 3))
        return (a.target & 3) < (b.target & 3);
    return a.property < b.property;
}

std::vector<Triple> readTriples(const fs::path &file) {
    const auto size = fs::file_size(file);
    if (size % sizeof(Triple))
        throw std::runtime_error(file.string() + ": size is no multiple of 12");
    std::vector<Triple> triples(size / sizeof(Triple));
    std::ifstream in(file, std::ios::binary);
    in.read(reinterpret_cast<char *>(triples.data()), static_cast<std::streamsize>(size));
    if (!in)
        throw std::runtime_error("cannot read " + file.string());
    return triples;
}

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20;
    fs::path input = "triple-index-slim", output = "target-counts";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "-i" && i + 1 < argc)
            input = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else {
            std::cerr << "usage: wdcount [-j 20] [-i triple-index-slim] [-o target-counts]\n";
            return 1;
        }
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        // Counts of every file of the triple index, into the buckets of their targets
        std::vector<std::vector<Count>> buckets(256);
        std::vector<std::mutex> locks(256);
        std::atomic<uint32_t> next{0};
        std::atomic<uint64_t> triples{0}, pairs{0};
        std::vector<std::thread> pool;
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                for (uint32_t file; (file = next++) < 256;) {
                    const auto data = readTriples(input / std::format("{:03}", file));
                    triples += data.size();
                    std::vector<std::vector<Count>> mine(256);
                    for (size_t i = 0; i < data.size();) {
                        size_t j = i, count = 0;
                        for (; j < data.size() && data[j].property == data[i].property && data[j].targetType == data[i].targetType; ++j)
                            count += data[j].rank() != Rank::deprecated;
                        if (count)
                            mine[data[i].target() & 0xFF].push_back(
                                    {data[i].targetType, data[i].property, static_cast<uint32_t>(count)});
                        i = j;
                    }
                    for (size_t b = 0; b < 256; ++b) {
                        pairs += mine[b].size();
                        std::lock_guard lock(locks[b]);
                        buckets[b].insert(buckets[b].end(), mine[b].begin(), mine[b].end());
                    }
                }
            });
        for (auto &thread : pool)
            thread.join();
        std::cout << std::format("{} triples, {} (target, property) pairs ({})", triples.load(), pairs.load(),
                                 duration(secondsSince(started)))
                  << std::endl;

        fs::create_directories(output);
        next = 0;
        pool.clear();
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                for (uint32_t b; (b = next++) < 256;) {
                    auto &records = buckets[b];
                    std::ranges::sort(records, before);
                    const auto name = output / std::format("{:03}", b);
                    std::ofstream out(fs::path(name.string() + ".part"), std::ios::binary);
                    out.write(reinterpret_cast<const char *>(records.data()),
                              static_cast<std::streamsize>(records.size() * sizeof(Count)));
                    if (!out)
                        throw std::runtime_error("cannot write " + name.string());
                    out.close();
                    fs::rename(fs::path(name.string() + ".part"), name);
                    std::vector<Count>().swap(records);
                }
            });
        for (auto &thread : pool)
            thread.join();
        std::cout << std::format("{}: 256 files, {:.2f} GB, in {}", output.string(), pairs * 12 / 1e9,
                                 duration(secondsSince(started)))
                  << std::endl;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
