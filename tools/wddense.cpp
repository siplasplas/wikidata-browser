// wddense: dense indexes of entities from the block indexes of wdindex.
//
// wddense [-i index] [-s sitelinks] [-o .] <date>.lst
//
// Reads index/NNN of every part NNN of the .lst and writes, in the output
// directory, an index per entity kind where the entry of number N is at
// N * entry size, so finding Q42 is one seek:
//   q.idx, l.idx   12-byte entries for Q0..Qmax, L0..Lmax
//   p.idx          uint32 count, then 16-byte entries for P0..Pmax (12 bytes
//                  as below + uint32 offset of the English label in the
//                  names after the entries), then the names, UTF-8, each
//                  ended by \0 (label: en, else the first)
// An entry, little-endian:
//   bytes 0-1   number of the dump part (line NNN of the .lst)
//   bytes 2-7   offset of the page's bz2 stream in that part (48 bits)
//   bytes 8-11  start of the page in the uncompressed stream
//   popularity     uint16 for Q0..Qmax: the number of language Wikipedias with
//                  an article on the item (sitelinks/NNN of wdindex), for
//                  ranking names found (Earth: over 300). Made when sitelinks/ exists.
// An absent entity (never created, deleted, merged into another: its page is a
// redirect) has an entry of zeros: offset 0 is the siteinfo stream of every
// part, never a page.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Entry = std::array<uint8_t, 12>;

Entry makeEntry(uint16_t part, uint64_t offset, uint32_t start) {
    if (offset >> 48)
        throw std::runtime_error(std::format("offset {} needs more than 48 bits", offset));
    Entry e{};
    for (int i = 0; i < 2; ++i)
        e[i] = static_cast<uint8_t>(part >> (8 * i));
    for (int i = 0; i < 6; ++i)
        e[2 + i] = static_cast<uint8_t>(offset >> (8 * i));
    for (int i = 0; i < 4; ++i)
        e[8 + i] = static_cast<uint8_t>(start >> (8 * i));
    return e;
}

template <class T> T number(std::string_view s, const std::string &where) {
    T value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (error != std::errc() || end != s.data() + s.size())
        throw std::runtime_error(std::format("{}: not a number: '{}'", where, s));
    return value;
}

struct Kind {
    char letter;
    std::vector<Entry> entries;
    std::vector<std::string> labels; // P only
    uint64_t count = 0, repeated = 0;

    void add(uint32_t n, const Entry &e, std::string_view label) {
        if (entries.size() <= n)
            entries.resize(n + 1, Entry{});
        if (entries[n] != Entry{})
            ++repeated;
        else
            ++count;
        entries[n] = e;
        if (letter == 'P') {
            if (labels.size() <= n)
                labels.resize(n + 1);
            labels[n] = label;
        }
    }
};

} // namespace

