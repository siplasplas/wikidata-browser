// wdbrowse: browser of Wikidata entities: JSONL files (e.g. wiktNeeded_1.jsonl
// of wiktparse, one entity per line; plain .jsonl, or .jsonl.bz2 with its
// .jsonl.idx) and the whole dump through the dense indexes of wddense.
//
// Left: the lines of the JSONL file with their titles, a filter and "go to
// line". Right: the entity as indented JSON (read-only qcodeedit, Kate
// highlighting and folding, the original text) and as a tree, where property
// ids may show their English names (View > Property names) and entity ids are
// links: click or F2 goes to the entity, Alt+Left / Alt+Right go back and
// forward. Go > Entity (Ctrl+G) reads Q42, P31 or L7 from the dump. Search >
// Find (Ctrl+F, F3, Shift+F3) in the JSON and the tree. F4 opens the JSON in
// Kate. The index directory (q.idx, p.idx, l.idx, <date>.lst) and the dump
// directory are set in Settings and kept in wdbrowse.ini next to the binary.
//
// wdbrowse [file.jsonl | file.jsonl.bz2 | Q42]
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTreeView>
#include <QVBoxLayout>
#include <qxfiledialog.h>

#include <bzlib.h>
#include <qce/CodeEdit.h>
#include <qce/CodeEditArea.h>
#include <qce/RuleBasedFoldingProvider.h>
#include <qce/RulesHighlighter.h>
#include <qce/SimpleTextDocument.h>
#include <qce/kate/KatePaths.h>
#include <qce/kate/KateTheme.h>
#include <qce/kate/KateXmlReader.h>
#include <qce/margins/FoldingGutter.h>
#include <qce/margins/LineNumberGutter.h>

#include <memory>
#include <nlohmann/json.hpp>

#include "Entities.h"
#include "FindBar.h"
#include "TreeFindBar.h"

using json = nlohmann::ordered_json;

