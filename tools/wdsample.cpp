// wdsample: what a slimmer Wikidata would take, measured on a random sample of
// whole bz2 streams (about 100 pages each) of all dump parts.
//
// wdsample [-j 20] [--percent 3] [--seed 1] <dump directory> <date>.lst <Wikimedia index directory>
//
// The offsets of the streams come from the dump's own indexes
// (wikidatawiki-<date>-pages-articles-multistream-index<N>.txt-<range>.bz2,
// "offset:page id:title"). For the items of the sample it measures the JSON
// (compact, as nlohmann dumps it) as it is, without hashes of snaks and
// references, then also without statement ids, then also without references;
// and, for the items in no Wikipedia (claims.h, wikipediaCount), the bytes by
// class (first P31 value): the candidates for leaving out. For the items kept
// (all but scholarly articles in no Wikipedia, as wdfilter) the same sizes,
// and the slim JSON (no hashes, statement ids, references) and the references
// apart (a line per item in the shape of its claims), both compressed with zstd
// in frames of 10 items, levels 3 and 19, without and with a dictionary
// trained on every 50th stream of the sample. Sizes are scaled to the whole
// dump by the share of compressed bytes read.
#include <bzlib.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zdict.h>
#include <zstd.h>

#include "claims.h"
#include "triples.h" // duration, secondsSince

namespace fs = std::filesystem;
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

// Whole decompression of a bz2 file in memory (the indexes are small enough)
std::string bunzip(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    std::string compressed((std::istreambuf_iterator<char>(in)), {});
    std::string out;
    std::vector<char> buffer(1 << 22);
    size_t pos = 0;
    while (pos < compressed.size()) {
        bz_stream bz{};
        BZ2_bzDecompressInit(&bz, 0, 0);
        bz.next_in = compressed.data() + pos;
        bz.avail_in = static_cast<unsigned>(compressed.size() - pos);
        int status = BZ_OK;
        while (status == BZ_OK) {
            bz.next_out = buffer.data();
            bz.avail_out = static_cast<unsigned>(buffer.size());
            status = BZ2_bzDecompress(&bz);
            out.append(buffer.data(), buffer.size() - bz.avail_out);
        }
        pos = compressed.size() - bz.avail_in;
        BZ2_bzDecompressEnd(&bz);
        if (status != BZ_STREAM_END)
            throw std::runtime_error("bad bz2 data in " + file.string());
    }
    return out;
}

// One stream at offset of a dump part, decompressed
std::string stream(const fs::path &file, uint64_t offset, uint64_t length) {
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string compressed(length, '\0');
    in.read(compressed.data(), static_cast<std::streamsize>(length));
    std::string out;
    std::vector<char> buffer(1 << 22);
    bz_stream bz{};
    BZ2_bzDecompressInit(&bz, 0, 0);
    bz.next_in = compressed.data();
    bz.avail_in = static_cast<unsigned>(in.gcount());
    int status = BZ_OK;
    while (status == BZ_OK) {
        bz.next_out = buffer.data();
        bz.avail_out = static_cast<unsigned>(buffer.size());
        status = BZ2_bzDecompress(&bz);
        out.append(buffer.data(), buffer.size() - bz.avail_out);
    }
    BZ2_bzDecompressEnd(&bz);
    if (status != BZ_STREAM_END)
        throw std::runtime_error(std::format("{}: no bz2 stream at {}", file.string(), offset));
    return out;
}

void eraseHashes(json &v) {
    if (v.is_object()) {
        v.erase("hash");
        for (auto &[k, child] : v.items())
            eraseHashes(child);
    } else if (v.is_array())
        for (auto &child : v)
            eraseHashes(child);
}

struct Job {
    size_t part;
    uint64_t offset, length;
};

