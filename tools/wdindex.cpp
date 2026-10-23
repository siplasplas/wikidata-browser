// wdindex: builds, in one pass over a Wikidata dump, the files from which the
// indexes are made: entity links (triples) and the pages of every bz2 block.
//
// wdindex [-j 20] [-t triples] [-i index] <dump directory> <date>.lst
//
// <date>.lst is written by wdlist; its line NNN gives the dump part whose
// results are triples/NNN and index/NNN. Parts are taken from a queue by the
// threads (the largest first); a part is written to NNN.part and renamed when
// done, so a stopped run continues with the parts not done yet.
//
// triples/NNN: for each claim P.. of an item, the main snak's entity value, as
// little-endian uint32 x 3: property, item, target ((31, 56782, 34770): Q56782
// instance of Q34770).
//
// index/NNN: the bz2 blocks of the dump part (each an independent stream of
// about 100 pages), colon-separated:
//   offset:count:uncompressed      a block: its position in the .bz2, the number
//                                  of its pages, its size uncompressed
//   :start:symbol:title            a page: its start in the uncompressed block
//                                  (it ends where the next one starts, the last
//                                  at uncompressed), entity id (Q42, P31, L7) or
//                                  "other", English label (lexemes: lemma), else
//                                  the first one; "other": the page title
//   EOF=<size>                     the size of the .bz2
// Blocks without pages (siteinfo at the start, </mediawiki> at the end) have
// count 0.
#include <bzlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Part {
    size_t number = 0;
    uint64_t size = 0;
    std::string name;
};

// Lines "NNN:  size:name" of a .lst
std::vector<Part> readList(const fs::path &file) {
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    std::vector<Part> parts;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line.starts_with('#'))
            continue;
        const auto first = line.find(':'), second = line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos)
            throw std::runtime_error("bad line in " + file.string() + ": " + line);
        parts.push_back({std::stoul(line.substr(0, first)), std::stoull(line.substr(first + 1, second - first - 1)),
                         line.substr(second + 1)});
    }
    return parts;
}

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

// English value of labels or lemmas ({"en": {"language": "en", "value": ...}}), else the first
std::string label(const json &entity) {
    for (const char *key : {"labels", "lemmas"}) {
        const auto it = entity.find(key);
        if (it == entity.end() || !it->is_object() || it->empty())
            continue;
        const auto en = it->find("en");
        const auto &chosen = en != it->end() ? *en : it->begin().value();
        if (chosen.is_object() && chosen.contains("value") && chosen["value"].is_string())
            return chosen["value"].get<std::string>();
    }
    return "";
}

// No line breaks in an index line
std::string oneLine(std::string s) {
    std::ranges::replace(s, '\n', ' ');
    std::ranges::replace(s, '\r', ' ');
    return s;
}

struct Progress {
    std::atomic<uint64_t> compressed{0}, pages{0}, triples{0};
    std::atomic<size_t> partsDone{0};
};

