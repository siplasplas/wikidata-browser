// Search bar of a read-only qcodeedit view, ported from qceditor's FindBar
#include "FindBar.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <qce/CodeEdit.h>
#include <qce/CodeEditArea.h>
#include <qce/ExtraSelection.h>
#include <qce/ITextDocument.h>
#include <qce/WordBoundary.h>

FindBar::FindBar(QWidget *host, qce::CodeEdit *edit, qce::ITextDocument *document)
    : QWidget(host), m_host(host), m_edit(edit), m_document(document) {
    setObjectName("findBar");
    // Not in the host's layout: the bar lies over the top lines of the editor,
    // or over m_space when it pushes them down (see reveal()).
    setAutoFillBackground(true);
    m_space = new QWidget(host);
    m_space->hide();
    edit->area()->installEventFilter(this);
    edit->installEventFilter(this);
    auto *scrollBar = edit->area()->verticalScrollBar();
    connect(scrollBar, &QScrollBar::rangeChanged, this, &FindBar::place);
    // Scrolled by hand: the first lines are shown below the bar, the others under it
    connect(scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_placing && !isHidden())
            setCovering(value > 0);
    });
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(5);
    auto button = [this](const QString &text, const QString &tip, bool toggle = false) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(toggle);
        return b;
    };
    auto *field = new QWidget(this);
    field->setObjectName("findField");
    field->setStyleSheet("#findField { border: 1px solid palette(mid); border-radius: 3px; background: palette(base); }");
    auto *fieldLayout = new QHBoxLayout(field);
    fieldLayout->setContentsMargins(5, 1, 3, 1);
    fieldLayout->setSpacing(1);
    fieldLayout->addWidget(new QLabel(QString::fromUtf8("⌕"), field));
    m_query = new QLineEdit(field);
    m_query->setFrame(false);
    m_query->setClearButtonEnabled(true);
    m_query->setPlaceholderText(tr("Search"));
    fieldLayout->addWidget(m_query, 1);
    m_wrap = button(QString::fromUtf8("↩"), tr("Wrap around"), true);
    m_wrap->setChecked(true);
    m_case = button("Cc", tr("Match case"), true);
    m_words = button("W", tr("Whole words"), true);
    m_regex = button(".*", tr("Regular expression"), true);
    for (auto *b : {m_wrap, m_case, m_words, m_regex})
        fieldLayout->addWidget(b);
    field->setMinimumWidth(260);
    field->setMaximumWidth(360);
    layout->addWidget(field, 1);
    m_count = new QLabel(this);
    m_count->setMinimumWidth(65);
    layout->addWidget(m_count);
    m_previous = button(QString(), tr("Previous match (Shift+F3)"));
    m_previous->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_next = button(QString(), tr("Next match (F3)"));
    m_next->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    layout->addWidget(m_previous);
    layout->addWidget(m_next);
    layout->addStretch();
    auto *close = button(QString(), tr("Close search (Escape)"));
    close->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    layout->addWidget(close);
    connect(close, &QToolButton::clicked, this, &FindBar::closeSearch);
    connect(m_previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(m_next, &QToolButton::clicked, this, [this] { findNext(); });
    connect(m_query, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(m_query, &QLineEdit::returnPressed, this, [this] { findNext(); });
    for (auto *b : {m_case, m_words, m_regex})
        connect(b, &QToolButton::toggled, this, [this] { rebuild(); });
    m_escape = new QShortcut(QKeySequence(Qt::Key_Escape), host);
    m_escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(m_escape, &QShortcut::activated, this, &FindBar::closeSearch);
    // Only while the bar is shown, so Escape otherwise reaches the window
    m_escape->setEnabled(false);
    auto *backwards = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return), m_query);
    connect(backwards, &QShortcut::activated, this, [this] { findNext(true); });
    connect(document, &qce::ITextDocument::linesChanged, this, [this] { rebuild(false); });
    connect(document, &qce::ITextDocument::linesInserted, this, [this] { rebuild(false); });
    connect(document, &qce::ITextDocument::linesRemoved, this, [this] { rebuild(false); });
    connect(document, &qce::ITextDocument::documentReset, this, [this] { rebuild(false); });
    hide();
}

void FindBar::showEvent(QShowEvent *event) {
    m_escape->setEnabled(true);
    QWidget::showEvent(event);
    // Until a result says otherwise: pushed down at the top of the text, covering below it
    m_covering = m_edit->area()->verticalScrollBar()->value() > 0;
    place();
}

void FindBar::hideEvent(QHideEvent *event) {
    m_escape->setEnabled(false);
    m_space->hide();
    QWidget::hideEvent(event);
}

