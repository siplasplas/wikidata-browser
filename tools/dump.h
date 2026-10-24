#pragma once
// Reading the Wikidata dump: parts of the .lst (wdlist), the dump's own
// indexes, bz2 streams, pages and their entity JSON.
#include <bzlib.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dump {

namespace fs = std::filesystem;

// &quot; &lt; &gt; &amp; &#039; of the XML text
inline std::string unescape(std::string_view s) {
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

struct Part {
    size_t number = 0;
    uint64_t size = 0;
    std::string name;
};

// Lines "NNN:  size:name" of a .lst
inline std::vector<Part> readList(const fs::path &file) {
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

// The dump's own index of a part:
// wikidatawiki-<date>-pages-articles-multistream<N>.xml-<range>.bz2 ->
// wikidatawiki-<date>-pages-articles-multistream-index<N>.txt-<range>.bz2
inline std::string indexNameOf(const std::string &partName) {
    static const std::regex pattern(R"((.*)-multistream(\d+)\.xml-(p\d+p\d+)\.bz2)");
    std::smatch m;
    if (!std::regex_match(partName, m, pattern))
        throw std::runtime_error("unexpected part name " + partName);
    return std::format("{}-multistream-index{}.txt-{}.bz2", m[1].str(), m[2].str(), m[3].str());
}

// Decompression of bz2 data of one or more streams
inline std::string bunzip(std::string_view compressed, bool oneStream = false) {
    std::string out;
    std::vector<char> buffer(1 << 22);
    size_t pos = 0;
    while (pos < compressed.size()) {
        bz_stream bz{};
        BZ2_bzDecompressInit(&bz, 0, 0);
        bz.next_in = const_cast<char *>(compressed.data() + pos);
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
            throw std::runtime_error("bad bz2 data");
        if (oneStream)
            break;
    }
    return out;
}

inline std::string readFile(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + file.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

// One bz2 stream of a file, decompressed
inline std::string stream(const fs::path &file, uint64_t offset, uint64_t length) {
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string compressed(length, '\0');
    in.read(compressed.data(), static_cast<std::streamsize>(length));
    compressed.resize(static_cast<size_t>(in.gcount()));
    return bunzip(compressed, true);
}

// A stream of the dump's index: offset and length in the part, and whether its
// pages include properties or lexemes (titles Property:P.., Lexeme:L..)
struct IndexedStream {
    uint64_t offset = 0, length = 0;
    bool properties = false, lexemes = false;
};

inline std::vector<IndexedStream> indexedStreams(const fs::path &indexFile, uint64_t partSize) {
    const auto text = bunzip(readFile(indexFile));
    std::vector<IndexedStream> streams;
    for (size_t pos = 0; pos < text.size();) {
        auto eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;
        const auto first = line.find(':'), second = line.find(':', first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos)
            continue;
        const auto offset = std::stoull(std::string(line.substr(0, first)));
        if (streams.empty() || streams.back().offset != offset)
            streams.push_back({offset});
        const auto title = line.substr(second + 1);
        streams.back().properties |= title.starts_with("Property:");
        streams.back().lexemes |= title.starts_with("Lexeme:");
    }
    for (size_t i = 0; i < streams.size(); ++i)
        streams[i].length = (i + 1 < streams.size() ? streams[i + 1].offset : partSize) - streams[i].offset;
    return streams;
}

// The pages of decompressed XML: title and the text (JSON, still escaped); the
// returned position is after the last whole page, so a partial page can wait
// for more data
inline size_t forEachPage(std::string_view text, const std::function<void(std::string_view title, std::string_view escaped)> &f) {
    size_t pos = 0;
    for (;;) {
        const auto page = text.find("<page>", pos);
        if (page == std::string_view::npos)
            return pos;
        const auto end = text.find("</page>", page);
        if (end == std::string_view::npos)
            return page;
        const auto body = text.substr(page, end - page);
        pos = end + 7;
        std::string_view title;
        if (const auto t = body.find("<title>"), te = body.find("</title>"); t != std::string_view::npos && te != std::string_view::npos)
            title = body.substr(t + 7, te - t - 7);
        const auto open = body.find("<text");
        const auto content = open == std::string_view::npos ? open : body.find('>', open);
        const auto close = body.find("</text>");
        if (content == std::string_view::npos || close == std::string_view::npos || body[content - 1] == '/')
            f(title, {});
        else
            f(title, body.substr(content + 1, close - content - 1));
    }
}

} // namespace dump