struct Totals {
    uint64_t compressed = 0, uncompressed = 0, pages = 0, items = 0, other = 0;
    uint64_t xml = 0;            // the JSON as escaped text in the XML
    uint64_t v[4] = {};          // items: as is, no hashes, no statement ids, no references
    uint64_t noWikiItems = 0, noWikiBytes = 0;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> classes; // first P31 of items in no Wikipedia: count, bytes
    uint64_t kept = 0, k[4] = {}, refsRaw = 0;              // items kept: the four sizes; references apart
    uint64_t zData[4] = {}, zRefs[4] = {};                  // zstd: 3, 3 + dictionary, 19, 19 + dictionary
    void add(const Totals &o) {
        kept += o.kept, refsRaw += o.refsRaw;
        for (int i = 0; i < 4; ++i)
            k[i] += o.k[i], zData[i] += o.zData[i], zRefs[i] += o.zRefs[i];
        compressed += o.compressed, uncompressed += o.uncompressed, pages += o.pages, items += o.items, other += o.other;
        xml += o.xml;
        for (int i = 0; i < 4; ++i)
            v[i] += o.v[i];
        noWikiItems += o.noWikiItems, noWikiBytes += o.noWikiBytes;
        for (const auto &[c, nb] : o.classes) {
            classes[c].first += nb.first;
            classes[c].second += nb.second;
        }
    }
};

// zstd of frames of 10 items, one per thread; without dictionaries (training)
// it only collects samples
struct Compressor {
    static constexpr int levels[2] = {3, 19};
    const ZSTD_CDict *dataDict[2] = {}, *refsDict[2] = {};
    ZSTD_CCtx *ctx = ZSTD_createCCtx();
    std::string data, refs, out;
    int dataCount = 0, refsCount = 0;
    std::vector<std::string> *dataSamples = nullptr, *refsSamples = nullptr; // training
    ~Compressor() { ZSTD_freeCCtx(ctx); }
    void compress(const std::string &frame, const ZSTD_CDict *const dicts[2], uint64_t z[4]) {
        out.resize(ZSTD_compressBound(frame.size()));
        for (int l = 0; l < 2; ++l) {
            z[2 * l] += ZSTD_compressCCtx(ctx, out.data(), out.size(), frame.data(), frame.size(), levels[l]);
            z[2 * l + 1] += ZSTD_compress_usingCDict(ctx, out.data(), out.size(), frame.data(), frame.size(), dicts[l]);
        }
    }
    void add(std::string &frame, int &count, const std::string &line, const ZSTD_CDict *const dicts[2], uint64_t z[4],
             std::vector<std::string> *samples) {
        if (samples) {
            samples->push_back(line);
            return;
        }
        frame += line;
        frame += '\n';
        if (++count == 10)
            flush(frame, count, dicts, z);
    }
    void flush(std::string &frame, int &count, const ZSTD_CDict *const dicts[2], uint64_t z[4]) {
        if (count)
            compress(frame, dicts, z);
        frame.clear();
        count = 0;
    }
};

// Scholarly article (P31 = Q13442814, not deprecated) in no Wikipedia: left out, as by wdfilter
bool leftOut(const json &entity) {
    const auto claims = entity.find("claims");
    if (claims == entity.end() || !claims->is_object())
        return false;
    const auto p31 = claims->find("P31");
    if (p31 == claims->end() || !p31->is_array())
        return false;
    bool scholarly = false;
    for (const auto &statement : *p31) {
        if (statement.value("rank", "normal") == "deprecated")
            continue;
        const auto &value = statement.value("mainsnak", json::object()).value("datavalue", json::object()).value("value", json());
        if (value.is_object() && value.value("numeric-id", json(0)) == 13442814)
            scholarly = true;
    }
    return scholarly && wikipediaCount(entity) == 0;
}

