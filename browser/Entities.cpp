// Entities of a Wikidata dump read through the dense indexes of wddense
#include "Entities.h"

#include <QDir>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <bzlib.h>

namespace {

uint64_t little(const uchar *bytes, int size) {
    uint64_t value = 0;
    for (int i = size - 1; i >= 0; --i)
        value = value << 8 | bytes[i];
    return value;
}

} // namespace

Entities::Entry Entities::decode(const uchar *bytes) {
    return {static_cast<uint16_t>(little(bytes, 2)), little(bytes + 2, 6), static_cast<uint32_t>(little(bytes + 8, 4))};
}

bool Entities::load(const QString &indexDir, const QString &dumpDir, QString &error) {
    *this = Entities();
    dumpDir_ = dumpDir;
    QDir dir(indexDir);
    // The newest .lst: line NNN gives the file name of dump part NNN
    const auto lists = dir.entryList({"*.lst"}, QDir::Files, QDir::Name);
    if (lists.isEmpty()) {
        error = "no <date>.lst (wdlist) in " + indexDir;
        return false;
    }
    QFile list(dir.filePath(lists.last()));
    if (!list.open(QIODevice::ReadOnly)) {
        error = "cannot read " + list.fileName();
        return false;
    }
    QStringList parts;
    while (!list.atEnd()) {
        const auto line = QString::fromUtf8(list.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const auto fields = line.split(':');
        if (fields.size() < 3)
            continue;
        const int number = fields[0].toInt();
        while (parts.size() <= number)
            parts.append(QString());
        parts[number] = fields.mid(2).join(':');
    }
    for (auto [name, file, data, size] :
         {std::tuple{"q.idx", &q_, &qData_, &qSize_}, std::tuple{"l.idx", &l_, &lData_, &lSize_}}) {
        *file = std::make_unique<QFile>(dir.filePath(name));
        if (!(*file)->open(QIODevice::ReadOnly)) {
            error = "cannot read " + dir.filePath(name) + " (wddense)";
            return false;
        }
        *size = (*file)->size();
        *data = *size ? (*file)->map(0, *size) : nullptr;
    }
    QFile p(dir.filePath("p.idx"));
    if (!p.open(QIODevice::ReadOnly)) {
        error = "cannot read " + p.fileName() + " (wddense)";
        return false;
    }
    p_ = p.readAll();
    parts_ = parts;
    return true;
}

QString Entities::propertyName(uint32_t number) const {
    if (p_.size() < 4)
        return {};
    const auto *data = reinterpret_cast<const uchar *>(p_.constData());
    const auto count = static_cast<uint32_t>(little(data, 4));
    if (number >= count)
        return {};
    const auto *entry = data + 4 + number * 16;
    if (decode(entry).offset == 0)
        return {};
    const qsizetype names = 4 + qsizetype(count) * 16;
    const qsizetype at = names + qsizetype(little(entry + 12, 4));
    const qsizetype end = p_.indexOf('\0', at);
    return at < p_.size() && end > at ? QString::fromUtf8(p_.mid(at, end - at)) : QString();
}

bool Entities::entry(QChar kind, uint32_t number, Entry &e) const {
    const uchar *data = nullptr;
    if (kind == 'Q' || kind == 'L') {
        const auto *base = kind == 'Q' ? qData_ : lData_;
        const auto size = kind == 'Q' ? qSize_ : lSize_;
        if (!base || (qint64(number) + 1) * 12 > size)
            return false;
        data = base + qint64(number) * 12;
    } else {
        if (p_.size() < 4)
            return false;
        const auto *base = reinterpret_cast<const uchar *>(p_.constData());
        if (number >= little(base, 4))
            return false;
        data = base + 4 + number * 16;
    }
    e = decode(data);
    return e.offset != 0;
}

// The whole bz2 stream at offset, decompressed (one stream: about 100 pages)
QByteArray Entities::stream(const QString &path, uint64_t offset, QString &error) {
    if (path == cachedPath_ && offset == cachedOffset_)
        return cachedText_;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(offset))) {
        error = "cannot read " + path;
        return {};
    }
    bz_stream bz{};
    BZ2_bzDecompressInit(&bz, 0, 0);
    QByteArray text, in, out(1 << 22, '\0');
    int status = BZ_OK;
    while (status == BZ_OK) {
        if (bz.avail_in == 0) {
            in = file.read(1 << 20);
            if (in.isEmpty())
                break;
            bz.next_in = in.data();
            bz.avail_in = static_cast<unsigned>(in.size());
        }
        bz.next_out = out.data();
        bz.avail_out = static_cast<unsigned>(out.size());
        status = BZ2_bzDecompress(&bz);
        text.append(out.constData(), out.size() - bz.avail_out);
    }
    BZ2_bzDecompressEnd(&bz);
    if (status != BZ_STREAM_END) {
        error = QString("no bz2 stream at %1 of %2").arg(offset).arg(path);
        return {};
    }
    cachedPath_ = path;
    cachedOffset_ = offset;
    cachedText_ = text;
    return text;
}

QByteArray Entities::json(const QString &id, QString &error) {
    static const QRegularExpression pattern("^([QPL])(\\d+)$");
    const auto match = pattern.match(id.trimmed().toUpper());
    if (!match.hasMatch()) {
        error = "not an entity id (Q42, P31, L7): " + id;
        return {};
    }
    if (!loaded()) {
        error = "no indexes: Settings > Index directory";
        return {};
    }
    const QChar kind = match.captured(1)[0];
    Entry e;
    if (!entry(kind, match.captured(2).toUInt(), e)) {
        error = id + " is not in the indexes (absent, deleted or merged into another entity)";
        return {};
    }
    if (e.part >= parts_.size() || parts_[e.part].isEmpty()) {
        error = QString("no dump part %1 in the .lst").arg(e.part);
        return {};
    }
    const auto text = stream(QDir(dumpDir_).filePath(parts_[e.part]), e.offset, error);
    if (text.isEmpty())
        return {};
    const qsizetype end = text.indexOf("</page>", e.start);
    if (end < 0) {
        error = "no page at the indexed position";
        return {};
    }
    // The page's <text>, with the XML entities (&quot;) decoded
    QXmlStreamReader reader(text.mid(e.start, end + 7 - e.start));
    while (!reader.atEnd())
        if (reader.readNext() == QXmlStreamReader::StartElement && reader.name() == u"text")
            return reader.readElementText().toUtf8();
    error = "no <text> in the page" + (reader.hasError() ? ": " + reader.errorString() : QString());
    return {};
}
