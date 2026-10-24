// Entities of a Wikidata dump read through the dense indexes of wddense
#include "Entities.h"

#include <QDir>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <bzlib.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "fold.h"

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
    // Names of items, optional (wdnames)
    auto map = [&](const char *name, std::unique_ptr<QFile> &file, qint64 &size) -> const uchar * {
        file = std::make_unique<QFile>(dir.filePath(name));
        if (!file->open(QIODevice::ReadOnly) || file->size() == 0)
            return nullptr;
        size = file->size();
        return file->map(0, size);
    };
    qint64 bytes = 0;
    names_ = reinterpret_cast<const char *>(map("names", namesFile_, namesSize_));
    byQ_ = reinterpret_cast<const uint32_t *>(map("names-by-q", byQFile_, bytes));
    byQCount_ = bytes / 4;
    byName_ = reinterpret_cast<const uint32_t *>(map("q-by-name", byNameFile_, bytes));
    byNameCount_ = bytes / 4;
    popularity_ = reinterpret_cast<const uint16_t *>(map("popularity", popularityFile_, bytes));
    popularitySize_ = bytes / 2;
    return true;
}

std::string_view Entities::nameAt(uint32_t number) const {
    if (!hasNames() || number >= byQCount_)
        return {};
    const qint64 at = qint64(byQ_[number]) * 2;
    if (at <= 0 || at >= namesSize_)
        return {};
    const char *p = names_ + at;
    return {p, strnlen(p, size_t(namesSize_ - at))};
}

QString Entities::itemName(uint32_t number) const {
    const auto name = nameAt(number);
    return QString::fromUtf8(name.data(), qsizetype(name.size()));
}

uint32_t Entities::byName(uint64_t i) const {
    return hasNames() && qint64(i) < byNameCount_ ? byName_[i] : 0;
}

