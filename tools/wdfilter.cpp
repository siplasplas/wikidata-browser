// wdfilter: leaves out of the triple index the items of a class that are in no
// Wikipedia (by default scholarly articles, Q13442814: about two thirds of the
// JSON of all items).
//
// wdfilter [-j 20] [--class 13442814] [-i triple-index] [-o triple-index-slim] [--popularity popularity] [--removed removed]
//
// Items left out: those with P31 = the class (rank not deprecated) and 0
// Wikipedias in popularity (wddense). They are found without reading all
// triples: in triple-index/031 (P31 % 256) the triples (P31, class) are one
// contiguous run. Writes the mask of the items left out (removed: 1 bit per Q,
// bit q % 8 of byte q / 8; for the conversion of the dump too) and the 256
// index files without the triples of those items. Triples pointing to them
// from other items stay: the names show them.
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "triples.h"

namespace fs = std::filesystem;

namespace {

std::vector<char> readFile(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    return {std::istreambuf_iterator<char>(in), {}};
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
    uint32_t cls = 13442814;
    fs::path input = "triple-index", output = "triple-index-slim", popularityFile = "popularity", removedFile = "removed";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "--class" && i + 1 < argc)
            cls = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "-i" && i + 1 < argc)
            input = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else if (arg == "--popularity" && i + 1 < argc)
            popularityFile = argv[++i];
        else if (arg == "--removed" && i + 1 < argc)
            removedFile = argv[++i];
        else {
            std::cerr << "usage: wdfilter [-j 20] [--class 13442814] [-i triple-index] [-o triple-index-slim] "
                         "[--popularity popularity] [--removed removed]\n";
            return 1;
        }
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        // Wikipedias per item (uint16)
        const auto popularityBytes = readFile(popularityFile);
        const auto *popularity = reinterpret_cast<const uint16_t *>(popularityBytes.data());
        const size_t popularityCount = popularityBytes.size() / 2;
        auto wikipedias = [&](uint32_t q) { return q < popularityCount ? popularity[q] : 0; };

        // The items of the class: the run (P31, class) of the file of P31
        const auto p31 = readTriples(input / std::format("{:03}", bucketOf(31)));
        const Triple key{31, 0, cls << 2 | static_cast<uint32_t>(TargetType::item)};
        const auto from = std::lower_bound(p31.begin(), p31.end(), key, [](const Triple &a, const Triple &b) { return a < b; });
        std::vector<uint8_t> removed;
        uint64_t ofClass = 0, left = 0;
        for (auto it = from; it != p31.end() && it->property == 31 && it->targetType == key.targetType; ++it) {
            if (it->rank() == Rank::deprecated)
                continue;
            ++ofClass;
            const auto q = it->item();
            if (wikipedias(q))
                continue;
            if (removed.size() <= q / 8)
                removed.resize(q / 8 + 1, 0);
            if (!(removed[q / 8] & (1 << (q % 8)))) {
                removed[q / 8] |= static_cast<uint8_t>(1 << (q % 8));
                ++left;
            }
        }
        {
            std::ofstream out(removedFile, std::ios::binary);
            out.write(reinterpret_cast<const char *>(removed.data()), static_cast<std::streamsize>(removed.size()));
            if (!out)
                throw std::runtime_error("cannot write " + removedFile.string());
        }
        std::cout << std::format("Q{}: {} statements P31 (not deprecated), {} items in no Wikipedia left out -> {} ({})",
                                 cls, ofClass, left, removedFile.string(), duration(secondsSince(started)))
                  << std::endl;
        auto isRemoved = [&](uint32_t q) { return q / 8 < removed.size() && (removed[q / 8] >> (q % 8) & 1); };

        // The 256 index files without the triples of those items
        fs::create_directories(output);
        std::atomic<uint32_t> next{0};
        std::atomic<uint64_t> kept{0}, dropped{0};
        std::vector<std::thread> pool;
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                for (uint32_t bucket; (bucket = next++) < 256;) {
                    const auto name = std::format("{:03}", bucket);
                    if (fs::exists(output / name))
                        continue;
                    auto triples = readTriples(input / name);
                    const auto before = triples.size();
                    std::erase_if(triples, [&](const Triple &t) { return isRemoved(t.item()); });
                    std::ofstream out(output / (name + ".part"), std::ios::binary);
                    out.write(reinterpret_cast<const char *>(triples.data()),
                              static_cast<std::streamsize>(triples.size() * sizeof(Triple)));
                    if (!out)
                        throw std::runtime_error("cannot write " + (output / name).string());
                    out.close();
                    fs::rename(output / (name + ".part"), output / name);
                    kept += triples.size();
                    dropped += before - triples.size();
                }
            });
        for (auto &thread : pool)
            thread.join();
        std::cout << std::format("{}: {} triples kept, {} left out ({:.1f} GB -> {:.1f} GB) in {}", output.string(),
                                 kept.load(), dropped.load(), (kept + dropped) * 12 / 1e9, kept * 12 / 1e9,
                                 duration(secondsSince(started)))
                  << std::endl;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
