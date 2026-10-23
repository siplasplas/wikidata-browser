// wdsort: sorts the triple files of wdindex for the reverse index.
//
// wdsort [-j 20] [triples [sort-triples]]
//
// Each file NNN of the input directory (default triples) is read, sorted by
// (bit-reversed property, target type, target, item, rank), freed of repeated triples and written
// to the output directory (default sort-triples) under the same name; see
// triples.h. Files are taken from a queue by the threads, the largest first;
// a file is written as NNN.part and renamed when done, so a stopped run
// continues with the files not done.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "triples.h"

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    size_t threads = 20;
    std::vector<fs::path> dirs;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-j" && i + 1 < argc)
            threads = std::max(1, std::stoi(argv[++i]));
        else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: wdsort [-j 20] [triples [sort-triples]]\n";
            return 0;
        } else
            dirs.emplace_back(arg);
    }
    const fs::path input = dirs.size() > 0 ? dirs[0] : "triples";
    const fs::path output = dirs.size() > 1 ? dirs[1] : "sort-triples";
    try {
        fs::create_directories(output);
        struct File {
            std::string name;
            uint64_t size;
        };
        std::vector<File> todo;
        uint64_t total = 0;
        for (const auto &entry : fs::directory_iterator(input)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || name.ends_with(".part") || fs::exists(output / name))
                continue;
            if (entry.file_size() % sizeof(Triple))
                throw std::runtime_error(entry.path().string() + ": size is no multiple of 14");
            todo.push_back({name, entry.file_size()});
            total += entry.file_size();
        }
        std::ranges::sort(todo, std::greater{}, &File::size);
        std::cout << todo.size() << " files to sort, " << total / 1e9 << " GB, " << threads << " threads" << std::endl;

        std::atomic<size_t> next{0}, done{0};
        std::atomic<uint64_t> bytesDone{0}, triples{0}, repeated{0};
        std::mutex mutex;
        std::condition_variable wake;
        bool finished = false;
        std::string error;
        const auto started = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (size_t t = 0; t < std::min(threads, todo.size()); ++t)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < todo.size();) {
                    const auto &file = todo[i];
                    try {
                        std::vector<Triple> data(file.size / sizeof(Triple));
                        {
                            std::ifstream in(input / file.name, std::ios::binary);
                            in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(file.size));
                            if (!in)
                                throw std::runtime_error("cannot read " + (input / file.name).string());
                        }
                        std::ranges::sort(data, [](const Triple &a, const Triple &b) { return a < b; });
                        const auto end = std::unique(data.begin(), data.end());
                        repeated += static_cast<uint64_t>(data.end() - end);
                        data.erase(end, data.end());
                        {
                            std::ofstream out(output / (file.name + ".part"), std::ios::binary);
                            out.write(reinterpret_cast<const char *>(data.data()),
                                      static_cast<std::streamsize>(data.size() * sizeof(Triple)));
                            if (!out)
                                throw std::runtime_error("cannot write " + (output / file.name).string());
                        }
                        fs::rename(output / (file.name + ".part"), output / file.name);
                        triples += data.size();
                        bytesDone += file.size;
                        ++done;
                    } catch (const std::exception &e) {
                        std::lock_guard lock(mutex);
                        std::cerr << "\n" << file.name << ": " << e.what() << std::endl;
                        if (error.empty())
                            error = e.what();
                    }
                }
            });
        std::thread reporter([&] {
            std::unique_lock lock(mutex);
            while (!wake.wait_for(lock, std::chrono::seconds(10), [&] { return finished; })) {
                const double seconds = secondsSince(started), bytes = static_cast<double>(bytesDone), rate = bytes / seconds;
                std::cout << std::format("{} {:5.1f}%  {:.1f}/{:.1f} GB  files {}/{}  left {}", duration(seconds),
                                         total ? 100 * bytes / total : 100.0, bytes / 1e9, total / 1e9, done.load(),
                                         todo.size(), rate > 0 ? duration((total - bytes) / rate) : "?")
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
        std::cout << std::format("done in {}: {} files, {} triples, {} repeated left out", duration(secondsSince(started)),
                                 done.load(), triples.load(), repeated.load())
                  << std::endl;
        if (!error.empty())
            return 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