bool FindBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_edit->area() && event->type() == QEvent::PaletteChange)
        updateHighlights();
    if (watched == m_edit && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        place();
    return QWidget::eventFilter(watched, event);
}

// Shows the bar over the top of the editor, or above it with the lines moved
// down (m_space in the host's layout)
void FindBar::place() {
    if (isHidden() || m_placing)
        return;
    m_placing = true;
    const int height = sizeHint().height();
    m_space->setFixedHeight(height);
    m_space->setVisible(!m_covering);
    if (auto *layout = m_host->layout())
        layout->activate();
    QRect rect = m_edit->geometry();
    auto *scrollBar = m_edit->area()->verticalScrollBar();
    if (m_covering) {
        // The scroll bar stays usable beside the bar
        if (scrollBar->isVisible())
            rect.setWidth(rect.width() - scrollBar->width());
        rect.setHeight(height);
    } else
        rect = QRect(rect.left(), m_space->y(), rect.width(), height);
    setGeometry(rect);
    raise();
    m_placing = false;
}

void FindBar::setCovering(bool covering) {
    if (m_covering == covering)
        return;
    m_covering = covering;
    place();
}

// The bar covers the top of the editor unless the row (visual, as the scroll
// bar counts) is among the first rows of the text: then it moves them down
void FindBar::reveal(int row) {
    if (isHidden())
        return;
    auto *area = m_edit->area();
    auto *scrollBar = area->verticalScrollBar();
    const int lineHeight = area->viewportState().lineHeight;
    const int viewportTop = area->viewport()->mapTo(m_edit, QPoint()).y();
    const int covered = lineHeight <= 0 ? 0 : std::max(0, (sizeHint().height() - viewportTop + lineHeight - 1) / lineHeight);
    const int first = scrollBar->value();
    bool covering = true;
    if (row >= first && row - first < covered) {
        covering = row >= covered;
        m_placing = true;
        scrollBar->setValue(covering ? row - covered : 0);
        m_placing = false;
    }
    setCovering(covering);
}

// Unfolds the folds hiding a range (as qceditor's EditorTab::revealRange)
void FindBar::revealRange(qce::TextCursor start, qce::TextCursor end) {
    auto *area = m_edit->area();
    auto &folds = area->foldState();
    int refreshLine = -1;
    const auto &regions = folds.regions();
    for (int i = 0; i < regions.size(); ++i) {
        if (!folds.isCollapsed(i))
            continue;
        const auto &region = regions[i];
        const qce::TextCursor hiddenStart{region.startLine, region.startColumn};
        const bool overlaps = start.line <= region.endLine && (start == end ? hiddenStart <= start : hiddenStart < end);
        if (!overlaps)
            continue;
        folds.setCollapsed(i, false);
        refreshLine = region.startLine;
    }
    if (refreshLine >= 0) {
        // The public toggle refreshes layout, scroll bars and the viewport; it
        // addresses the first region on a line, so that one is inverted first
        const int first = folds.regionStartingAt(refreshLine);
        folds.setCollapsed(first, !folds.isCollapsed(first));
        area->toggleFoldAt(refreshLine);
    }
}

void FindBar::showSearch() {
    auto *area = m_edit->area();
    const auto selection = area->selectedText();
    // The selected text is searched for, unless it is the current result
    const bool selectedResult = !isHidden() && m_current >= 0 &&
                                offsetFor(area->selectionStart()) == m_matches[m_current].start &&
                                offsetFor(area->selectionEnd()) == m_matches[m_current].end;
    show();
    if (!selectedResult && !selection.isEmpty() && !selection.contains('\n'))
        m_query->setText(selection);
    rebuild();
    if (m_current < 0)
        reveal(area->visualRow(area->cursorPosition()));
    m_query->setFocus();
    m_query->selectAll();
}

int FindBar::offsetFor(qce::TextCursor cursor) const {
    int offset = cursor.column;
    for (int line = 0; line < cursor.line; ++line)
        offset += m_document->lineAt(line).size() + 1;
    return offset;
}

int FindBar::cursorOffset() const {
    auto *area = m_edit->area();
    return offsetFor(area->hasSelection() ? area->selectionStart() : area->cursorPosition());
}

qce::TextCursor FindBar::positionFor(int offset) const {
    const int line = int(std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), offset) - m_lineStarts.begin()) - 1;
    return line < 0 ? qce::TextCursor{} : qce::TextCursor{line, offset - m_lineStarts[line]};
}