void measure(const std::string &text, Totals &t, Compressor &z) {
    for (size_t pos = 0;;) {
        const auto page = text.find("<page>", pos);
        if (page == std::string::npos)
            break;
        const auto end = text.find("</page>", page);
        if (end == std::string::npos)
            break;
        pos = end + 7;
        ++t.pages;
        std::string_view body(text.data() + page, end - page);
        const auto open = body.find("<text");
        const auto content = open == std::string_view::npos ? open : body.find('>', open);
        const auto close = body.find("</text>");
        if (content == std::string_view::npos || close == std::string_view::npos || body[content - 1] == '/')
            continue;
        const auto escaped = body.substr(content + 1, close - content - 1);
        auto entity = json::parse(unescape(escaped), nullptr, false);
        if (entity.is_discarded() || !entity.is_object() || !entity.contains("id") || !entity["id"].is_string() ||
            !entity["id"].get_ref<const std::string &>().starts_with('Q')) {
            ++t.other;
            continue;
        }
        ++t.items;
        t.xml += escaped.size();
        const uint64_t asIs = entity.dump().size();
        t.v[0] += asIs;
        // Class: the first P31 value; Wikipedias: language editions with an article
        uint32_t cls = 0;
        if (const auto claims = entity.find("claims"); claims != entity.end() && claims->is_object())
            if (const auto p31 = claims->find("P31"); p31 != claims->end() && p31->is_array() && !p31->empty()) {
                const auto &value = (*p31)[0].value("mainsnak", json::object()).value("datavalue", json::object()).value("value", json());
                if (value.is_object() && value.contains("numeric-id") && value["numeric-id"].is_number_unsigned())
                    cls = value["numeric-id"].get<uint32_t>();
            }
        if (wikipediaCount(entity) == 0) {
            ++t.noWikiItems;
            t.noWikiBytes += asIs;
            auto &c = t.classes[cls];
            ++c.first;
            c.second += asIs;
        }
        const bool kept = !leftOut(entity);
        if (kept) {
            ++t.kept;
            t.k[0] += asIs;
        }
        eraseHashes(entity);
        const uint64_t noHashes = entity.dump().size();
        t.v[1] += noHashes;
        if (kept)
            t.k[1] += noHashes;
        auto statements = [&](auto &&f) {
            if (auto claims = entity.find("claims"); claims != entity.end() && claims->is_object())
                for (auto &[p, list] : claims->items())
                    if (list.is_array())
                        for (auto &statement : list)
                            if (statement.is_object())
                                f(statement);
        };
        statements([](json &s) { s.erase("id"); });
        const uint64_t noIds = entity.dump().size();
        t.v[2] += noIds;
        // References apart: {"id":..,"references":{"P569":[[ref, ref], []]}}
        json refs = json::object();
        bool anyRefs = false;
        if (auto claims = entity.find("claims"); claims != entity.end() && claims->is_object())
            for (auto &[p, list] : claims->items()) {
                if (!list.is_array())
                    continue;
                json perStatement = json::array();
                for (auto &statement : list) {
                    json r = statement.is_object() && statement.contains("references") ? statement["references"] : json::array();
                    anyRefs |= !r.empty();
                    perStatement.push_back(std::move(r));
                }
                refs[p] = std::move(perStatement);
            }
        statements([](json &s) { s.erase("references"); });
        const std::string slim = entity.dump();
        t.v[3] += slim.size();
        if (kept) {
            t.k[2] += noIds;
            t.k[3] += slim.size();
            z.add(z.data, z.dataCount, slim, z.dataDict, t.zData, z.dataSamples);
            if (anyRefs) {
                const std::string line = json{{"id", entity["id"]}, {"references", std::move(refs)}}.dump();
                t.refsRaw += line.size() + 1;
                z.add(z.refs, z.refsCount, line, z.refsDict, t.zRefs, z.refsSamples);
            }
        }
    }
    // Frames do not cross the streams of the dump
    if (!z.dataSamples) {
        z.flush(z.data, z.dataCount, z.dataDict, t.zData);
        z.flush(z.refs, z.refsCount, z.refsDict, t.zRefs);
    }
}