std::pair<uint64_t, uint64_t> Entities::namesStartingWith(const QString &text) const {
    if (!hasNames() || text.isEmpty())
        return {0, 0};
    const auto prefix = foldKey(text.toStdString());
    // q-by-name is sorted by the folded names: the names starting with prefix are one range
    auto key = [&](uint64_t i) { return foldKey(nameAt(byName_[i])); };
    uint64_t lo = 0, hi = uint64_t(byNameCount_);
    // first position whose key is not below prefix
    for (uint64_t a = 0, b = hi; a < b;) {
        const uint64_t m = a + (b - a) / 2;
        if (key(m) < prefix)
            a = m + 1;
        else
            b = m;
        lo = a;
    }
    // first position whose key, cut to the prefix length, is above it
    uint64_t end = lo;
    for (uint64_t a = lo, b = hi; a < b;) {
        const uint64_t m = a + (b - a) / 2;
        if (key(m).compare(0, prefix.size(), prefix) <= 0)
            a = m + 1;
        else
            b = m;
        end = a;
    }
    return {lo, end};
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

bool Entities::loadSlim(const QString &slimDir, QString &error) {
    slimDir_.clear();
    QDir dir(slimDir);
    const char *indexes[3] = {"q.idx", "p.idx", "l.idx"}, *dicts[3] = {"items.dict", "properties.dict", "lexemes.dict"};
    SlimKind kinds[3];
    for (int k = 0; k < 3; ++k) {
        kinds[k].index = std::make_unique<QFile>(dir.filePath(indexes[k]));
        if (!kinds[k].index->open(QIODevice::ReadOnly) || kinds[k].index->size() == 0) {
            error = "cannot read " + dir.filePath(indexes[k]) + " (wddense --slim)";
            return false;
        }
        kinds[k].size = kinds[k].index->size();
        kinds[k].data = kinds[k].index->map(0, kinds[k].size);
        QFile dict(dir.filePath(dicts[k]));
        if (!dict.open(QIODevice::ReadOnly)) {
            error = "cannot read " + dict.fileName() + " (wdconvert)";
            return false;
        }
        const auto bytes = dict.readAll();
        kinds[k].dict.reset(ZSTD_createDDict(bytes.constData(), size_t(bytes.size())));
    }
    for (int k = 0; k < 3; ++k)
        slim_[k] = std::move(kinds[k]);
    slimDir_ = slimDir;
    return true;
}

bool Entities::slimEntry(int kind, uint32_t number, Entry &e) const {
    const auto &k = slim_[kind];
    if (!k.data)
        return false;
    const uchar *at = nullptr;
    if (kind == 1) { // p.idx: uint32 count, then 16-byte entries
        if (k.size < 4 || number >= little(k.data, 4))
            return false;
        at = k.data + 4 + qint64(number) * 16;
    } else {
        if ((qint64(number) + 1) * 12 > k.size)
            return false;
        at = k.data + qint64(number) * 12;
    }
    e = decode(at);
    return e.offset != 0 || e.start != 0;
}

QByteArray Entities::frame(const QString &path, uint64_t offset, const ZSTD_DDict *dict, QString &error) {
    if (path == cachedFramePath_ && offset == cachedFrameOffset_)
        return cachedFrame_;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(offset))) {
        error = "cannot read " + path;
        return {};
    }
    // Streaming decompression to the end of the frame: its length is not needed
    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx *)> ctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
    ZSTD_DCtx_refDDict(ctx.get(), dict);
    QByteArray text, in, out(qsizetype(ZSTD_DStreamOutSize()), '\0');
    for (size_t left = 1; left != 0;) {
        in = file.read(1 << 20);
        if (in.isEmpty()) {
            error = QString("zstd frame at %1 of %2 ends early").arg(offset).arg(path);
            return {};
        }
        ZSTD_inBuffer input{in.constData(), size_t(in.size()), 0};
        while (input.pos < input.size && left != 0) {
            ZSTD_outBuffer output{out.data(), size_t(out.size()), 0};
            left = ZSTD_decompressStream(ctx.get(), &output, &input);
            if (ZSTD_isError(left)) {
                error = QString("zstd: %1 at %2 of %3").arg(ZSTD_getErrorName(left)).arg(offset).arg(path);
                return {};
            }
            text.append(out.constData(), qsizetype(output.pos));
        }
    }
    cachedFramePath_ = path;
    cachedFrameOffset_ = offset;
    cachedFrame_ = text;
    return text;
}

namespace {

// (kind << 32 | number) of an id: Q 0, P 1, L 2; ~0 for no id
uint64_t keyOf(QStringView id) {
    if (id.size() < 2)
        return ~0ull;
    const int kind = id[0] == 'Q' ? 0 : id[0] == 'P' ? 1 : id[0] == 'L' ? 2 : -1;
    bool ok = false;
    const auto number = id.mid(1).toUInt(&ok);
    return kind < 0 || !ok ? ~0ull : uint64_t(kind) << 32 | number;
}

QString idOf(uint64_t key) {
    return QString("%1%2").arg(QChar("QPL"[key >> 32])).arg(uint32_t(key));
}

} // namespace

QString Entities::redirectOf(const QString &id) {
    if (!redirectsRead_) {
        redirectsRead_ = true;
        if (hasSlim()) {
            QDir dir(QDir(slimDir_).filePath("redirects"));
            for (const auto &name : dir.entryList(QDir::Files, QDir::Name)) {
                QFile file(dir.filePath(name));
                if (!file.open(QIODevice::ReadOnly))
                    continue;
                const auto text = file.readAll();
                for (const auto &line : text.split('\n')) {
                    const auto colon = line.indexOf(':');
                    if (colon < 0)
                        continue;
                    const auto from = keyOf(QString::fromLatin1(line.left(colon))),
                               to = keyOf(QString::fromLatin1(line.mid(colon + 1)));
                    if (from != ~0ull && to != ~0ull)
                        redirects_.emplace_back(from, to);
                }
            }
            std::ranges::sort(redirects_);
        }
    }
    const auto key = keyOf(id);
    const auto it = std::ranges::lower_bound(redirects_, std::pair{key, uint64_t(0)});
    return it != redirects_.end() && it->first == key ? idOf(it->second) : QString();
}

