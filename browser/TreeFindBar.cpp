// Search bar of a tree view
#include "TreeFindBar.h"
#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QShortcut>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <functional>

TreeFindBar::TreeFindBar(QWidget *parent, QTreeView *tree) : QWidget(parent), m_tree(tree) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(5);
    auto button = [this](const QString &text, const QString &tip, bool toggle = false) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setCheckable(toggle);
        return b;
    };
    layout->addWidget(new QLabel(QString::fromUtf8("⌕"), this));
    m_query = new QLineEdit(this);
    m_query->setClearButtonEnabled(true);
    m_query->setPlaceholderText(tr("Search in keys and values"));
    m_query->setMaximumWidth(360);
    layout->addWidget(m_query, 1);
    m_case = button("Cc", tr("Match case"), true);
    m_regex = button(".*", tr("Regular expression"), true);
    layout->addWidget(m_case);
    layout->addWidget(m_regex);
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
    connect(close, &QToolButton::clicked, this, &TreeFindBar::closeSearch);
    connect(m_previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(m_next, &QToolButton::clicked, this, [this] { findNext(); });
    connect(m_query, &QLineEdit::textChanged, this, [this] {
        rebuild();
        if (!m_matches.isEmpty())
            activate(0);
    });
    connect(m_query, &QLineEdit::returnPressed, this, [this] { findNext(); });
    for (auto *b : {m_case, m_regex})
        connect(b, &QToolButton::toggled, this, [this] { rebuild(); });
    auto *backwards = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return), m_query);
    connect(backwards, &QShortcut::activated, this, [this] { findNext(true); });
    m_escape = new QShortcut(QKeySequence(Qt::Key_Escape), parent);
    m_escape->setContext(Qt::WidgetWithChildrenShortcut);
    m_escape->setEnabled(false);
    connect(m_escape, &QShortcut::activated, this, &TreeFindBar::closeSearch);
    hide();
}

void TreeFindBar::showSearch() {
    show();
    m_escape->setEnabled(true);
    rebuild();
    m_query->setFocus();
    m_query->selectAll();
}

void TreeFindBar::reset() {
    rebuild();
}

// All matching items in the order of the tree (depth first, row by row, column by column)
void TreeFindBar::rebuild() {
    m_matches.clear();
    m_current = -1;
    m_query->setToolTip(QString());
    auto *model = m_tree->model();
    if (!model || isHidden() || m_query->text().isEmpty()) {
        updateCount();
        return;
    }
    QRegularExpression expression(m_regex->isChecked() ? m_query->text() : QRegularExpression::escape(m_query->text()),
                                  m_case->isChecked() ? QRegularExpression::UseUnicodePropertiesOption
                                                      : QRegularExpression::CaseInsensitiveOption |
                                                                QRegularExpression::UseUnicodePropertiesOption);
    if (!expression.isValid()) {
        m_count->setText(tr("Invalid pattern"));
        m_query->setToolTip(expression.errorString());
        return;
    }
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            for (int column = 0; column < model->columnCount(parent); ++column) {
                const auto index = model->index(row, column, parent);
                if (expression.match(index.data().toString()).hasMatch())
                    m_matches.append(index);
            }
            visit(model->index(row, 0, parent));
        }
    };
    visit({});
    // Continue from the current item
    const auto current = m_tree->currentIndex();
    for (int i = 0; i < m_matches.size() && current.isValid(); ++i)
        if (m_matches[i] == current)
            m_current = i;
    updateCount();
}

void TreeFindBar::activate(int index) {
    m_current = index;
    const QModelIndex item = m_matches[index];
    if (!item.isValid())
        return;
    for (auto parent = item.parent(); parent.isValid(); parent = parent.parent())
        m_tree->expand(parent);
    m_tree->setCurrentIndex(item);
    m_tree->scrollTo(item, QAbstractItemView::PositionAtCenter);
    updateCount();
}

void TreeFindBar::findNext(bool backwards) {
    if (isHidden()) {
        // F3 without the bar: the last query
        show();
        rebuild();
        hide();
    }
    if (m_matches.isEmpty()) {
        if (!m_query->text().isEmpty())
            QToolTip::showText(m_tree->mapToGlobal(QPoint(10, 10)), tr("\"%1\" not found").arg(m_query->text()), m_tree);
        return;
    }
    int next = m_current + (backwards ? -1 : 1);
    if (m_current < 0)
        next = backwards ? int(m_matches.size()) - 1 : 0;
    next = (next + int(m_matches.size())) % int(m_matches.size());
    activate(next);
}

void TreeFindBar::updateCount() {
    m_next->setEnabled(!m_matches.isEmpty());
    m_previous->setEnabled(!m_matches.isEmpty());
    if (m_query->toolTip().isEmpty())
        m_count->setText(m_query->text().isEmpty() ? QString()
                                                   : tr("%1/%2").arg(m_current < 0 ? 0 : m_current + 1).arg(m_matches.size()));
}

void TreeFindBar::closeSearch() {
    hide();
    m_escape->setEnabled(false);
    m_tree->setFocus();
}
