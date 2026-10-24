#pragma once
// Entities of a Wikidata dump read through the dense indexes of wddense:
// q.idx, p.idx, l.idx and the <date>.lst of wdlist in one directory, the dump
// parts in another. An entity is its page's JSON, cut from one decompressed
// bz2 stream of the dump.
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <zstd.h>

class Entities {
public:
    // Reads the indexes; false (with error) when the directory has none
    bool load(const QString &indexDir, const QString &dumpDir, QString &error);
    bool loaded() const { return !parts_.isEmpty(); }

    // The slim files of wdconvert (slim/items/NNN, ..., q.idx, p.idx, l.idx of
    // wddense --slim, items.dict, ...): entities are read from them first, from
    // the dump when they are not there (items left out). After load().
    bool loadSlim(const QString &slimDir, QString &error);
    bool hasSlim() const { return !slimDir_.isEmpty(); }
    // Where the last entity of json() came from: "slim" or "dump"
    QString lastSource() const { return lastSource_; }

    // English label of P<number> from p.idx, empty when unknown
    QString propertyName(uint32_t number) const;

    // The JSON of an entity ("Q42", "P31", "L7"); empty with error when absent
    QByteArray json(const QString &id, QString &error);

    // Names of items (wdnames: names, names-by-q, q-by-name), when present
    bool hasNames() const { return names_ && byQ_ && byName_; }
    // The name of Q<number>, empty when it has none
    QString itemName(uint32_t number) const;
    // The range [first, last) of q-by-name whose names start with text (case and
    // diacritics ignored, fold.h); the Q of position i is byName(i)
    std::pair<uint64_t, uint64_t> namesStartingWith(const QString &text) const;
    uint32_t byName(uint64_t i) const;
    // The items whose names start with text, those with exactly that name first,
    // in both groups the most popular first (popularity of wddense: number of
    // Wikipedias with an article), else in the order of the names
    std::vector<uint32_t> rankedNames(const QString &text, size_t &exact) const;
    // Number of language Wikipedias with an article on Q<number>
    unsigned popularity(uint32_t number) const;

private:
    struct Entry {
        uint16_t part = 0;
        uint64_t offset = 0; // of the bz2 stream in the part
        uint32_t start = 0;  // of the page in the uncompressed stream
    };
    static Entry decode(const uchar *bytes);
    bool entry(QChar kind, uint32_t number, Entry &e) const;
    QByteArray stream(const QString &path, uint64_t offset, QString &error);
    // The slim entry of an entity, false when it is not in the slim files
    bool slimEntry(int kind, uint32_t number, Entry &e) const;
    // A zstd frame of a slim file, decompressed
    QByteArray frame(const QString &path, uint64_t offset, const ZSTD_DDict *dict, QString &error);

    struct DDictFree {
        void operator()(ZSTD_DDict *d) const { ZSTD_freeDDict(d); }
    };
    struct SlimKind {
        std::unique_ptr<QFile> index; // q.idx, p.idx (uint32 count, then 16-byte entries), l.idx
        const uchar *data = nullptr;
        qint64 size = 0;
        std::unique_ptr<ZSTD_DDict, DDictFree> dict;
    };
    QString slimDir_, lastSource_;
    SlimKind slim_[3]; // items, properties, lexemes
    QString cachedFramePath_;
    uint64_t cachedFrameOffset_ = 0;
    QByteArray cachedFrame_;

    QString dumpDir_;
    QStringList parts_;        // file names of the dump parts by number (.lst)
    std::unique_ptr<QFile> q_, l_;
    const uchar *qData_ = nullptr, *lData_ = nullptr;
    qint64 qSize_ = 0, lSize_ = 0;
    QByteArray p_;             // p.idx, small: read whole
    std::string_view nameAt(uint32_t number) const; // UTF-8, from the mapped names
    std::unique_ptr<QFile> namesFile_, byQFile_, byNameFile_, popularityFile_;
    const uint16_t *popularity_ = nullptr;
    qint64 popularitySize_ = 0;
    const char *names_ = nullptr;
    const uint32_t *byQ_ = nullptr, *byName_ = nullptr;
    qint64 namesSize_ = 0, byQCount_ = 0, byNameCount_ = 0;
    // The last decompressed stream
    QString cachedPath_;
    uint64_t cachedOffset_ = 0;
    QByteArray cachedText_;
};
