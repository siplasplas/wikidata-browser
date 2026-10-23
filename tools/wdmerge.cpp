// wdmerge: merges the sorted triple files of wdsort into 256 index files.
//
// wdmerge [-j 20] [sort-triples [triple-index]]
//
// Output file NNN (000-255) holds the triples of all input files whose
// property % 256 = NNN, sorted by (bit-reversed property, target, item): "all
// items with P106 = Q36180" are one contiguous run of file 106, found by binary
// search. In every sorted input file these triples are one contiguous run (the
// top 8 bits of the reversed property), so an output file is made of a run of
// each input file, about 1/256 of it, merged. Input files are memory-mapped;
// threads make output files from a queue. A file is written as NNN.part and
// renamed when done, so a stopped run continues with the files not done.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "triples.h"

namespace fs = std::filesystem;

namespace {

// A read-only memory-mapped triple file
class Mapped {
public:
    explicit Mapped(const fs::path &file) {
        size_ = fs::file_size(file);
        if (size_ % sizeof(Triple))
            throw std::runtime_error(file.string() + ": size is no multiple of 12");
        if (size_ == 0)
            return;
        const int fd = ::open(file.c_str(), O_RDONLY);
        if (fd < 0)
            throw std::runtime_error("cannot open " + file.string());
        data_ = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (data_ == MAP_FAILED)
            throw std::runtime_error("cannot map " + file.string());
    }
    Mapped(const Mapped &) = delete;
    Mapped &operator=(const Mapped &) = delete;
    ~Mapped() {
        if (size_)
            ::munmap(data_, size_);
    }
    std::span<const Triple> triples() const {
        return {static_cast<const Triple *>(data_), size_ / sizeof(Triple)};
    }

private:
    void *data_ = nullptr;
    size_t size_ = 0;
};

// The run of a sorted file with property % 256 = bucket
std::span<const Triple> runOf(std::span<const Triple> sorted, uint32_t bucket) {
    const uint32_t top = reverseBits(bucket) & 0xFF000000u; // top 8 bits of reversed properties of the bucket
    auto key = [](const Triple &t) { return reverseBits(t.property) & 0xFF000000u; };
    const auto begin = std::ranges::lower_bound(sorted, top, {}, key);
    const auto end = std::ranges::upper_bound(begin, sorted.end(), top, {}, key);
    return {begin, end};
}

} // namespace

int main(int argc, char *argv[]) {
    size_t threads = 20;
    std::vector<fs::path> dirs;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: wdmerge [-j 20] [sort-triples [triple-index]]\n";
            return 0;
        } else
            dirs.emplace_back(arg);
    }
    const fs::path input = dirs.size() > 0 ? dirs[0] : "sort-triples";
    const fs::path output = dirs.size() > 1 ? dirs[1] : "triple-index";
    try {
        fs::create_directories(output);
        std::vector<std::unique_ptr<Mapped>> files;
        std::vector<std::string> names;
        for (const auto &entry : fs::directory_iterator(input))
            if (entry.is_regular_file() && !entry.path().filename().string().ends_with(".part"))
                names.push_back(entry.path().filename().string());
        std::ranges::sort(names);
        uint64_t total = 0;
        for (const auto &name : names) {
            files.push_back(std::make_unique<Mapped>(input / name));
            total += files.back()->triples().size();
        }
        std::vector<uint32_t> todo;
        for (uint32_t bucket = 0; bucket < 256; ++bucket)
            if (!fs::exists(output / std::format("{:03}", bucket)))
                todo.push_back(bucket);
        std::cout << files.size() << " sorted files, " << total << " triples; " << todo.size()
                  << " of 256 index files to do, " << threads << " threads" << std::endl;

        std::atomic<size_t> next{0}, done{0};
        std::atomic<uint64_t> written{0};
        std::mutex mutex;
        std::condition_variable wake;
        bool finished = false;
        std::string error;
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (size_t t = 0; t < std::min(threads, todo.size()); ++t)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < todo.size();) {
                    const auto bucket = todo[i];
                    const auto name = std::format("{:03}", bucket);
                    try {
                        // One run of each input file, merged through a heap of their heads
                        std::vector<std::span<const Triple>> runs;
                        for (const auto &file : files)
                            if (auto run = runOf(file->triples(), bucket); !run.empty())
                                runs.push_back(run);
                        auto later = [&](size_t a, size_t b) { return runs[b].front() < runs[a].front(); };
                        std::priority_queue<size_t, std::vector<size_t>, decltype(later)> heap(later);
                        for (size_t r = 0; r < runs.size(); ++r)
                            heap.push(r);
                        std::ofstream out(output / (name + ".part"), std::ios::binary);
                        std::vector<Triple> buffer;
                        buffer.reserve(1 << 20);
                        Triple last{};
                        bool any = false;
                        while (!heap.empty()) {
                            const auto r = heap.top();
                            heap.pop();
                            const Triple t = runs[r].front();
                            runs[r] = runs[r].subspan(1);
                            if (!runs[r].empty())
                                heap.push(r);
                            ++written;
                            // The same triple in two files (an item in two parts) is written once
                            if (any && last == t)
                                continue;
                            last = t;
                            any = true;
                            buffer.push_back(t);
                            if (buffer.size() == buffer.capacity()) {
                                out.write(reinterpret_cast<const char *>(buffer.data()),
                                          static_cast<std::streamsize>(buffer.size() * sizeof(Triple)));
                                buffer.clear();
                            }
                        }
                        out.write(reinterpret_cast<const char *>(buffer.data()),
                                  static_cast<std::streamsize>(buffer.size() * sizeof(Triple)));
                        if (!out)
                            throw std::runtime_error("cannot write " + (output / name).string());
                        out.close();
                        fs::rename(output / (name + ".part"), output / name);
                        ++done;
                    } catch (const std::exception &e) {
                        std::lock_guard lock(mutex);
                        std::cerr << "\n" << name << ": " << e.what() << std::endl;
                        if (error.empty())
                            error = e.what();
                    }
                }
            });
        std::thread reporter([&] {
            std::unique_lock lock(mutex);
            while (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return finished; })) {
                const double seconds = secondsSince(started), count = static_cast<double>(written),
                             rate = count / seconds;
                std::cout << std::format("{} {:5.1f}%  {:.0f}/{} M triples  files {}/{}  left {}", duration(seconds),
                                         total ? 100 * count / total : 100.0, count / 1e6, total / 1000000, done.load(),
                                         todo.size(), rate > 0 ? duration((total - count) / rate) : "?")
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
        std::cout << std::format("done in {}: {} index files", duration(secondsSince(started)), done.load()) << std::endl;
        if (!error.empty())
            return 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
