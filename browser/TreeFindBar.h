#pragma once
// Search bar of a tree view: finds the text in all items (every column, also
// in collapsed branches); going to a match expands its ancestors, selects and
// shows it. Same keys as FindBar: Enter / F3 next, Shift+Enter / Shift+F3
// previous, Escape closes.
#include <QPersistentModelIndex>
#include <QVector>
#include <QWidget>
class QLabel;
class QLineEdit;
class QShortcut;
class QToolButton;
class QTreeView;

class TreeFindBar : public QWidget {
    Q_OBJECT
public:
    TreeFindBar(QWidget *parent, QTreeView *tree);
    void showSearch();
    void findNext(bool backwards = false);
    // The model changed (another line shown): the matches are found again
    void reset();

private:
    void rebuild();
    void activate(int index);
    void updateCount();
    void closeSearch();
    QTreeView *m_tree;
    QLineEdit *m_query;
    QLabel *m_count;
    QToolButton *m_case;
    QToolButton *m_regex;
    QToolButton *m_previous;
    QToolButton *m_next;
    QShortcut *m_escape;
    QVector<QPersistentModelIndex> m_matches;
    int m_current = -1;
};
