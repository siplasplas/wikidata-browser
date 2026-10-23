// wdtriples: links between Wikidata items from the start of a dump part, as
// 12-byte triples (triples.h): property, item << 2 | rank, target, e.g.
// (31, 56782 << 2 | normal, 34770) for "Q56782 instance of (P31) Q34770".
//
// wdtriples [--limit MB] [-o triples.bin] dump.xml.bz2
//
// Reads the bz2 streams of a pages-articles-multistream part (with --limit
// only its first MB of compressed data), takes the JSON of each entity page
// (<text>) and, for each claim P.. of it, the main snak's value when it is an
// entity (datavalue.value.numeric-id). Prints the sizes, the number of pages,
// entities and triples; -o writes the triples.
#include <bzlib.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "claims.h"

using json = nlohmann::json;

namespace {

// &quot; &lt; &gt; &amp; &#039; of the XML text
std::string unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        static const std::pair<std::string_view, char> entities[] = {
                {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&#039;", '\''}, {"&apos;", '\''}};
        bool found = false;
        for (const auto &[entity, c] : entities)
            if (s.substr(i, entity.size()) == entity) {
                out += c;
                i += entity.size() - 1;
                found = true;
                break;
            }
        if (!found)
            out += '&';
    }
    return out;
}

struct Stats {
    uint64_t compressed = 0, uncompressed = 0, pages = 0, items = 0, properties = 0, other = 0, invalid = 0;
    uint64_t triples = 0, statements = 0;
    std::map<std::string, uint64_t> targetTypes, ranks; // of the triples
};

} // namespace

int main(int argc, char *argv[]) {
    uint64_t limit = 0;
    std::string input, output;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--limit" && i + 1 < argc)
            limit = std::stoull(argv[++i]) * 1000 * 1000;
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else
            input = arg;
    }
    if (input.empty()) {
        std::cerr << "usage: wdtriples [--limit MB] [-o triples.bin] dump.xml.bz2\n";
        return 1;
    }
    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << input << std::endl;
        return 1;
    }
    std::ofstream out;
    if (!output.empty())
        out.open(output, std::ios::binary);
    const auto started = std::chrono::steady_clock::now();
    Stats stats;
    std::string text; // decompressed, not yet parsed
    std::vector<Triple> triples;
    std::vector<char> buffer(1 << 20), decoded(1 << 22);
    bz_stream bz{};
    BZ2_bzDecompressInit(&bz, 0, 0);

    // Whole <page> elements of text are parsed and removed
    auto parsePages = [&] {
        size_t pos = 0;
        for (;;) {
            const auto start = text.find("<page>", pos);
            if (start == std::string::npos)
                break;
            const auto end = text.find("</page>", start);
            if (end == std::string::npos) {
                pos = start;
                break;
            }
            std::string_view page(text.data() + start, end - start);
            pos = end + 7;
            ++stats.pages;
            const auto open = page.find("<text");
            const auto content = open == std::string_view::npos ? open : page.find('>', open);
            const auto close = page.find("</text>");
            if (content == std::string_view::npos || close == std::string_view::npos || page[content - 1] == '/')
                continue;
            auto entity = json::parse(unescape(page.substr(content + 1, close - content - 1)), nullptr, false);
            if (entity.is_discarded() || !entity.is_object() || !entity.contains("id")) {
                ++stats.invalid;
                continue;
            }
            const auto id = entity["id"].get<std::string>();
            if (id.starts_with('Q'))
                ++stats.items;
            else if (id.starts_with('P'))
                ++stats.properties;
            else
                ++stats.other;
            for (const auto &[property, statements] : entity.value("claims", json::object()).items())
                if (statements.is_array())
                    stats.statements += statements.size();
            triples.clear();
            size_t other = 0;
            appendTriples(entity, triples, &other);
            stats.targetTypes["property or lexeme, left out"] += other;
            for (const auto &t : triples) {
                ++stats.triples;
                ++stats.ranks[t.rank() == Rank::preferred ? "preferred" : t.rank() == Rank::deprecated ? "deprecated" : "normal"];
            }
            if (out)
                out.write(reinterpret_cast<const char *>(triples.data()), static_cast<std::streamsize>(triples.size() * sizeof(Triple)));
        }
        text.erase(0, pos);
    };

    bool done = false;
    while (!done) {
        if (bz.avail_in == 0) {
            if (limit && stats.compressed >= limit)
                break;
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (in.gcount() == 0)
                break;
            stats.compressed += in.gcount();
            bz.next_in = buffer.data();
            bz.avail_in = static_cast<unsigned>(in.gcount());
        }
        bz.next_out = decoded.data();
        bz.avail_out = static_cast<unsigned>(decoded.size());
        const int status = BZ2_bzDecompress(&bz);
        const size_t produced = decoded.size() - bz.avail_out;
        stats.uncompressed += produced;
        text.append(decoded.data(), produced);
        if (status == BZ_STREAM_END) {
            // Next stream of the multistream file
            char *rest = bz.next_in;
            const unsigned restSize = bz.avail_in;
            BZ2_bzDecompressEnd(&bz);
            bz = bz_stream{};
            BZ2_bzDecompressInit(&bz, 0, 0);
            bz.next_in = rest;
            bz.avail_in = restSize;
        } else if (status != BZ_OK) {
            std::cerr << "bz2 error " << status << std::endl;
            done = true;
        }
        if (text.size() > (64 << 20))
            parsePages();
    }
    parsePages();
    BZ2_bzDecompressEnd(&bz);
    // Bytes of the last buffer not read when stopped at the limit
    stats.compressed -= bz.avail_in;

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "compressed read:   " << stats.compressed << " bytes\n"
              << "uncompressed:      " << stats.uncompressed << " bytes (ratio " << double(stats.uncompressed) / stats.compressed
              << ")\n"
              << "pages:             " << stats.pages << " (items " << stats.items << ", properties " << stats.properties
              << ", other " << stats.other << ", without entity JSON " << stats.invalid << ")\n"
              << "statements:        " << stats.statements << "\n"
              << "triples:           " << stats.triples << " = " << stats.triples * sizeof(Triple) << " bytes ("
              << double(stats.triples) / std::max<uint64_t>(1, stats.items) << " per item)\n"
              << "other targets:    ";
    for (const auto &[type, count] : stats.targetTypes)
        std::cout << " " << type << " " << count;
    std::cout << "\nranks:            ";
    for (const auto &[rank, count] : stats.ranks)
        std::cout << " " << rank << " " << count;
    std::cout << "\ntime:              " << seconds << " s\n";
}