namespace {

// Lines of a JSONL file with their titles
class Lines {
public:
    virtual ~Lines() = default;
    [[nodiscard]] virtual size_t count() const = 0;
    [[nodiscard]] virtual QString title(size_t i) const = 0;
    [[nodiscard]] virtual QByteArray line(size_t i) = 0;
};

// "id" with the English label (Q1084 noun), or "title"
QString titleOf(const QByteArray &line) {
    auto value = [&](const char *key) -> QString {
        int at = line.indexOf(key);
        if (at < 0)
            return {};
        at += static_cast<int>(strlen(key));
        return QString::fromUtf8(line.mid(at, line.indexOf('"', at) - at));
    };
    QString id = value("\"id\":\"");
    if (id.isEmpty())
        return value("\"title\":\"");
    if (auto english = value("\"en\":{\"language\":\"en\",\"value\":\""); !english.isEmpty())
        id += "  " + english;
    return id;
}

class PlainLines : public Lines {
public:
    explicit PlainLines(const QString &path) : file_(path) {
        if (!file_.open(QIODevice::ReadOnly))
            return;
        while (!file_.atEnd()) {
            offsets_.push_back(file_.pos());
            titles_.push_back(titleOf(file_.readLine()));
        }
    }
    [[nodiscard]] size_t count() const override { return offsets_.size(); }
    [[nodiscard]] QString title(size_t i) const override { return titles_[i]; }
    QByteArray line(size_t i) override {
        file_.seek(offsets_[i]);
        return file_.readLine();
    }

private:
    QFile file_;
    std::vector<qint64> offsets_;
    std::vector<QString> titles_;
};

// A .jsonl.bz2 of independent bz2 blocks with its index (colon-separated): per
// block "offset:count:uncompressed", then ":start:title" per entry (start in
// the uncompressed block), at the end EOF=<size>. An entry ends where the next
// one starts, the last at uncompressed; a block where the next one starts, the
// last at EOF.
class Bz2Lines : public Lines {
public:
    Bz2Lines(const QString &path, const QString &indexPath) : file_(path) {
        QFile index(indexPath);
        if (!file_.open(QIODevice::ReadOnly))
            return;
        if (!index.open(QIODevice::ReadOnly))
            return;
        quint64 end = 0;
        while (!index.atEnd()) {
            QByteArray l = index.readLine();
            l.chop(l.endsWith('\n') ? 1 : 0);
            if (l.startsWith('#'))
                continue;
            if (l.startsWith("EOF=")) {
                end = l.mid(4).toULongLong();
                continue;
            }
            // Titles may contain ':': only the first fields are split
            const auto parts = l.split(':');
            if (l.startsWith(':')) {
                if (parts.size() >= 3) {
                    starts_.push_back(parts[1].toUInt());
                    titles_.push_back(QString::fromUtf8(l.mid(parts[1].size() + 2)));
                }
            } else if (parts.size() == 3)
                blocks_.push_back({parts[0].toULongLong(), 0, parts[2].toUInt(), titles_.size()});
        }
        if (!end)
            end = static_cast<quint64>(file_.size());
        for (size_t b = 0; b < blocks_.size(); ++b)
            blocks_[b].compressed = static_cast<unsigned>((b + 1 < blocks_.size() ? blocks_[b + 1].offset : end) - blocks_[b].offset);
    }
    [[nodiscard]] size_t count() const override { return titles_.size(); }
    [[nodiscard]] QString title(size_t i) const override { return titles_[i]; }
    QByteArray line(size_t i) override {
        auto it = std::upper_bound(blocks_.begin(), blocks_.end(), i,
                                   [](size_t line, const Block &b) { return line < b.firstLine; });
        if (it == blocks_.begin() || i >= titles_.size())
            return {};
        const Block &b = *std::prev(it);
        if (cached_ != b.offset) {
            file_.seek(static_cast<qint64>(b.offset));
            QByteArray compressed = file_.read(b.compressed);
            text_ = QByteArray(static_cast<qsizetype>(b.uncompressed), '\0');
            unsigned length = b.uncompressed;
            if (BZ2_bzBuffToBuffDecompress(text_.data(), &length, compressed.data(), b.compressed, 0, 0) != BZ_OK) {
                cached_ = ~0ull;
                return {};
            }
            text_.truncate(length);
            cached_ = b.offset;
        }
        const bool last = it == blocks_.end() ? i + 1 == titles_.size() : i + 1 == it->firstLine;
        const qsizetype from = starts_[i], to = last ? text_.size() : starts_[i + 1];
        QByteArray entry = text_.mid(from, to - from);
        entry.chop(entry.endsWith('\n') ? 1 : 0);
        return entry;
    }

private:
    struct Block {
        quint64 offset;
        unsigned compressed, uncompressed;
        size_t firstLine;
    };
    QFile file_;
    std::vector<Block> blocks_;
    std::vector<QString> titles_;
    std::vector<unsigned> starts_; // of the entries in their uncompressed block
    QByteArray text_;              // the decompressed block at cached_
    quint64 cached_ = ~0ull;
};

QString scalar(const json &v) {
    return v.is_string() ? QString::fromStdString(v.get<std::string>()) : QString::fromStdString(v.dump());
}

// Role of tree items holding the id of an entity they link to
constexpr int linkRole = Qt::UserRole + 1;

// The entity a text names: an id (Q42, P31, L7) or a Wikidata URL (units, calendars, globes)
QString entityOf(const QString &text) {
    static const QRegularExpression id("^[QPL][1-9]\\d*$");
    static const QRegularExpression url("^https?://www\\.wikidata\\.org/entity/([QPL][1-9]\\d*)$");
    if (id.match(text).hasMatch())
        return text;
    if (const auto m = url.match(text); m.hasMatch())
        return m.captured(1);
    return {};
}

// A place shown: a line of the JSONL file or an entity of the dump
struct Location {
    int line = -1;
    QString entity;
    bool operator==(const Location &) const = default;
};

class Browser : public QMainWindow {
public:
    Browser() {
        auto *central = new QSplitter(this);
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        filter_ = new QLineEdit;
        filter_->setPlaceholderText("filter titles");
        lineBox_ = new QSpinBox;
        lineBox_->setPrefix("line ");
        lineBox_->setMinimum(1);
        status_ = new QLabel;
        list_ = new QListWidget;
        auto *top = new QHBoxLayout;
        top->addWidget(filter_);
        top->addWidget(lineBox_);
        leftLayout->addLayout(top);
        leftLayout->addWidget(list_);
        leftLayout->addWidget(status_);

        tabs_ = new QTabWidget;
        setupEditor(json_, "json.xml");
        tree_ = new QTreeView;
        model_ = new QStandardItemModel(this);
        tree_->setModel(model_);
        treePage_ = new QWidget;
        auto *treeLayout = new QVBoxLayout(treePage_);
        treeLayout->setContentsMargins(0, 0, 0, 0);
        treeLayout->setSpacing(0);
        treeFind_ = new TreeFindBar(treePage_, tree_);
        treeLayout->addWidget(treeFind_);
        treeLayout->addWidget(tree_);
        tabs_->addTab(json_.page, "JSON");
        tabs_->addTab(treePage_, "Tree");

        central->addWidget(left);
        central->addWidget(tabs_);
        central->setStretchFactor(1, 3);
        setCentralWidget(central);
        resize(1400, 900);

        connect(filter_, &QLineEdit::textChanged, this, [this] { fillList(); });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (auto *item = list_->item(row))
                go({item->data(Qt::UserRole).toInt(), {}});
        });
        connect(lineBox_, &QSpinBox::editingFinished, this, [this] { go({lineBox_->value() - 1, {}}); });
        // A link of the tree: a click on it, or F2 on its row
        connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex &index) {
            if (auto entity = index.data(linkRole).toString(); !entity.isEmpty())
                go({-1, entity});
        });
        // F4: the JSON, indented, in Kate itself (a read-only file in the temporary directory)
        connect(new QShortcut(QKeySequence(Qt::Key_F4), this), &QShortcut::activated, this, [this] { openInKate(); });

        auto *file = menuBar()->addMenu("&File");
        file->addAction("&Open...", QKeySequence::Open, this, [this] {
            if (auto path = choose(this, QFileInfo(path_).absolutePath()); !path.isEmpty())
                open(path);
        });
        file->addSeparator();
        file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

        auto *goMenu = menuBar()->addMenu("&Go");
        goMenu->addAction("&Entity...", QKeySequence(Qt::CTRL | Qt::Key_G), this, [this] {
            bool ok = false;
            const auto id = QInputDialog::getText(this, "Go to entity", "Q, P or L with a number (Q42, P31, L7):",
                                                  QLineEdit::Normal, lastAsked_, &ok);
            if (ok && !id.trimmed().isEmpty()) {
                lastAsked_ = id.trimmed().toUpper();
                go({-1, lastAsked_});
            }
        });
        goMenu->addAction("&Follow link", QKeySequence(Qt::Key_F2), this, [this] { followLink(); });
        goMenu->addSeparator();
        backAction_ = goMenu->addAction("&Back", QKeySequence(Qt::ALT | Qt::Key_Left), this, [this] { step(-1); });
        forwardAction_ = goMenu->addAction("&Forward", QKeySequence(Qt::ALT | Qt::Key_Right), this, [this] { step(1); });
        updateHistoryActions();

        auto *view = menuBar()->addMenu("&View");
        namesAction_ = view->addAction("&Property names in the tree");
        namesAction_->setCheckable(true);
        namesAction_->setChecked(settings().value("view/propertyNames", true).toBool());
        connect(namesAction_, &QAction::toggled, this, [this](bool on) {
            settings().setValue("view/propertyNames", on);
            fillTree();
        });

        auto *search = menuBar()->addMenu("&Search");
        search->addAction("&Find...", QKeySequence::Find, this, [this] {
            if (tabs_->currentWidget() == json_.page)
                json_.find->showSearch();
            else
                treeFind_->showSearch();
        });
        search->addAction("Find &Next", QKeySequence(Qt::Key_F3), this, [this] {
            if (tabs_->currentWidget() == json_.page)
                json_.find->findNext();
            else
                treeFind_->findNext();
        });
        search->addAction("Find &Previous", QKeySequence(Qt::SHIFT | Qt::Key_F3), this, [this] {
            if (tabs_->currentWidget() == json_.page)
                json_.find->findNext(true);
            else
                treeFind_->findNext(true);
        });

        auto *setup = menuBar()->addMenu("&Settings");
        setup->addAction("&Index directory (q.idx, p.idx, l.idx, .lst)...", this, [this] {
            auto dir = QxFileDialog::getExistingDirectory(this, "Directory of q.idx, p.idx, l.idx and <date>.lst",
                                                          settings().value("entities/index").toString());
            if (!dir.isEmpty()) {
                settings().setValue("entities/index", dir);
                loadEntities(true);
            }
        });
        setup->addAction("&Dump directory...", this, [this] {
            auto dir = QxFileDialog::getExistingDirectory(this, "Directory of the wikidatawiki-*-pages-articles-multistream*.bz2 parts",
                                                          settings().value("entities/dumps").toString());
            if (!dir.isEmpty()) {
                settings().setValue("entities/dumps", dir);
                loadEntities(true);
            }
        });
        loadEntities(false);
    }

    // A JSONL file chosen in the dialog, empty when cancelled
    static QString choose(QWidget *parent, const QString &dir = {}) {
        return QxFileDialog::getOpenFileName(parent, "JSONL file", dir, "JSONL (*.jsonl *.jsonl.bz2 *.json);;All (*)");
    }

    void open(const QString &path) {
        QString index = path;
        if (path.endsWith(".bz2"))
            index = path.left(path.size() - 4) + ".idx";
        if (path.endsWith(".bz2") && QFileInfo::exists(index))
            lines_ = std::make_unique<Bz2Lines>(path, index);
        else
            lines_ = std::make_unique<PlainLines>(path);
        path_ = path;
        history_.clear();
        position_ = -1;
        lineBox_->setMaximum(std::max<int>(1, static_cast<int>(lines_->count())));
        fillList();
        if (lines_->count() > 0)
            list_->setCurrentRow(0);
    }

    // Shows a place and records it in the history
    void go(const Location &location) {
        if (!display(location))
            return;
        if (position_ >= 0 && history_[position_] == location)
            return;
        history_.resize(position_ + 1); // the places after the current one are dropped
        history_.push_back(location);
        position_ = static_cast<int>(history_.size()) - 1;
        updateHistoryActions();
    }