// A zstd dictionary trained on samples (at most about 100 MB of them)
std::string trainDictionary(const std::vector<std::string> &samples, size_t size) {
    std::string buffer;
    std::vector<size_t> sizes;
    for (const auto &s : samples) {
        if (buffer.size() + s.size() > (100u << 20))
            break;
        buffer += s;
        sizes.push_back(s.size());
    }
    std::string dictionary(size, '\0');
    const size_t made = ZDICT_trainFromBuffer(dictionary.data(), dictionary.size(), buffer.data(), sizes.data(),
                                              static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(made))
        throw std::runtime_error(std::string("dictionary training failed: ") + ZDICT_getErrorName(made));
    dictionary.resize(made);
    return dictionary;
}

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20;
    double percent = 3;
    unsigned seed = 1;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "--percent" && i + 1 < argc)
            percent = std::stod(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc)
            seed = static_cast<unsigned>(std::stoul(argv[++i]));
        else
            positional.push_back(arg);
    }
    if (positional.size() != 3) {
        std::cerr << "usage: wdsample [-j 20] [--percent 3] [--seed 1] <dump directory> <date>.lst <Wikimedia index directory>\n";
        return 1;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const fs::path dumps = positional[0], indexes = positional[2];
        std::vector<std::string> parts;
        {
            std::ifstream in(positional[1]);
            if (!in)
                throw std::runtime_error("cannot open " + positional[1]);
            for (std::string line; std::getline(in, line);)
                if (!line.empty() && !line.starts_with('#'))
                    parts.push_back(line.substr(line.find(':', line.find(':') + 1) + 1));
        }
        // Streams of every part from its index; a random share of them
        static const std::regex partName(R"((.*)-multistream(\d+)\.xml-(p\d+p\d+)\.bz2)");
        std::vector<Job> jobs;
        uint64_t totalCompressed = 0;
        std::mt19937_64 random(seed);
        std::bernoulli_distribution take(percent / 100);
        for (size_t p = 0; p < parts.size(); ++p) {
            std::smatch m;
            if (!std::regex_match(parts[p], m, partName))
                throw std::runtime_error("unexpected part name " + parts[p]);
            const auto indexFile = indexes / std::format("{}-multistream-index{}.txt-{}.bz2", m[1].str(), m[2].str(), m[3].str());
            const auto text = bunzip(indexFile);
            std::vector<uint64_t> offsets;
            for (size_t pos = 0; pos < text.size();) {
                const auto eol = text.find('\n', pos);
                const auto colon = text.find(':', pos);
                if (colon < eol) {
                    const auto offset = std::stoull(text.substr(pos, colon - pos));
                    if (offsets.empty() || offsets.back() != offset)
                        offsets.push_back(offset);
                }
                pos = eol == std::string::npos ? text.size() : eol + 1;
            }
            const uint64_t size = fs::file_size(dumps / parts[p]);
            totalCompressed += size;
            for (size_t i = 0; i < offsets.size(); ++i)
                if (take(random))
                    jobs.push_back({p, offsets[i], (i + 1 < offsets.size() ? offsets[i + 1] : size) - offsets[i]});
        }
        std::cout << std::format("{} streams sampled of {} parts ({}%), read in {} threads ({})", jobs.size(), parts.size(),
                                 percent, threads, duration(secondsSince(started)))
                  << std::endl;

        // Dictionaries for zstd: trained on every 50th stream of the sample
        std::mutex mutex;
        std::vector<std::string> dataSamples, refsSamples;
        {
            std::atomic<size_t> next{0};
            std::vector<std::thread> pool;
            for (size_t t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    std::vector<std::string> data, refs;
                    Compressor z;
                    z.dataSamples = &data;
                    z.refsSamples = &refs;
                    Totals ignored;
                    for (size_t i; (i = next.fetch_add(50)) < jobs.size();) {
                        const auto &job = jobs[i];
                        measure(stream(dumps / parts[job.part], job.offset, job.length), ignored, z);
                    }
                    std::lock_guard lock(mutex);
                    for (auto &d : data)
                        dataSamples.push_back(std::move(d));
                    for (auto &r : refs)
                        refsSamples.push_back(std::move(r));
                });
            for (auto &thread : pool)
                thread.join();
        }
        std::shuffle(dataSamples.begin(), dataSamples.end(), random);
        std::shuffle(refsSamples.begin(), refsSamples.end(), random);
        const auto dataDictionary = trainDictionary(dataSamples, 110 << 10);
        const auto refsDictionary = trainDictionary(refsSamples, 110 << 10);
        dataSamples.clear();
        refsSamples.clear();
        ZSTD_CDict *dicts[4];
        for (int l = 0; l < 2; ++l) {
            dicts[l] = ZSTD_createCDict(dataDictionary.data(), dataDictionary.size(), Compressor::levels[l]);
            dicts[2 + l] = ZSTD_createCDict(refsDictionary.data(), refsDictionary.size(), Compressor::levels[l]);
        }
        std::cout << std::format("dictionaries: {} B for the items, {} B for the references ({})", dataDictionary.size(),
                                 refsDictionary.size(), duration(secondsSince(started)))
                  << std::endl;

        Totals totals;
        std::atomic<size_t> next{0}, done{0};
        std::vector<std::thread> pool;
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                Totals mine;
                Compressor z;
                for (int l = 0; l < 2; ++l) {
                    z.dataDict[l] = dicts[l];
                    z.refsDict[l] = dicts[2 + l];
                }
                for (size_t i; (i = next++) < jobs.size();) {
                    const auto &job = jobs[i];
                    const auto text = stream(dumps / parts[job.part], job.offset, job.length);
                    mine.compressed += job.length;
                    mine.uncompressed += text.size();
                    measure(text, mine, z);
                    if (++done % 2000 == 0) {
                        std::lock_guard lock(mutex);
                        std::cout << std::format("  {}/{} streams ({})", done.load(), jobs.size(), duration(secondsSince(started)))
                                  << std::endl;
                    }
                }
                std::lock_guard lock(mutex);
                totals.add(mine);
            });
        for (auto &thread : pool)
            thread.join();

        const double scale = totals.compressed ? double(totalCompressed) / totals.compressed : 0;
        auto gb = [&](uint64_t bytes) { return std::format("{:8.1f} GB", bytes * scale / 1e9); };
        std::cout << std::format("\nsample: {} streams, {:.2f} GB compressed, {:.2f} GB uncompressed, {} pages, {} items, {} other\n"
                                 "scaled to the whole dump ({:.1f} GB compressed, x{:.1f}):\n",
                                 jobs.size(), totals.compressed / 1e9, totals.uncompressed / 1e9, totals.pages, totals.items,
                                 totals.other, totalCompressed / 1e9, scale)
                  << "  uncompressed XML of the streams     " << gb(totals.uncompressed) << "\n"
                  << "  items, JSON escaped in the XML      " << gb(totals.xml) << "\n"
                  << "  items, JSON                         " << gb(totals.v[0]) << "\n"
                  << "    without hashes                    " << gb(totals.v[1]) << "\n"
                  << "    and without statement ids         " << gb(totals.v[2]) << "\n"
                  << "    and without references            " << gb(totals.v[3]) << "\n"
                  << std::format("  items in no Wikipedia: {:.1f}% of items, {}\n", 100.0 * totals.noWikiItems / std::max<uint64_t>(1, totals.items),
                                 gb(totals.noWikiBytes))
                  << "\nclasses (first P31) of items in no Wikipedia, by JSON bytes (scaled):\n";
        std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> classes(totals.classes.begin(), totals.classes.end());
        std::ranges::sort(classes, std::greater{}, [](const auto &c) { return c.second.second; });
        for (size_t i = 0; i < classes.size() && i < 40; ++i)
            std::cout << std::format("  Q{:<10} {:>12.0f} items {}  {:4.1f}%\n", classes[i].first, classes[i].second.first * scale,
                                     gb(classes[i].second.second), 100.0 * classes[i].second.second / std::max<uint64_t>(1, totals.v[0]));
        std::cout << "(Q0: no P31)\n";
        std::cout << std::format("\nitems kept (all but scholarly articles in no Wikipedia): {:.1f}% of items\n",
                                 100.0 * totals.kept / std::max<uint64_t>(1, totals.items))
                  << "  JSON                                " << gb(totals.k[0]) << "\n"
                  << "    without hashes                    " << gb(totals.k[1]) << "\n"
                  << "    and without statement ids         " << gb(totals.k[2]) << "\n"
                  << "    and without references (slim)     " << gb(totals.k[3]) << "\n"
                  << "  references apart                    " << gb(totals.refsRaw) << "\n"
                  << "zstd, frames of 10 items:              level 3   + dict   level 19   + dict\n"
                  << std::format("  slim items                        {:8.1f} {:8.1f} {:9.1f} {:8.1f} GB\n",
                                 totals.zData[0] * scale / 1e9, totals.zData[1] * scale / 1e9, totals.zData[2] * scale / 1e9,
                                 totals.zData[3] * scale / 1e9)
                  << std::format("  references                        {:8.1f} {:8.1f} {:9.1f} {:8.1f} GB\n",
                                 totals.zRefs[0] * scale / 1e9, totals.zRefs[1] * scale / 1e9, totals.zRefs[2] * scale / 1e9,
                                 totals.zRefs[3] * scale / 1e9)
                  << "done in " << duration(secondsSince(started)) << std::endl;
        for (auto *d : dicts)
            ZSTD_freeCDict(d);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
