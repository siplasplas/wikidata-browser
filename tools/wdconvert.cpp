// wdconvert: the Wikidata dump slimmed, in zstd frames with an index of them.
//
// wdconvert [-j 20] [--level 9] [--frame 524288] [--removed removed] [-o slim]
//           <dump directory> <date>.lst <Wikimedia index directory>
//
// For each dump part NNN of the .lst it writes, in the output directory:
//   items/NNN, properties/NNN, lexemes/NNN   one entity per line (JSON), in
//          independent zstd frames of about --frame bytes uncompressed (an
//          entity larger than that is a frame of its own), level --level, with
//          the dictionary of its kind (items.dict, properties.dict,
//          lexemes.dict); parts without properties or lexemes have no such files
//   items/NNN.idx, ...                        the index of the frames:
//          # wdconvert index: offset:count:uncompressed, then :start:id per entry; EOF=<size>
//          offset:count:uncompressed           a frame: offset in the file, entities, size uncompressed
//          :start:Q315                         an entity: start in the uncompressed frame, id
//          EOF=<size>                          size of the file
//   redirects/NNN                             merged entities: lines "Q1:Q2" (from, to)
// Slimmed: no hashes (snaks, references), no statement ids (also of forms and
// senses), no nextFormId/nextSenseId; references are kept. Items of the mask
// --removed (wdfilter: scholarly articles in no Wikipedia) are left out.
//
// The dictionaries are trained first on a sample of streams of the dump (from
// its own indexes: random streams for items, streams with properties and
// lexemes for theirs) and written; when they exist they are used again, so
// that a stopped run continues with the same ones. Parts are taken from a
// queue by the threads, the largest first; files are written as .part and
// renamed when the part is done (items/NNN.idx last), so a stopped run
// continues with the parts not done.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

#include <nlohmann/json.hpp>
#include <zdict.h>
#include <zstd.h>

#include "claims.h"
#include "dump.h"
#include "triples.h" // duration, secondsSince

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

enum Kind { items, properties, lexemes };
const char *const kindNames[3] = {"items", "properties", "lexemes"};

// Kind of an entity id, -1 for none
int kindOf(std::string_view id) {
    return id.starts_with('Q') ? items : id.starts_with('P') ? properties : id.starts_with('L') ? lexemes : -1;
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

// Statement ids of the claims of an entity, its forms and senses
void eraseStatementIds(json &holder) {
    if (const auto claims = holder.find("claims"); claims != holder.end() && claims->is_object())
        for (auto &[p, list] : claims->items())
            if (list.is_array())
                for (auto &statement : list)
                    if (statement.is_object())
                        statement.erase("id");
}

// The entity without what is only for editing
void slim(json &entity) {
    eraseHashes(entity);
    eraseStatementIds(entity);
    for (const char *part : {"forms", "senses"})
        if (const auto list = entity.find(part); list != entity.end() && list->is_array())
            for (auto &element : *list)
                if (element.is_object())
                    eraseStatementIds(element);
    entity.erase("nextFormId");
    entity.erase("nextSenseId");
}

// Bit q of the mask of removed items
struct Mask {
    std::string bits;
    bool has(uint32_t q) const { return q / 8 < bits.size() && (uint8_t(bits[q / 8]) >> (q % 8) & 1); }
};

// The slim JSON of a page, its id and kind; empty for pages that are no entity
// (a redirect sets redirectTo)
struct Entity {
    std::string id, json, redirectTo;
    int kind = -1;
};

Entity entityOf(std::string_view escaped, const Mask &removed) {
    Entity e;
    if (escaped.empty())
        return e;
    auto entity = json::parse(dump::unescape(escaped), nullptr, false);
    if (entity.is_discarded() || !entity.is_object())
        return e;
    // Redirect of a merged entity: {"entity":"Q1","redirect":"Q2"}
    if (entity.contains("redirect") && entity.contains("entity") && entity["redirect"].is_string() &&
        entity["entity"].is_string()) {
        e.id = entity["entity"].get<std::string>();
        e.redirectTo = entity["redirect"].get<std::string>();
        return e;
    }
    if (!entity.contains("id") || !entity["id"].is_string())
        return e;
    e.id = entity["id"].get<std::string>();
    e.kind = kindOf(e.id);
    if (e.kind < 0)
        return e;
    if (e.kind == items && removed.has(static_cast<uint32_t>(std::stoul(e.id.substr(1))))) {
        e.kind = -1;
        return e;
    }
    slim(entity);
    e.json = entity.dump(-1, ' ', false, json::error_handler_t::replace);
    return e;
}

// Frames of one kind of one part: data file and index
class Writer {
public:
    Writer(fs::path path, const ZSTD_CDict *dict, size_t frame) : path_(std::move(path)), dict_(dict), target_(frame) {}
    ~Writer() { ZSTD_freeCCtx(ctx_); }

    void add(const std::string &id, const std::string &line) {
        if (!data_.is_open()) {
            data_.open(part(path_), std::ios::binary);
            index_ << "# wdconvert index: offset:count:uncompressed, then :start:id per entry; EOF=<size>\n";
        }
        entries_ += std::format(":{}:{}\n", frame_.size(), id);
        ++count_;
        frame_ += line;
        frame_ += '\n';
        if (frame_.size() >= target_)
            flush();
    }
    // Returns whether files were written
    bool finish() {
        if (!data_.is_open())
            return false;
        flush();
        index_ << "EOF=" << offset_ << '\n';
        data_.close();
        std::ofstream(part(index())) << index_.str();
        if (!data_)
            throw std::runtime_error("cannot write " + path_.string());
        return true;
    }
    fs::path index() const { return fs::path(path_.string() + ".idx"); }
    static fs::path part(const fs::path &p) { return fs::path(p.string() + ".part"); }
    const fs::path &path() const { return path_; }
    uint64_t bytes() const { return offset_; }

private:
    void flush() {
        if (frame_.empty())
            return;
        out_.resize(ZSTD_compressBound(frame_.size()));
        const auto size = ZSTD_compress_usingCDict(ctx_, out_.data(), out_.size(), frame_.data(), frame_.size(), dict_);
        if (ZSTD_isError(size))
            throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(size));
        data_.write(out_.data(), static_cast<std::streamsize>(size));
        index_ << std::format("{}:{}:{}\n", offset_, count_, frame_.size()) << entries_;
        offset_ += size;
        frame_.clear();
        entries_.clear();
        count_ = 0;
    }

    fs::path path_;
    const ZSTD_CDict *dict_;
    size_t target_;
    ZSTD_CCtx *ctx_ = ZSTD_createCCtx();
    std::ofstream data_;
    std::ostringstream index_;
    std::string frame_, entries_, out_;
    size_t count_ = 0;
    uint64_t offset_ = 0;
};