// One bz2 block (stream) of a part: index lines of its pages, triples of its items
void processBlock(const std::string &text, uint64_t offset, std::string &index, std::vector<uint32_t> &triples,
                  Progress &progress) {
    std::vector<std::pair<size_t, std::string>> entries; // start, "symbol:title"
    for (size_t pos = 0;;) {
        const auto page = text.find("<page>", pos);
        if (page == std::string::npos)
            break;
        const auto end = text.find("</page>", page);
        if (end == std::string::npos)
            throw std::runtime_error(std::format("page without end in the block at {}", offset));
        pos = end + 7;
        // The entry starts at the beginning of the line of <page>
        const auto lineStart = text.rfind('\n', page);
        const size_t start = lineStart == std::string::npos ? 0 : lineStart + 1;
        std::string_view body(text.data() + page, end - page);
        std::string symbol = "other", title;
        if (const auto t = body.find("<title>"), te = body.find("</title>"); t != std::string_view::npos && te != std::string_view::npos)
            title = unescape(body.substr(t + 7, te - t - 7));
        const auto open = body.find("<text");
        const auto content = open == std::string_view::npos ? open : body.find('>', open);
        const auto close = body.find("</text>");
        if (content != std::string_view::npos && close != std::string_view::npos && body[content - 1] != '/') {
            auto entity = json::parse(unescape(body.substr(content + 1, close - content - 1)), nullptr, false);
            if (!entity.is_discarded() && entity.is_object() && entity.contains("id") && entity["id"].is_string()) {
                symbol = entity["id"].get<std::string>();
                title = label(entity);
                const auto claims = entity.find("claims");
                if (symbol.starts_with('Q') && claims != entity.end() && claims->is_object()) {
                    const auto subject = static_cast<uint32_t>(std::stoul(symbol.substr(1)));
                    for (const auto &[property, statements] : claims->items()) {
                        if (!property.starts_with('P') || !statements.is_array())
                            continue;
                        const auto p = static_cast<uint32_t>(std::stoul(property.substr(1)));
                        for (const auto &statement : statements) {
                            const auto snak = statement.find("mainsnak");
                            if (snak == statement.end())
                                continue;
                            const auto datavalue = snak->find("datavalue");
                            if (datavalue == snak->end() || !datavalue->contains("value"))
                                continue;
                            const auto &value = (*datavalue)["value"];
                            if (!value.is_object())
                                continue;
                            const auto number = value.find("numeric-id");
                            if (number == value.end() || !number->is_number_unsigned())
                                continue;
                            triples.insert(triples.end(), {p, subject, number->get<uint32_t>()});
                            ++progress.triples;
                        }
                    }
                }
            }
        }
        entries.emplace_back(start, symbol + ':' + oneLine(title));
        ++progress.pages;
    }
    index += std::format("{}:{}:{}\n", offset, entries.size(), text.size());
    for (const auto &[start, entry] : entries)
        index += std::format(":{}:{}\n", start, entry);
}

void processPart(const Part &part, const fs::path &dumps, const fs::path &triplesDir, const fs::path &indexDir,
                 Progress &progress) {
    const auto name = std::format("{:03}", part.number);
    std::ifstream in(dumps / part.name, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + (dumps / part.name).string());
    std::ofstream triplesOut(triplesDir / (name + ".part"), std::ios::binary);
    std::ofstream indexOut(indexDir / (name + ".part"), std::ios::binary);
    indexOut << "# wdindex " << part.name << ": offset:count:uncompressed, then :start:symbol:title per page; EOF=<size>\n";

    std::vector<char> buffer(1 << 20), decoded(1 << 22);
    std::string text, index;
    std::vector<uint32_t> triples;
    bz_stream bz{};
    BZ2_bzDecompressInit(&bz, 0, 0);
    uint64_t read = 0;        // bytes read from the file
    uint64_t blockStart = 0;  // offset of the current stream
    for (;;) {
        if (bz.avail_in == 0) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = static_cast<uint64_t>(in.gcount());
            if (got == 0)
                break;
            read += got;
            progress.compressed += got;
            bz.next_in = buffer.data();
            bz.avail_in = static_cast<unsigned>(got);
        }
        bz.next_out = decoded.data();
        bz.avail_out = static_cast<unsigned>(decoded.size());
        const int status = BZ2_bzDecompress(&bz);
        text.append(decoded.data(), decoded.size() - bz.avail_out);
        if (status == BZ_STREAM_END) {
            processBlock(text, blockStart, index, triples, progress);
            text.clear();
            blockStart = read - bz.avail_in;
            char *rest = bz.next_in;
            const unsigned restSize = bz.avail_in;
            BZ2_bzDecompressEnd(&bz);
            bz = bz_stream{};
            BZ2_bzDecompressInit(&bz, 0, 0);
            bz.next_in = rest;
            bz.avail_in = restSize;
            if (index.size() > (8 << 20)) {
                indexOut << index;
                index.clear();
            }
            if (triples.size() > (4 << 20)) {
                triplesOut.write(reinterpret_cast<const char *>(triples.data()),
                                 static_cast<std::streamsize>(triples.size() * sizeof(uint32_t)));
                triples.clear();
            }
        } else if (status != BZ_OK)
            throw std::runtime_error(std::format("{}: bz2 error {} after {} bytes", part.name, status, read));
    }
    BZ2_bzDecompressEnd(&bz);
    if (!text.empty() || blockStart != read)
        throw std::runtime_error(std::format("{}: incomplete bz2 stream at {}", part.name, blockStart));
    indexOut << index << "EOF=" << read << '\n';
    triplesOut.write(reinterpret_cast<const char *>(triples.data()),
                     static_cast<std::streamsize>(triples.size() * sizeof(uint32_t)));
    if (!indexOut || !triplesOut)
        throw std::runtime_error(part.name + ": write failed");
    indexOut.close();
    triplesOut.close();
    fs::rename(triplesDir / (name + ".part"), triplesDir / name);
    fs::rename(indexDir / (name + ".part"), indexDir / name);
}

