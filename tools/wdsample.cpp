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
    // zstd (level 3, level 3 + dictionary, level 19 + dictionary) of frames of about
    // 64 KiB, 256 KiB, 512 KiB, 1 MiB of items with references; and 512 KiB without
    uint64_t zData[4][3] = {}, zSlim[3] = {}, frames[4] = {};
    void add(const Totals &o) {
        kept += o.kept, refsRaw += o.refsRaw;
        for (int i = 0; i < 4; ++i) {
            k[i] += o.k[i], frames[i] += o.frames[i];
            for (int j = 0; j < 3; ++j)
                zData[i][j] += o.zData[i][j];
        }
        for (int j = 0; j < 3; ++j)
            zSlim[j] += o.zSlim[j];
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

// Frames of items about a size (uncompressed; an item larger than it is a frame
// of its own), compressed with zstd: level 3, level 3 + dictionary, level 19 +
// dictionary. Frames may span streams of the sample, as items of a part do.
struct Series {
    size_t target;
    std::string frame;
    uint64_t z[3] = {};
    uint64_t frames = 0;
};

// One per thread; without dictionaries (training) it only collects samples
struct Compressor {
    const ZSTD_CDict *dataDict3 = nullptr, *dataDict19 = nullptr, *slimDict3 = nullptr, *slimDict19 = nullptr;
    ZSTD_CCtx *ctx = ZSTD_createCCtx();
    std::string out;
    Series data[4] = {{64 << 10}, {256 << 10}, {512 << 10}, {1 << 20}}; // items with references
    Series slim{512 << 10};                                               // items without references
    std::vector<std::string> *dataSamples = nullptr, *slimSamples = nullptr; // training
    ~Compressor() { ZSTD_freeCCtx(ctx); }
    void compress(Series &series, const ZSTD_CDict *dict3, const ZSTD_CDict *dict19) {
        const auto &f = series.frame;
        out.resize(ZSTD_compressBound(f.size()));
        series.z[0] += ZSTD_compressCCtx(ctx, out.data(), out.size(), f.data(), f.size(), 3);
        series.z[1] += ZSTD_compress_usingCDict(ctx, out.data(), out.size(), f.data(), f.size(), dict3);
        series.z[2] += ZSTD_compress_usingCDict(ctx, out.data(), out.size(), f.data(), f.size(), dict19);
        ++series.frames;
        series.frame.clear();
    }
    void add(const std::string &withReferences, const std::string &withoutReferences) {
        if (dataSamples) {
            dataSamples->push_back(withReferences);
            slimSamples->push_back(withoutReferences);
            return;
        }
        for (auto &series : data) {
            series.frame += withReferences;
            series.frame += '\n';
            if (series.frame.size() >= series.target)
                compress(series, dataDict3, dataDict19);
        }
        slim.frame += withoutReferences;
        slim.frame += '\n';
        if (slim.frame.size() >= slim.target)
            compress(slim, slimDict3, slimDict19);
    }
    // The last frames, and the sizes into t
    void finish(Totals &t) {
        if (dataSamples)
            return;
        for (int i = 0; i < 4; ++i) {
            if (!data[i].frame.empty())
                compress(data[i], dataDict3, dataDict19);
            for (int j = 0; j < 3; ++j)
                t.zData[i][j] += data[i].z[j];
            t.frames[i] += data[i].frames;
        }
        if (!slim.frame.empty())
            compress(slim, slimDict3, slimDict19);
        for (int j = 0; j < 3; ++j)
            t.zSlim[j] += slim.z[j];
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
        const uint64_t noIds = entity.dump().size(); // with references
        t.v[2] += noIds;
        const std::string withReferences = entity.dump();
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
            z.add(withReferences, slim);
            if (anyRefs)
                t.refsRaw += json{{"id", entity["id"]}, {"references", std::move(refs)}}.dump().size() + 1;
        }
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
        std::vector<std::string> dataSamples, slimSamples;
        {
            std::atomic<size_t> next{0};
            std::vector<std::thread> pool;
            for (size_t t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    std::vector<std::string> data, slims;
                    Compressor z;
                    z.dataSamples = &data;
                    z.slimSamples = &slims;
                    Totals ignored;
                    for (size_t i; (i = next.fetch_add(50)) < jobs.size();) {
                        const auto &job = jobs[i];
                        measure(stream(dumps / parts[job.part], job.offset, job.length), ignored, z);
                    }
                    std::lock_guard lock(mutex);
                    for (auto &d : data)
                        dataSamples.push_back(std::move(d));
                    for (auto &r : slims)
                        slimSamples.push_back(std::move(r));
                });
            for (auto &thread : pool)
                thread.join();
        }
        std::shuffle(dataSamples.begin(), dataSamples.end(), random);
        std::shuffle(slimSamples.begin(), slimSamples.end(), random);
        const auto dataDictionary = trainDictionary(dataSamples, 110 << 10);
        const auto slimDictionary = trainDictionary(slimSamples, 110 << 10);
        dataSamples.clear();
        slimSamples.clear();
        ZSTD_CDict *dicts[4] = {ZSTD_createCDict(dataDictionary.data(), dataDictionary.size(), 3),
                                ZSTD_createCDict(dataDictionary.data(), dataDictionary.size(), 19),
                                ZSTD_createCDict(slimDictionary.data(), slimDictionary.size(), 3),
                                ZSTD_createCDict(slimDictionary.data(), slimDictionary.size(), 19)};
        std::cout << std::format("dictionaries: {} B with references, {} B without ({})", dataDictionary.size(),
                                 slimDictionary.size(), duration(secondsSince(started)))
                  << std::endl;

        Totals totals;
        std::atomic<size_t> next{0}, done{0};
        std::vector<std::thread> pool;
        for (size_t t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                Totals mine;
                Compressor z;
                z.dataDict3 = dicts[0], z.dataDict19 = dicts[1], z.slimDict3 = dicts[2], z.slimDict19 = dicts[3];
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
                z.finish(mine);
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
                  << "zstd of the items kept, frames by size (uncompressed):   level 3  + dict  level 19 + dict\n";
        const char *sizes[4] = {" 64 KiB", "256 KiB", "512 KiB", "  1 MiB"};
        for (int i = 0; i < 4; ++i)
            std::cout << std::format("  with references, {} ({:5.1f} items/frame)      {:7.1f}  {:7.1f}  {:7.1f} GB\n", sizes[i],
                                     double(totals.kept) / std::max<uint64_t>(1, totals.frames[i]),
                                     totals.zData[i][0] * scale / 1e9, totals.zData[i][1] * scale / 1e9,
                                     totals.zData[i][2] * scale / 1e9);
        std::cout << std::format("  without references, 512 KiB                         {:7.1f}  {:7.1f}  {:7.1f} GB\n",
                                 totals.zSlim[0] * scale / 1e9, totals.zSlim[1] * scale / 1e9, totals.zSlim[2] * scale / 1e9)
                  << "done in " << duration(secondsSince(started)) << std::endl;
        for (auto *d : dicts)
            ZSTD_freeCDict(d);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