int main(int argc, char *argv[]) {
    fs::path indexDir = "index", sitelinksDir = "sitelinks", output = ".";
    std::string list;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
            indexDir = argv[++i];
        else if (arg == "-s" && i + 1 < argc)
            sitelinksDir = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else
            list = arg;
    }
    if (list.empty()) {
        std::cerr << "usage: wddense [-i index] [-s sitelinks] [-o .] <date>.lst\n";
        return 1;
    }
    try {
        std::vector<uint16_t> parts;
        {
            std::ifstream in(list);
            if (!in)
                throw std::runtime_error("cannot open " + list);
            for (std::string line; std::getline(in, line);)
                if (!line.empty() && !line.starts_with('#'))
                    parts.push_back(number<uint16_t>(std::string_view(line).substr(0, line.find(':')), list));
        }
        Kind q{'Q'}, p{'P'}, l{'L'};
        uint64_t other = 0;
        for (const auto part : parts) {
            const auto file = indexDir / std::format("{:03}", part);
            std::ifstream in(file);
            if (!in)
                throw std::runtime_error("cannot open " + file.string() + " (wdindex not finished?)");
            uint64_t offset = 0;
            bool complete = false;
            size_t lineNumber = 0;
            for (std::string line; std::getline(in, line);) {
                ++lineNumber;
                const auto where = std::format("{}:{}", file.string(), lineNumber);
                if (line.empty() || line.starts_with('#'))
                    continue;
                if (line.starts_with("EOF=")) {
                    complete = true;
                    continue;
                }
                std::string_view v(line);
                if (v[0] != ':') { // block: offset:count:uncompressed
                    offset = number<uint64_t>(v.substr(0, v.find(':')), where);
                    continue;
                }
                // page: :start:symbol:title
                const auto second = v.find(':', 1), third = v.find(':', second + 1);
                if (second == std::string_view::npos || third == std::string_view::npos)
                    throw std::runtime_error(where + ": bad line");
                const auto start = number<uint32_t>(v.substr(1, second - 1), where);
                const auto symbol = v.substr(second + 1, third - second - 1);
                const auto title = v.substr(third + 1);
                Kind *kind = symbol.starts_with('Q') ? &q : symbol.starts_with('P') ? &p : symbol.starts_with('L') ? &l : nullptr;
                if (!kind) {
                    ++other;
                    continue;
                }
                kind->add(number<uint32_t>(symbol.substr(1), where), makeEntry(part, offset, start), title);
            }
            if (!complete)
                throw std::runtime_error(file.string() + ": no EOF line (wdindex not finished?)");
        }

        fs::create_directories(output);
        for (const auto *kind : {&q, &l}) {
            const auto name = output / std::format("{}.idx", char(std::tolower(kind->letter)));
            std::ofstream out(name, std::ios::binary);
            out.write(reinterpret_cast<const char *>(kind->entries.data()),
                      static_cast<std::streamsize>(kind->entries.size() * sizeof(Entry)));
            if (!out)
                throw std::runtime_error("cannot write " + name.string());
            std::cout << std::format("{}: {} entities, {}{} max, {:.1f} MB{}", name.string(), kind->count, kind->letter,
                                     kind->entries.empty() ? 0 : kind->entries.size() - 1,
                                     kind->entries.size() * 12 / 1e6,
                                     kind->repeated ? std::format(", {} repeated (the last kept)", kind->repeated) : "")
                      << std::endl;
        }
        {
            const auto name = output / "p.idx";
            std::ofstream out(name, std::ios::binary);
            const auto count = static_cast<uint32_t>(p.entries.size());
            out.write(reinterpret_cast<const char *>(&count), 4);
            std::string names;
            for (uint32_t n = 0; n < count; ++n) {
                const auto nameOffset = static_cast<uint32_t>(names.size());
                out.write(reinterpret_cast<const char *>(p.entries[n].data()), 12);
                out.write(reinterpret_cast<const char *>(&nameOffset), 4);
                names += p.labels[n];
                names += '\0';
            }
            out << names;
            if (!out)
                throw std::runtime_error("cannot write " + name.string());
            std::cout << std::format("{}: {} properties, P{} max, {:.1f} MB with names", name.string(), p.count,
                                     count ? count - 1 : 0, (4 + count * 16.0 + names.size()) / 1e6)
                      << std::endl;
        }
        std::cout << other << " other pages (redirects, not entities) left out" << std::endl;
        // Popularity: Wikipedias per item
        if (fs::is_directory(sitelinksDir)) {
            std::vector<uint16_t> popularity(q.entries.size(), 0);
            uint64_t items = 0;
            for (const auto part : parts) {
                const auto file = sitelinksDir / std::format("{:03}", part);
                std::ifstream in(file, std::ios::binary);
                if (!in)
                    throw std::runtime_error("cannot open " + file.string() + " (wdindex not finished?)");
                for (std::array<uint8_t, 6> r; in.read(reinterpret_cast<char *>(r.data()), 6);) {
                    const uint32_t item = r[0] | r[1] << 8 | r[2] << 16 | uint32_t(r[3]) << 24;
                    const unsigned count = r[4] | r[5] << 8;
                    if (item >= popularity.size())
                        popularity.resize(item + 1, 0);
                    popularity[item] = static_cast<uint16_t>(count);
                    ++items;
                }
            }
            const auto name = output / "popularity";
            std::ofstream out(name, std::ios::binary);
            out.write(reinterpret_cast<const char *>(popularity.data()),
                      static_cast<std::streamsize>(popularity.size() * sizeof(uint16_t)));
            if (!out)
                throw std::runtime_error("cannot write " + name.string());
            std::cout << std::format("{}: {} items in Wikipedias, {:.1f} MB", name.string(), items, popularity.size() * 2 / 1e6)
                      << std::endl;
        } else
            std::cout << "no " << sitelinksDir.string() << "/: no popularity" << std::endl;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
