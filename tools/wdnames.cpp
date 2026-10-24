// wdnames: name indexes of items from the block indexes of wdindex.
//
// wdnames [-j 20] [-i index] [-o .] <date>.lst
//
// The name of an item is its English label, else the first one (as in
// index/NNN). Writes in the output directory:
//   names        the names in the order of Q numbers, UTF-8, each ended by \0
//                and padded with a second \0 to an even length; it starts with
//                an empty name (\0\0) at offset 0
//   names-by-q   dense: uint32 for Q0..Qmax, the offset of its name in
//                2-byte units (so 4 bytes reach 8 GiB); 0 = no name (absent
//                item or one without labels)
//   q-by-name    uint32 Q numbers of the items with a name, sorted by the
//                search key of the name (fold.h: case and diacritics ignored),
//                then by Q: binary search finds a name and the names starting
//                with a text
// All little-endian.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fold.h"
#include "triples.h" // duration, secondsSince

namespace fs = std::filesystem;

namespace {

template <class T> T number(std::string_view s, const std::string &where) {
    T value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (error != std::errc() || end != s.data() + s.size())
        throw std::runtime_error(std::format("{}: not a number: '{}'", where, s));
    return value;
}

// Strings in one buffer, addressed by their offsets
struct Strings {
    std::string data;
    std::vector<uint64_t> offsets; // by index; data[offsets[i]..] up to \0
    std::string_view at(size_t i) const {
        const char *p = data.data() + offsets[i];
        return {p, std::char_traits<char>::length(p)};
    }
};

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20;
    fs::path indexDir = "index", output = ".";
    std::string list;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "-i" && i + 1 < argc)
            indexDir = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else
            list = arg;
    }
    if (list.empty()) {
        std::cerr << "usage: wdnames [-j 20] [-i index] [-o .] <date>.lst\n";
        return 1;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        std::vector<uint16_t> parts;
        {
            std::ifstream in(list);
            if (!in)
                throw std::runtime_error("cannot open " + list);
            for (std::string line; std::getline(in, line);)
                if (!line.empty() && !line.starts_with('#'))
                    parts.push_back(number<uint16_t>(std::string_view(line).substr(0, line.find(':')), list));
        }
        // The names of all items, by Q
        std::vector<std::string> byQ;
        uint64_t named = 0, bytes = 0;
        for (const auto part : parts) {
            const auto file = indexDir / std::format("{:03}", part);
            std::ifstream in(file);
            if (!in)
                throw std::runtime_error("cannot open " + file.string());
            bool complete = false;
            for (std::string line; std::getline(in, line);) {
                if (line.starts_with("EOF=")) {
                    complete = true;
                    continue;
                }
                if (!line.starts_with(":"))
                    continue;
                // :start:Q123:name
                const auto second = line.find(':', 1), third = line.find(':', second + 1);
                if (second == std::string::npos || third == std::string::npos || line[second + 1] != 'Q')
                    continue;
                const auto q = number<uint32_t>(std::string_view(line).substr(second + 2, third - second - 2), file.string());
                if (byQ.size() <= q)
                    byQ.resize(std::max<size_t>(q + 1, byQ.size() * 5 / 4));
                byQ[q] = line.substr(third + 1);
                if (!byQ[q].empty()) {
                    ++named;
                    bytes += byQ[q].size() + 1;
                }
            }
            if (!complete)
                throw std::runtime_error(file.string() + ": no EOF line (wdindex not finished?)");
        }
        while (!byQ.empty() && byQ.back().empty())
            byQ.pop_back();
        std::cout << std::format("{} items with a name, Q{} max, {:.2f} GB of names ({})", named, byQ.size() - 1,
                                 bytes / 1e9, duration(secondsSince(started)))
                  << std::endl;

        // names and names-by-q
        fs::create_directories(output);
        {
            std::ofstream names(output / "names", std::ios::binary);
            std::vector<uint32_t> offsets(byQ.size(), 0);
            names.write("\0\0", 2); // offset 0: no name
            uint64_t at = 2;
            for (size_t q = 0; q < byQ.size(); ++q) {
                if (byQ[q].empty())
                    continue;
                if (at / 2 > UINT32_MAX)
                    throw std::runtime_error("names over 8 GiB: offsets do not fit in 4 bytes");
                offsets[q] = static_cast<uint32_t>(at / 2);
                names.write(byQ[q].data(), static_cast<std::streamsize>(byQ[q].size()));
                const size_t end = byQ[q].size() % 2 ? 1 : 2; // \0, and a second one to an even length
                names.write("\0\0", static_cast<std::streamsize>(end));
                at += byQ[q].size() + end;
            }
            std::ofstream byQout(output / "names-by-q", std::ios::binary);
            byQout.write(reinterpret_cast<const char *>(offsets.data()),
                         static_cast<std::streamsize>(offsets.size() * sizeof(uint32_t)));
            if (!names || !byQout)
                throw std::runtime_error("cannot write names");
            std::cout << std::format("names: {:.2f} GB, names-by-q: {:.2f} GB ({})", at / 1e9,
                                     offsets.size() * 4 / 1e9, duration(secondsSince(started)))
                      << std::endl;
        }

        // Search keys, in threads; then q-by-name, sorted in chunks and merged
        std::vector<uint32_t> qs;
        qs.reserve(named);
        for (size_t q = 0; q < byQ.size(); ++q)
            if (!byQ[q].empty())
                qs.push_back(static_cast<uint32_t>(q));
        std::vector<std::string> keys(byQ.size());
        {
            std::atomic<size_t> next{0};
            std::vector<std::thread> pool;
            for (size_t t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    for (size_t chunk; (chunk = next.fetch_add(100000)) < qs.size();)
                        for (size_t i = chunk; i < std::min(qs.size(), chunk + 100000); ++i) {
                            const auto q = qs[i];
                            keys[q] = foldKey(byQ[q]);
                            if (keys[q] == byQ[q])
                                keys[q].clear(); // the same as the name: kept once
                        }
                });
            for (auto &thread : pool)
                thread.join();
        }
        auto keyOf = [&](uint32_t q) -> const std::string & { return keys[q].empty() ? byQ[q] : keys[q]; };
        auto less = [&](uint32_t a, uint32_t b) {
            const auto c = keyOf(a).compare(keyOf(b));
            return c != 0 ? c < 0 : a < b;
        };
        std::cout << "search keys made (" << duration(secondsSince(started)) << ")" << std::endl;
        {
            const size_t chunks = threads;
            std::vector<std::pair<size_t, size_t>> ranges;
            for (size_t c = 0; c < chunks; ++c)
                ranges.emplace_back(qs.size() * c / chunks, qs.size() * (c + 1) / chunks);
            std::vector<std::thread> pool;
            for (const auto &[from, to] : ranges)
                pool.emplace_back([&, from, to] { std::sort(qs.begin() + from, qs.begin() + to, less); });
            for (auto &thread : pool)
                thread.join();
            std::cout << "chunks sorted (" << duration(secondsSince(started)) << ")" << std::endl;
            // Merge of the sorted chunks
            auto later = [&](size_t a, size_t b) { return less(qs[ranges[b].first], qs[ranges[a].first]); };
            std::priority_queue<size_t, std::vector<size_t>, decltype(later)> heap(later);
            for (size_t c = 0; c < ranges.size(); ++c)
                if (ranges[c].first < ranges[c].second)
                    heap.push(c);
            std::ofstream out(output / "q-by-name", std::ios::binary);
            std::vector<uint32_t> buffer;
            buffer.reserve(1 << 20);
            while (!heap.empty()) {
                const auto c = heap.top();
                heap.pop();
                buffer.push_back(qs[ranges[c].first++]);
                if (ranges[c].first < ranges[c].second)
                    heap.push(c);
                if (buffer.size() == buffer.capacity()) {
                    out.write(reinterpret_cast<const char *>(buffer.data()),
                              static_cast<std::streamsize>(buffer.size() * sizeof(uint32_t)));
                    buffer.clear();
                }
            }
            out.write(reinterpret_cast<const char *>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size() * sizeof(uint32_t)));
            if (!out)
                throw std::runtime_error("cannot write q-by-name");
        }
        std::cout << std::format("q-by-name: {:.2f} GB; done in {}", qs.size() * 4 / 1e9, duration(secondsSince(started)))
                  << std::endl;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