std::string duration(double seconds) {
    const auto s = static_cast<long>(seconds);
    return std::format("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60);
}

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20;
    fs::path triplesDir = "triples", indexDir = "index";
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "-t" && i + 1 < argc)
            triplesDir = argv[++i];
        else if (arg == "-i" && i + 1 < argc)
            indexDir = argv[++i];
        else
            positional.push_back(arg);
    }
    if (positional.size() != 2) {
        std::cerr << "usage: wdindex [-j 20] [-t triples] [-i index] <dump directory> <date>.lst\n";
        return 1;
    }
    try {
        const fs::path dumps = positional[0];
        auto parts = readList(positional[1]);
        fs::create_directories(triplesDir);
        fs::create_directories(indexDir);
        // Parts done in an earlier run are skipped
        std::vector<Part> todo;
        uint64_t total = 0;
        for (const auto &p : parts) {
            const auto name = std::format("{:03}", p.number);
            if (fs::exists(triplesDir / name) && fs::exists(indexDir / name))
                continue;
            todo.push_back(p);
            total += p.size;
        }
        // The largest first, so that no large part is left for one thread at the end
        std::ranges::sort(todo, std::greater{}, &Part::size);
        std::cout << todo.size() << " of " << parts.size() << " parts to do, " << total / 1e9 << " GB, " << threads
                  << " threads" << std::endl;

        Progress progress;
        std::atomic<size_t> next{0};
        std::mutex mutex;
        std::string error;
        bool finished = false;
        std::condition_variable wake;
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (size_t t = 0; t < std::min(threads, todo.size()); ++t)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < todo.size();) {
                    try {
                        processPart(todo[i], dumps, triplesDir, indexDir, progress);
                        ++progress.partsDone;
                    } catch (const std::exception &e) {
                        std::lock_guard lock(mutex);
                        std::cerr << "\n" << e.what() << std::endl;
                        if (error.empty())
                            error = e.what();
                    }
                }
            });
        // Progress every 10 s: done, speed, time left
        std::thread reporter([&] {
            std::unique_lock lock(mutex);
            while (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return finished; })) {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                const double done = static_cast<double>(progress.compressed);
                const double rate = done / seconds;
                std::cout << std::format("{} {:5.1f}%  {:.1f}/{:.1f} GB  {:.0f} MB/s  parts {}/{}  pages {}  triples {}  left {}",
                                         duration(seconds), total ? 100 * done / total : 100.0, done / 1e9, total / 1e9,
                                         rate / 1e6, progress.partsDone.load(), todo.size(), progress.pages.load(),
                                         progress.triples.load(), rate > 0 ? duration((total - done) / rate) : "?")
                          << std::endl;
            }
        });
        for (auto &thread : pool)
            thread.join();
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        wake.notify_all();
        reporter.join();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << std::format("done in {}: {} parts, {} pages, {} triples ({:.1f} GB)", duration(seconds),
                                 progress.partsDone.load(), progress.pages.load(), progress.triples.load(),
                                 progress.triples * 12 / 1e9)
                  << std::endl;
        if (!error.empty()) {
            std::cerr << "errors: parts with errors are left as .part; run again to redo them" << std::endl;
            return 1;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
