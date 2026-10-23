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
#include <vector>

class Entities {
public:
    // Reads the indexes; false (with error) when the directory has none
    bool load(const QString &indexDir, const QString &dumpDir, QString &error);
    bool loaded() const { return !parts_.isEmpty(); }

    // English label of P<number> from p.idx, empty when unknown
    QString propertyName(uint32_t number) const;

    // The JSON of an entity ("Q42", "P31", "L7"); empty with error when absent
    QByteArray json(const QString &id, QString &error);

private:
    struct Entry {
        uint16_t part = 0;
        uint64_t offset = 0; // of the bz2 stream in the part
        uint32_t start = 0;  // of the page in the uncompressed stream
    };
    static Entry decode(const uchar *bytes);
    bool entry(QChar kind, uint32_t number, Entry &e) const;
    QByteArray stream(const QString &path, uint64_t offset, QString &error);

    QString dumpDir_;
    QStringList parts_;        // file names of the dump parts by number (.lst)
    std::unique_ptr<QFile> q_, l_;
    const uchar *qData_ = nullptr, *lData_ = nullptr;
    qint64 qSize_ = 0, lSize_ = 0;
    QByteArray p_;             // p.idx, small: read whole
    // The last decompressed stream
    QString cachedPath_;
    uint64_t cachedOffset_ = 0;
    QByteArray cachedText_;
};