// The entity, or the one it was merged into (chains of redirects followed)
QByteArray Entities::json(const QString &id, QString &error) {
    lastRedirect_.clear();
    auto result = read(id, error);
    QString current = id.trimmed().toUpper();
    for (int step = 0; result.isEmpty() && step < 5; ++step) {
        const auto target = redirectOf(current);
        if (target.isEmpty())
            break;
        current = target;
        QString ignored;
        result = read(current, ignored);
        if (!result.isEmpty()) {
            lastRedirect_ = current;
            error.clear();
        }
    }
    return result;
}

QByteArray Entities::read(const QString &id, QString &error) {
    static const QRegularExpression pattern("^([QPL])(\\d+)$");
    const auto match = pattern.match(id.trimmed().toUpper());
    if (!match.hasMatch()) {
        error = "not an entity id (Q42, P31, L7): " + id;
        return {};
    }
    const QChar kind = match.captured(1)[0];
    // The slim files first
    if (hasSlim()) {
        const int k = kind == 'Q' ? 0 : kind == 'P' ? 1 : 2;
        static const char *dirs[3] = {"items", "properties", "lexemes"};
        Entry s;
        if (slimEntry(k, match.captured(2).toUInt(), s)) {
            const auto text = frame(QDir(slimDir_).filePath(QString("%1/%2").arg(dirs[k]).arg(s.part, 3, 10, QChar('0'))),
                                    s.offset, slim_[k].dict.get(), error);
            if (text.isEmpty())
                return {};
            const qsizetype end = text.indexOf('\n', s.start);
            lastSource_ = "slim";
            return text.mid(s.start, (end < 0 ? text.size() : end) - s.start);
        }
    }
    if (!loaded()) {
        error = hasSlim() ? id + " is not in the slim files, and no indexes of the dump: Settings > Index directory"
                          : "no indexes: Settings > Index directory";
        return {};
    }
    lastSource_ = "dump";
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

unsigned Entities::popularity(uint32_t number) const {
    return popularity_ && qint64(number) < popularitySize_ ? popularity_[number] : 0;
}

std::vector<uint32_t> Entities::rankedNames(const QString &text, size_t &exact) const {
    exact = 0;
    const auto [lo, hi] = namesStartingWith(text);
    if (lo == hi)
        return {};
    // The exact names are the first of the range (equal keys sort first)
    const auto key = foldKey(text.toStdString());
    uint64_t exactEnd = lo;
    for (uint64_t a = lo, b = hi; a < b;) {
        const uint64_t m = a + (b - a) / 2;
        if (foldKey(nameAt(byName_[m])) == key)
            a = m + 1;
        else
            b = m;
        exactEnd = a;
    }
    exact = size_t(exactEnd - lo);
    std::vector<uint32_t> result;
    result.reserve(size_t(hi - lo));
    // Each group by popularity, the most popular first: counting sort by the byte (stable)
    for (const auto [from, to] : {std::pair{lo, exactEnd}, std::pair{exactEnd, hi}}) {
        if (!popularity_) {
            for (uint64_t i = from; i < to; ++i)
                result.push_back(byName_[i]);
            continue;
        }
        // buckets 0..65535 by 65535 - popularity: the most popular first
        static thread_local std::vector<size_t> starts;
        starts.assign(65537, 0);
        for (uint64_t i = from; i < to; ++i)
            ++starts[65536 - popularity(byName_[i])];
        for (size_t b = 1; b < starts.size(); ++b)
            starts[b] += starts[b - 1];
        std::vector<uint32_t> group(size_t(to - from));
        for (uint64_t i = to; i-- > from;) {
            const auto q = byName_[i];
            group[--starts[65536 - popularity(q)]] = q;
        }
        result.insert(result.end(), group.begin(), group.end());
    }
    return result;
}