// A zstd dictionary trained on samples (at most about 100 MB of them)
std::string trainDictionary(std::vector<std::string> &samples, size_t size, std::mt19937_64 &random) {
    std::ranges::shuffle(samples, random);
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
    if (!ZDICT_isError(made)) {
        dictionary.resize(made);
        return dictionary;
    }
    // Too few samples to train on: the samples themselves (a raw dictionary; none without samples)
    return buffer.substr(0, std::min(buffer.size(), size));
}

struct Progress {
    std::atomic<uint64_t> compressed{0}, entities[3] = {}, written[3] = {}, removed{0}, redirects{0};
    std::atomic<size_t> partsDone{0};
};

void convertPart(const dump::Part &part, const fs::path &dumps, const fs::path &output, const ZSTD_CDict *const dicts[3],
                 size_t frame, const Mask &removed, Progress &progress) {
    const auto name = std::format("{:03}", part.number);
    std::vector<std::unique_ptr<Writer>> writers;
    for (int k = 0; k < 3; ++k)
        writers.push_back(std::make_unique<Writer>(output / kindNames[k] / name, dicts[k], frame));
    std::ofstream redirects(Writer::part(output / "redirects" / name));

    std::ifstream in(dumps / part.name, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + (dumps / part.name).string());
    std::vector<char> buffer(1 << 20), decoded(1 << 22);
    std::string text;
    bz_stream bz{};
    BZ2_bzDecompressInit(&bz, 0, 0);
    auto pages = [&](bool all) {
        const auto used = dump::forEachPage(text, [&](std::string_view, std::string_view escaped) {
            auto e = entityOf(escaped, removed);
            if (!e.redirectTo.empty()) {
                redirects << e.id << ':' << e.redirectTo << '\n';
                ++progress.redirects;
            } else if (e.kind >= 0) {
                writers[e.kind]->add(e.id, e.json);
                ++progress.entities[e.kind];
            } else if (!e.id.empty() && kindOf(e.id) == items)
                ++progress.removed;
        });
        text.erase(0, all ? text.size() : used);
    };
    for (;;) {
        if (bz.avail_in == 0) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = static_cast<uint64_t>(in.gcount());
            if (got == 0)
                break;
            progress.compressed += got;
            bz.next_in = buffer.data();
            bz.avail_in = static_cast<unsigned>(got);
        }
        bz.next_out = decoded.data();
        bz.avail_out = static_cast<unsigned>(decoded.size());
        const int status = BZ2_bzDecompress(&bz);
        text.append(decoded.data(), decoded.size() - bz.avail_out);
        if (status == BZ_STREAM_END) {
            pages(false);
            char *rest = bz.next_in;
            const unsigned restSize = bz.avail_in;
            BZ2_bzDecompressEnd(&bz);
            bz = bz_stream{};
            BZ2_bzDecompressInit(&bz, 0, 0);
            bz.next_in = rest;
            bz.avail_in = restSize;
        } else if (status != BZ_OK)
            throw std::runtime_error(std::format("{}: bz2 error {}", part.name, status));
    }
    BZ2_bzDecompressEnd(&bz);
    pages(true);
    redirects.close();
    // Data and index files of the part; the index of items last: it marks the part done
    std::vector<int> written;
    for (int k = 0; k < 3; ++k)
        if (writers[k]->finish()) {
            written.push_back(k);
            progress.written[k] += writers[k]->bytes();
        }
    for (const int k : written)
        fs::rename(Writer::part(writers[k]->path()), writers[k]->path());
    fs::rename(Writer::part(output / "redirects" / name), output / "redirects" / name);
    for (const int k : written)
        if (k != items)
            fs::rename(Writer::part(writers[k]->index()), writers[k]->index());
    if (std::ranges::find(written, int(items)) != written.end())
        fs::rename(Writer::part(writers[items]->index()), writers[items]->index());
    else // a part without items: an empty index marks it done
        std::ofstream(output / kindNames[items] / (name + ".idx"))
                << "# wdconvert index: offset:count:uncompressed, then :start:id per entry; EOF=<size>\nEOF=0\n";
}

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20, frame = 512 << 10;
    int level = 9;
    fs::path removedFile = "removed", output = "slim";
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "--level" && i + 1 < argc)
            level = std::stoi(argv[++i]);
        else if (arg == "--frame" && i + 1 < argc)
            frame = std::stoul(argv[++i]);
        else if (arg == "--removed" && i + 1 < argc)
            removedFile = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            output = argv[++i];
        else
            positional.push_back(arg);
    }
    if (positional.size() != 3) {
        std::cerr << "usage: wdconvert [-j 20] [--level 9] [--frame 524288] [--removed removed] [-o slim]\n"
                     "                 <dump directory> <date>.lst <Wikimedia index directory>\n";
        return 1;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        const fs::path dumps = positional[0], indexes = positional[2];
        const auto parts = dump::readList(positional[1]);
        Mask removed;
        if (fs::exists(removedFile))
            removed.bits = dump::readFile(removedFile);
        else
            std::cout << "no " << removedFile.string() << ": no items left out" << std::endl;
        for (const char *dir : {"items", "properties", "lexemes", "redirects"})
            fs::create_directories(output / dir);

        // Dictionaries: made once, used again by a continued run
        std::string dictionaries[3];
        bool haveAll = true;
        for (int k = 0; k < 3; ++k)
            if (const auto file = output / std::format("{}.dict", kindNames[k]); fs::exists(file))
                dictionaries[k] = dump::readFile(file);
            else
                haveAll = false;
        if (!haveAll) {
            std::mt19937_64 random(1);
            // Streams to learn from: random ones (items), and of those with properties, lexemes
            struct Job {
                size_t part;
                dump::IndexedStream stream;
            };
            std::vector<Job> all, withProperties, withLexemes;
            for (size_t p = 0; p < parts.size(); ++p)
                for (const auto &s : dump::indexedStreams(indexes / dump::indexNameOf(parts[p].name), parts[p].size)) {
                    all.push_back({p, s});
                    if (s.properties)
                        withProperties.push_back({p, s});
                    if (s.lexemes)
                        withLexemes.push_back({p, s});
                }
            auto pick = [&](std::vector<Job> &jobs, size_t count) {
                std::ranges::shuffle(jobs, random);
                if (jobs.size() > count)
                    jobs.resize(count);
            };
            pick(all, 1500);
            pick(withProperties, 1000);
            pick(withLexemes, 1000);
            std::vector<Job> jobs = all;
            jobs.insert(jobs.end(), withProperties.begin(), withProperties.end());
            jobs.insert(jobs.end(), withLexemes.begin(), withLexemes.end());
            std::cout << std::format("training dictionaries on {} streams...", jobs.size()) << std::endl;
            std::vector<std::string> samples[3];
            std::mutex mutex;
            std::atomic<size_t> next{0};
            std::vector<std::thread> pool;
            for (size_t t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    std::vector<std::string> mine[3];
                    for (size_t i; (i = next++) < jobs.size();) {
                        const auto &job = jobs[i];
                        const auto text = dump::stream(dumps / parts[job.part].name, job.stream.offset, job.stream.length);
                        dump::forEachPage(text, [&](std::string_view, std::string_view escaped) {
                            auto e = entityOf(escaped, removed);
                            if (e.kind >= 0 && mine[e.kind].size() < 200000)
                                mine[e.kind].push_back(std::move(e.json));
                        });
                    }
                    std::lock_guard lock(mutex);
                    for (int k = 0; k < 3; ++k)
                        for (auto &s : mine[k])
                            samples[k].push_back(std::move(s));
                });
            for (auto &thread : pool)
                thread.join();
            for (int k = 0; k < 3; ++k) {
                if (!dictionaries[k].empty())
                    continue;
                dictionaries[k] = trainDictionary(samples[k], 110 << 10, random);
                std::ofstream(output / std::format("{}.dict", kindNames[k]), std::ios::binary) << dictionaries[k];
                std::cout << std::format("  {}.dict: {} B from {} samples", kindNames[k], dictionaries[k].size(),
                                         samples[k].size())
                          << std::endl;
            }
        }
        ZSTD_CDict *dicts[3];
        for (int k = 0; k < 3; ++k)
            dicts[k] = ZSTD_createCDict(dictionaries[k].data(), dictionaries[k].size(), level);

        // Parts not done yet, the largest first
        std::vector<dump::Part> todo;
        uint64_t total = 0;
        for (const auto &p : parts)
            if (!fs::exists(output / "items" / std::format("{:03}.idx", p.number))) {
                todo.push_back(p);
                total += p.size;
            }
        std::ranges::sort(todo, std::greater{}, &dump::Part::size);
        std::cout << std::format("{} of {} parts to convert, {:.1f} GB, {} threads, zstd level {}, frames of {} B ({})",
                                 todo.size(), parts.size(), total / 1e9, threads, level, frame, duration(secondsSince(started)))
                  << std::endl;

        Progress progress;
        std::atomic<size_t> next{0};
        std::mutex mutex;
        std::condition_variable wake;
        bool finished = false;
        std::string error;
        const auto convertStarted = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (size_t t = 0; t < std::min(threads, todo.size()); ++t)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < todo.size();) {
                    try {
                        convertPart(todo[i], dumps, output, dicts, frame, removed, progress);
                        ++progress.partsDone;
                    } catch (const std::exception &e) {
                        std::lock_guard lock(mutex);
                        std::cerr << "\n" << todo[i].name << ": " << e.what() << std::endl;
                        if (error.empty())
                            error = e.what();
                    }
                }
            });
        std::thread reporter([&] {
            std::unique_lock lock(mutex);
            while (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return finished; })) {
                const double seconds = secondsSince(convertStarted), done = double(progress.compressed),
                             rate = done / seconds;
                std::cout << std::format("{} {:5.1f}%  {:.1f}/{:.1f} GB  {:.0f} MB/s  parts {}/{}  Q {}  P {}  L {}  "
                                         "left out {}  left {}",
                                         duration(seconds), total ? 100 * done / total : 100.0, done / 1e9, total / 1e9,
                                         rate / 1e6, progress.partsDone.load(), todo.size(), progress.entities[0].load(),
                                         progress.entities[1].load(), progress.entities[2].load(), progress.removed.load(),
                                         rate > 0 ? duration((total - done) / rate) : "?")
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
        for (auto *d : dicts)
            ZSTD_freeCDict(d);
        std::cout << std::format("done in {}: {} parts; items {} ({:.2f} GB), properties {} ({:.3f} GB), lexemes {} "
                                 "({:.2f} GB); {} items left out, {} redirects",
                                 duration(secondsSince(started)), progress.partsDone.load(), progress.entities[0].load(),
                                 progress.written[0] / 1e9, progress.entities[1].load(), progress.written[1] / 1e9,
                                 progress.entities[2].load(), progress.written[2] / 1e9, progress.removed.load(),
                                 progress.redirects.load())
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