void FindBar::rebuild(bool moveCursor, bool whileHidden) {
    m_matches.clear();
    m_lineStarts.clear();
    m_current = -1;
    QString text;
    for (int i = 0; i < m_document->lineCount(); ++i) {
        m_lineStarts.append(int(text.size()));
        text += m_document->lineAt(i);
        text += '\n';
    }
    m_query->setToolTip(QString());
    if ((!isHidden() || whileHidden) && !m_query->text().isEmpty()) {
        QString pattern = m_regex->isChecked() ? m_query->text() : QRegularExpression::escape(m_query->text());
        if (m_words->isChecked())
            pattern = qce::words::wholeWordPattern(pattern);
        auto options = QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
        if (!m_case->isChecked())
            options |= QRegularExpression::CaseInsensitiveOption;
        QRegularExpression expression(pattern, options);
        if (!expression.isValid()) {
            m_count->setText(tr("Invalid pattern"));
            m_query->setToolTip(expression.errorString());
            m_next->setEnabled(false);
            m_previous->setEnabled(false);
            updateHighlights();
            return;
        }
        for (auto results = expression.globalMatch(text); results.hasNext();) {
            const auto result = results.next();
            m_matches.append({int(result.capturedStart()), int(result.capturedEnd())});
        }
        if (!m_matches.isEmpty()) {
            const int cursor = cursorOffset();
            auto it = std::lower_bound(m_matches.begin(), m_matches.end(), cursor,
                                       [](const Match &match, int pos) { return match.start < pos; });
            m_current = it == m_matches.end() ? 0 : int(it - m_matches.begin());
        }
    }
    if (m_current >= 0 && moveCursor)
        activate(m_current);
    else
        updateHighlights();
}

void FindBar::activate(int index) {
    m_current = index;
    const auto match = m_matches[index];
    auto *area = m_edit->area();
    const auto start = positionFor(match.start);
    const auto end = positionFor(match.end);
    revealRange(start, end);
    area->setSelection(start, end);
    reveal(area->visualRow(start));
    updateHighlights();
}

void FindBar::findNext(bool backwards) {
    if (isHidden()) {
        // F3 and Shift+F3 go to the next or previous result of the last query without showing the bar
        rebuild(false, true);
        auto *area = m_edit->area();
        const int from = offsetFor(backwards ? area->selectionStart() : area->selectionEnd());
        const int after = int(std::lower_bound(m_matches.begin(), m_matches.end(), from,
                                               [](const Match &match, int pos) { return match.start < pos; }) -
                              m_matches.begin());
        int next = backwards ? after - 1 : after;
        if (!m_matches.isEmpty() && (next < 0 || next >= m_matches.size()) && m_wrap->isChecked())
            next = backwards ? int(m_matches.size()) - 1 : 0;
        if (next < 0 || next >= m_matches.size()) {
            showNotFound();
            return;
        }
        activate(next);
        return;
    }
    if (m_matches.isEmpty()) {
        showNotFound();
        return;
    }
    int next = m_current + (backwards ? -1 : 1);
    if (next < 0 || next >= m_matches.size()) {
        if (!m_wrap->isChecked())
            return;
        next = backwards ? int(m_matches.size()) - 1 : 0;
    }
    activate(next);
}

void FindBar::showNotFound() {
    auto *area = m_edit->area();
    const QRect caret = area->inputMethodQuery(Qt::ImCursorRectangle).toRect();
    QToolTip::showText(area->mapToGlobal(caret.bottomLeft()), tr("\"%1\" not found").arg(m_query->text()), area);
}

void FindBar::updateHighlights() {
    auto *area = m_edit->area();
    QVector<qce::ExtraSelection> selections;
    if (!isHidden()) {
        const bool dark = area->palette().color(QPalette::Base).lightness() < 128;
        const QColor background(dark ? "#685326" : "#ffe39a");
        const QColor activeBackground(dark ? "#8d4d13" : "#ffae52");
        selections.reserve(m_matches.size() + 1);
        for (const auto &match : m_matches)
            selections.append({positionFor(match.start), positionFor(match.end), background, {}});
        if (m_current >= 0) {
            const auto current = m_matches[m_current];
            selections.append({positionFor(current.start), positionFor(current.end), activeBackground, {}});
        }
    }
    area->setExtraSelections(selections);
    m_next->setEnabled(!m_matches.isEmpty());
    m_previous->setEnabled(!m_matches.isEmpty());
    if (m_query->toolTip().isEmpty())
        m_count->setText(m_query->text().isEmpty() ? QString()
                                                   : tr("%1/%2").arg(m_current < 0 ? 0 : m_current + 1).arg(m_matches.size()));
}

void FindBar::closeSearch() {
    hide();
    m_matches.clear();
    m_current = -1;
    updateHighlights();
    m_edit->area()->setFocus();
}
