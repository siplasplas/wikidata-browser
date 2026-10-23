// wdlist: lists the parts of Wikidata dumps in a directory.
//
// wdlist [directory]      (default ".")
//
// Finds the files wikidatawiki-<date>-pages-articles-multistream<N>.xml-<range>.bz2
// (<date>: 8 digits, e.g. 20261001; there may be several dates) and writes, for
// each date, <date>.lst in the current directory: the files of that date in
// alphabetical order.
//
//   # 106:152345678901
//   000:  441223456:wikidatawiki-20261001-pages-articles-multistream1.xml-p1p441397.bz2
//   001:1234567890:wikidatawiki-20261001-pages-articles-multistream10.xml-p...bz2
//
// The first line: "# ", the number of files, ":", their total size in bytes.
// Then a line per file: its number (3 digits, from 000), ":", its size in bytes
// right-aligned to the longest size, ":", its name without the directory.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    if (argc > 2 || (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))) {
        std::cout << "usage: wdlist [directory]\n"
                     "  writes <date>.lst in the current directory for the files\n"
                     "  wikidatawiki-<date>-pages-articles-multistream*.xml-*.bz2 of the directory\n";
        return argc == 2 ? 0 : 1;
    }
    const fs::path dir = argc == 2 ? argv[1] : ".";
    try {
        static const std::regex part(R"(wikidatawiki-(\d{8})-pages-articles-multistream\d*\.xml-.*\.bz2)");
        std::map<std::string, std::vector<std::pair<std::string, uintmax_t>>> dates; // date -> name, size
        for (const auto &entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            std::smatch m;
            if (entry.is_regular_file() && std::regex_match(name, m, part))
                dates[m[1].str()].emplace_back(name, entry.file_size());
        }
        if (dates.empty()) {
            std::cerr << "no wikidatawiki-*-pages-articles-multistream*.xml-*.bz2 in " << dir.string() << std::endl;
            return 1;
        }
        for (auto &[date, files] : dates) {
            std::ranges::sort(files);
            uintmax_t total = 0;
            size_t width = 0;
            for (const auto &[name, size] : files) {
                total += size;
                width = std::max(width, std::to_string(size).size());
            }
            const auto list = date + ".lst";
            std::ofstream out(list);
            out << std::format("# {}:{}\n", files.size(), total);
            for (size_t i = 0; i < files.size(); ++i)
                out << std::format("{:03}:{:>{}}:{}\n", i, files[i].second, width, files[i].first);
            if (!out)
                throw std::runtime_error("cannot write " + list);
            std::cout << list << ": " << files.size() << " files, " << total << " bytes" << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
