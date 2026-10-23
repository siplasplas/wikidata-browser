#pragma once
// Search bar of a read-only qcodeedit view: a port of the find bar of qceditor
// (editor/src/FindBar.cpp) without replacing. The host is the tab: a widget
// whose QVBoxLayout holds space() and then the editor; the bar lies over the
// top lines of the editor, or over space() when it pushes them down.
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <qce/TextCursor.h>
class QLabel;
class QLineEdit;
class QShortcut;
class QToolButton;

namespace qce {
class CodeEdit;
class ITextDocument;
} // namespace qce

class FindBar : public QWidget {
    Q_OBJECT
public:
    FindBar(QWidget *host, qce::CodeEdit *edit, qce::ITextDocument *document);
    void showSearch();
    void findNext(bool backwards = false);
    QWidget *space() const { return m_space; }

private:
    struct Match {
        int start;
        int end;
    };
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void rebuild(bool moveCursor = true, bool whileHidden = false);
    void showNotFound();
    void updateHighlights();
    void activate(int index);
    void revealRange(qce::TextCursor start, qce::TextCursor end);
    void reveal(int row);
    void setCovering(bool covering);
    void place();
    void closeSearch();
    int cursorOffset() const;
    int offsetFor(qce::TextCursor cursor) const;
    qce::TextCursor positionFor(int offset) const;
    QWidget *m_host;
    qce::CodeEdit *m_edit;
    qce::ITextDocument *m_document;
    QLineEdit *m_query;
    QLabel *m_count;
    QToolButton *m_case;
    QToolButton *m_words;
    QToolButton *m_regex;
    QToolButton *m_wrap;
    QToolButton *m_previous;
    QToolButton *m_next;
    QShortcut *m_escape = nullptr;
    QWidget *m_space;
    bool m_covering = false;
    bool m_placing = false;
    QVector<Match> m_matches;
    QVector<int> m_lineStarts;
    int m_current = -1;
};