private:
    // wdbrowse.ini next to the binary
    static QSettings &settings() {
        static QSettings s(QCoreApplication::applicationDirPath() + "/wdbrowse.ini", QSettings::IniFormat);
        return s;
    }

    void loadEntities(bool report) {
        const auto index = settings().value("entities/index").toString();
        if (index.isEmpty())
            return;
        QString error;
        if (!entities_.load(index, settings().value("entities/dumps").toString(), error)) {
            if (report)
                QMessageBox::warning(this, "wdbrowse", error);
            statusBar()->showMessage(error);
            return;
        }
        statusBar()->showMessage("indexes of " + index);
        fillTree();
    }

    // Shows a place without touching the history; false when it cannot be read
    bool display(const Location &location) {
        QByteArray raw;
        QString title;
        if (!location.entity.isEmpty()) {
            QString error;
            raw = entities_.json(location.entity, error);
            if (raw.isEmpty()) {
                statusBar()->showMessage(error);
                return false;
            }
            title = location.entity;
        } else {
            if (!lines_ || location.line < 0 || location.line >= static_cast<int>(lines_->count()))
                return false;
            raw = lines_->line(static_cast<size_t>(location.line));
            title = lines_->title(static_cast<size_t>(location.line));
            lineBox_->setValue(location.line + 1);
        }
        current_ = json::parse(raw.constData(), raw.constData() + raw.size(), nullptr, false);
        json_.doc->setText(current_.is_discarded()
                                   ? QString::fromUtf8(raw)
                                   : QString::fromStdString(current_.dump(2, ' ', false, json::error_handler_t::replace)));
        if (current_.is_discarded())
            current_ = json();
        currentTitle_ = titleOf(raw);
        if (currentTitle_.isEmpty())
            currentTitle_ = title;
        setWindowTitle(currentTitle_ + (location.entity.isEmpty() ? " — " + path_ : QString()) + " — wdbrowse");
        statusBar()->showMessage(location.entity.isEmpty() ? QString("line %1").arg(location.line + 1)
                                                           : location.entity + " from the dump");
        fillTree();
        return true;
    }

    void step(int direction) {
        const int target = position_ + direction;
        if (target < 0 || target >= static_cast<int>(history_.size()))
            return;
        if (display(history_[target])) {
            position_ = target;
            updateHistoryActions();
        }
    }

    void updateHistoryActions() {
        backAction_->setEnabled(position_ > 0);
        forwardAction_->setEnabled(position_ + 1 < static_cast<int>(history_.size()));
    }

    // F2: the link of the current row of the tree
    void followLink() {
        if (tabs_->currentWidget() != treePage_)
            return;
        const auto index = tree_->currentIndex();
        for (int column : {1, 0})
            if (auto entity = index.siblingAtColumn(column).data(linkRole).toString(); !entity.isEmpty()) {
                go({-1, entity});
                return;
            }
    }

    // A property id with its English name when names are shown: "P31  instance of"
    QString withName(const QString &id) const {
        if (!namesAction_->isChecked() || !id.startsWith('P'))
            return id;
        const auto name = entities_.propertyName(id.mid(1).toUInt());
        return name.isEmpty() ? id : id + "  " + name;
    }

    void markLink(QStandardItem *item, const QString &entity) {
        item->setData(entity, linkRole);
        item->setForeground(palette().color(QPalette::Link));
        auto font = item->font();
        font.setUnderline(true);
        item->setFont(font);
        item->setToolTip("go to " + entity + " (click or F2)");
    }

    // A readable label of an array element: claims (their value), labels, sitelinks
    QString label(const json &v, size_t i) const {
        QString text = QString("[%1]").arg(i);
        if (!v.is_object())
            return text + " " + scalar(v).left(120);
        if (v.contains("mainsnak")) { // a statement: its value
            const auto &snak = v["mainsnak"];
            if (snak.contains("datavalue") && snak["datavalue"].contains("value")) {
                const auto &value = snak["datavalue"]["value"];
                if (value.is_object() && value.contains("id"))
                    return text + " " + scalar(value["id"]);
                if (value.is_object() && value.contains("time"))
                    return text + " " + scalar(value["time"]);
                if (value.is_object() && value.contains("amount"))
                    return text + " " + scalar(value["amount"]);
                if (value.is_object() && value.contains("text"))
                    return text + " " + scalar(value["text"]);
                return text + " " + scalar(value).left(120);
            }
            return text + " " + scalar(snak.value("snaktype", json("?")));
        }
        for (const char *key : {"value", "id", "language"})
            if (v.contains(key))
                return text + " " + scalar(v[key]).left(120);
        return text;
    }

    void addItems(QStandardItem *parent, const json &v, const QString &name) {
        const QString keyEntity = entityOf(name);
        auto *keyItem = new QStandardItem(keyEntity.startsWith('P') && keyEntity == name ? withName(name) : name);
        auto *valueItem = new QStandardItem();
        keyItem->setEditable(false);
        valueItem->setEditable(false);
        if (keyEntity == name && !keyEntity.isEmpty())
            markLink(keyItem, keyEntity);
        parent->appendRow({keyItem, valueItem});
        if (v.is_object()) {
            for (const auto &[k, child] : v.items())
                addItems(keyItem, child, QString::fromStdString(k));
        } else if (v.is_array()) {
            valueItem->setText(QString("%1 items").arg(v.size()));
            for (size_t i = 0; i < v.size(); ++i)
                addItems(keyItem, v[i], label(v[i], i));
        } else {
            const auto text = scalar(v);
            const auto entity = v.is_string() ? entityOf(text) : QString();
            valueItem->setText((entity == text ? withName(text) : text).left(500));
            valueItem->setToolTip(text.left(4000));
            if (!entity.isEmpty())
                markLink(valueItem, entity);
        }
    }

    void fillTree() {
        model_->clear();
        model_->setHorizontalHeaderLabels({"Key", "Value"});
        if (current_.is_object() || current_.is_array())
            for (const auto &[k, child] : current_.items())
                addItems(model_->invisibleRootItem(), child, QString::fromStdString(k));
        tree_->expandToDepth(1);
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        treeFind_->reset();
    }

    Entities entities_;
    json current_;
    QString currentTitle_, lastAsked_;
    std::vector<Location> history_;
    int position_ = -1;
    QAction *backAction_ = nullptr, *forwardAction_ = nullptr, *namesAction_ = nullptr;
    std::unique_ptr<Lines> lines_;
    QString path_;
    QLineEdit *filter_;
    QSpinBox *lineBox_;
    QLabel *status_;
    QListWidget *list_;
    QTabWidget *tabs_;
    // A read-only qcodeedit view with Kate highlighting and folding
    struct CodeView {
        qce::SimpleTextDocument *doc = nullptr;
        qce::CodeEdit *edit = nullptr;
        std::unique_ptr<qce::LineNumberGutter> lineNumbers;
        std::unique_ptr<qce::FoldingGutter> foldGutter;
        std::unique_ptr<qce::RulesHighlighter> highlighter;
        std::unique_ptr<qce::RuleBasedFoldingProvider> foldProvider;
        QWidget *page = nullptr; // the tab: find bar space and editor
        FindBar *find = nullptr;
    };
    CodeView json_; // the entity as indented JSON
    QTreeView *tree_;
    QWidget *treePage_ = nullptr; // find bar and tree
    TreeFindBar *treeFind_ = nullptr;
    QStandardItemModel *model_;

    // Read-only qcodeedit with a Kate syntax and folding
    void setupEditor(CodeView &view, const QString &syntaxFile) {
        view.doc = new qce::SimpleTextDocument(this);
        view.edit = new qce::CodeEdit;
        view.edit->setDocument(view.doc);
        view.edit->area()->setReadOnly(true);
        view.lineNumbers = std::make_unique<qce::LineNumberGutter>(view.doc);
        view.lineNumbers->setFont(view.edit->area()->font());
        view.edit->addLeftMargin(view.lineNumbers.get());
        view.foldGutter = std::make_unique<qce::FoldingGutter>(&view.edit->area()->foldState(),
                                                               [edit = view.edit](int line) { edit->area()->toggleFoldAt(line); });
        view.edit->addLeftMargin(view.foldGutter.get());

        const auto theme = KateTheme::loadDefault();
        QPalette palette = view.edit->area()->palette();
        if (theme.editorBackground.isValid()) {
            palette.setColor(QPalette::Base, theme.editorBackground);
            palette.setColor(QPalette::Window, theme.editorBackground);
        }
        if (auto normal = theme.styles.constFind("Normal"); normal != theme.styles.constEnd() && normal->fg.isValid())
            palette.setColor(QPalette::Text, normal->fg);
        view.edit->setPalette(palette);
        view.edit->area()->setPalette(palette);
        view.edit->area()->viewport()->setPalette(palette);
        view.edit->setGutterColors({theme.iconBorder, theme.lineNumbers, theme.separator});

        const QString syntax = qce::kate::syntaxDir() + "/" + syntaxFile;
        view.highlighter = theme.isValid() ? KateXmlReader::load(syntax, theme) : KateXmlReader::load(syntax);
        if (view.highlighter) {
            view.edit->area()->setHighlighter(view.highlighter.get());
            view.foldProvider = std::make_unique<qce::RuleBasedFoldingProvider>(view.highlighter.get());
            for (const char *region : {"Brace1", "brace", "curly"})
                view.foldProvider->setPlaceholderFor(region, "{…}");
            for (const char *region : {"Bracket1", "bracket", "square"})
                view.foldProvider->setPlaceholderFor(region, "[…]");
            view.edit->area()->setFoldingProvider(view.foldProvider.get());
        }
        view.edit->area()->setWordWrap(true);
        view.page = new QWidget;
        auto *layout = new QVBoxLayout(view.page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        view.find = new FindBar(view.page, view.edit, view.doc);
        layout->addWidget(view.find->space());
        layout->addWidget(view.edit);
    }

    void fillList() {
        list_->clear();
        if (!lines_)
            return;
        const QString text = filter_->text();
        for (size_t i = 0; i < lines_->count(); ++i) {
            if (!text.isEmpty() && !lines_->title(i).contains(text, Qt::CaseInsensitive))
                continue;
            auto *item = new QListWidgetItem(QString("%1  %2").arg(i + 1).arg(lines_->title(i)));
            item->setData(Qt::UserRole, static_cast<int>(i));
            list_->addItem(item);
            if (list_->count() >= 200000)
                break;
        }
        status_->setText(QString("%1 lines, %2 shown").arg(lines_->count()).arg(list_->count()));
    }

    void openInKate() {
        const QString dir = QDir::tempPath() + "/wdbrowse";
        QDir().mkpath(dir);
        QString name = currentTitle_;
        name.replace('/', '_');
        const QString path = QString("%1/%2.json").arg(dir).arg(name.left(80));
        QFile file(path);
        file.setPermissions(QFileDevice::WriteOwner | QFileDevice::ReadOwner);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        file.write(json_.doc->toPlainText().toUtf8());
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        QProcess::startDetached("kate", {path});
    }
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    Browser browser;
    browser.show();
    if (argc > 1) {
        const auto arg = QString::fromLocal8Bit(argv[1]);
        if (!entityOf(arg.toUpper()).isEmpty())
            browser.go({-1, arg.toUpper()});
        else
            browser.open(arg);
    }
    return app.exec();
}
