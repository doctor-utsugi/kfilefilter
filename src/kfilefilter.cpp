// kfilefilter: a type-to-filter box for KDE's file and folder dialogs
// (KFileWidget, KDirSelectDialog), injected with LD_PRELOAD so neither KIO
// nor plasma-integration is rebuilt.

#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>

namespace
{

int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : fallback;
}

QStringList skippedNames()
{
    const QString raw = qEnvironmentVariable("KFILEFILTER_SKIP", QStringLiteral("node_modules,target,__pycache__,.git,.venv,venv"));
    return raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

class Filter : public QObject
{
    Q_OBJECT

public:
    // `anchor` is the widget the box goes above. `dirOperator` is set for
    // KFileWidget, whose view is swapped when the view mode changes.
    Filter(QWidget *dialog, QWidget *anchor, QBoxLayout *layout, QAbstractItemView *view, QObject *dirOperator)
        : QObject(dialog)
        , m_maxDepth(envInt("KFILEFILTER_DEPTH", 5))
        , m_maxListings(envInt("KFILEFILTER_MAX_DIRS", 3000))
        , m_skip(skippedNames())
    {
        m_edit = new QLineEdit(anchor->parentWidget());
        m_edit->setPlaceholderText(QStringLiteral("Filter (Ctrl+F)"));
        m_edit->setClearButtonEnabled(true);
        m_edit->installEventFilter(this);
        layout->insertWidget(layout->indexOf(anchor), m_edit);

        m_debounce.setSingleShot(true);
        m_debounce.setInterval(150);
        connect(&m_debounce, &QTimer::timeout, this, &Filter::apply);
        connect(m_edit, &QLineEdit::textChanged, &m_debounce, qOverload<>(&QTimer::start));

        // Listings arrive in bursts; expanding once per burst keeps the walk cheap.
        m_expand.setSingleShot(true);
        m_expand.setInterval(100);
        connect(&m_expand, &QTimer::timeout, this, &Filter::expandMatches);

        auto *find = new QShortcut(QKeySequence::Find, dialog);
        find->setContext(Qt::WidgetWithChildrenShortcut);
        connect(find, &QShortcut::activated, this, [this] {
            m_edit->setFocus();
            m_edit->selectAll();
        });

        if (dirOperator) {
            // String-based so no KIO symbol is linked; a rename upstream only disconnects.
            connect(dirOperator, SIGNAL(viewChanged(QAbstractItemView *)), this, SLOT(setView(QAbstractItemView *)));
            connect(dirOperator, SIGNAL(urlEntered(QUrl)), m_edit, SLOT(clear()));
        }

        setView(view);
    }

    static QAbstractItemView *findView(QWidget *dirOperator)
    {
        const auto views = dirOperator->findChildren<QAbstractItemView *>();
        for (QAbstractItemView *view : views) {
            if (isDirView(view)) {
                return view;
            }
        }
        return nullptr;
    }

    static bool isDirView(QAbstractItemView *view)
    {
        auto *proxy = view ? qobject_cast<QSortFilterProxyModel *>(view->model()) : nullptr;
        return proxy && proxy->sourceModel() && proxy->sourceModel()->inherits("KDirModel");
    }

private Q_SLOTS:
    void setView(QAbstractItemView *view)
    {
        if (m_view) {
            m_view->removeEventFilter(this);
        }
        m_view = view;
        m_proxy = nullptr;
        m_source = nullptr;
        if (!isDirView(view)) {
            return;
        }
        m_proxy = static_cast<QSortFilterProxyModel *>(view->model());
        m_source = m_proxy->sourceModel();
        m_view->installEventFilter(this);
        connect(m_source, &QAbstractItemModel::rowsInserted, this, &Filter::sourceRowsInserted, Qt::UniqueConnection);
        connect(m_proxy, &QAbstractItemModel::rowsInserted, &m_expand, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        if (m_active) {
            m_active = false;
            apply();
        }
    }

private:
    void apply()
    {
        if (!m_proxy) {
            return;
        }
        m_text = m_edit->text().trimmed();
        if (m_text.isEmpty()) {
            if (m_active) {
                m_active = false;
                m_proxy->setFilterFixedString(QString());
                m_proxy->setRecursiveFilteringEnabled(m_savedRecursive);
                m_proxy->setAutoAcceptChildRows(m_savedAutoAccept);
                restoreExpanded();
                if (m_view && m_view->currentIndex().isValid()) {
                    m_view->scrollTo(m_view->currentIndex(), QAbstractItemView::PositionAtCenter);
                }
            }
            return;
        }
        if (!m_active) {
            m_active = true;
            m_listings = 0;
            saveExpanded();
            m_savedRecursive = m_proxy->isRecursiveFilteringEnabled();
            m_savedAutoAccept = m_proxy->autoAcceptChildRows();
            m_proxy->setRecursiveFilteringEnabled(true);
            m_proxy->setAutoAcceptChildRows(true);
            m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
            m_proxy->setFilterKeyColumn(0);
            m_crawlRoot = crawlRoot();
            crawl(m_crawlRoot, 0);
        }
        m_proxy->setFilterFixedString(m_text);
        m_expand.start();
    }

    QModelIndex sourceRoot() const
    {
        return m_view ? m_proxy->mapToSource(m_view->rootIndex()) : QModelIndex();
    }

    // The folder dialog's tree starts at "/": listing ahead from there would
    // spend the budget on /usr and /proc, so it starts at home when home is
    // in the tree.
    QModelIndex crawlRoot() const
    {
        QModelIndex index = sourceRoot();
        const QStringList parts = QDir::homePath().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            QModelIndex next;
            const int rows = m_source->rowCount(index);
            for (int row = 0; row < rows && !next.isValid(); ++row) {
                const QModelIndex child = m_source->index(row, 0, index);
                if (child.data().toString() == part) {
                    next = child;
                }
            }
            if (!next.isValid()) {
                return sourceRoot();
            }
            index = next;
        }
        return index;
    }

    // Depth below the crawl root, or -1 when the index lies outside it.
    int depthOf(QModelIndex index) const
    {
        const QModelIndex root = m_crawlRoot;
        int depth = 0;
        while (index != root) {
            if (!index.isValid()) {
                return -1;
            }
            index = index.parent();
            ++depth;
        }
        return depth;
    }

    // The tree only holds folders someone expanded, so matches deeper down
    // are found by listing the subtree ahead of the user, within a budget.
    void crawl(const QModelIndex &parent, int depth)
    {
        const int rows = m_source->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            visit(m_source->index(row, 0, parent), depth + 1);
        }
    }

    void visit(const QModelIndex &index, int depth)
    {
        if (depth >= m_maxDepth || !m_source->hasChildren(index) || m_skip.contains(index.data().toString())) {
            return;
        }
        if (m_source->canFetchMore(index)) {
            if (m_listings < m_maxListings) {
                ++m_listings;
                m_source->fetchMore(index);
            }
            return;
        }
        crawl(index, depth);
    }

    void sourceRowsInserted(const QModelIndex &parent, int first, int last)
    {
        if (!m_active) {
            return;
        }
        const int depth = depthOf(parent);
        if (depth < 0) {
            return;
        }
        for (int row = first; row <= last; ++row) {
            visit(m_source->index(row, 0, parent), depth + 1);
        }
    }

    // Kept in source terms: proxy indexes do not survive the filter changing.
    void saveExpanded()
    {
        m_expanded.clear();
        if (auto *tree = qobject_cast<QTreeView *>(m_view.data())) {
            saveExpandedBelow(tree, m_view->rootIndex());
        }
    }

    void saveExpandedBelow(QTreeView *tree, const QModelIndex &parent)
    {
        const int rows = m_proxy->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (tree->isExpanded(index)) {
                m_expanded << QPersistentModelIndex(m_proxy->mapToSource(index));
                saveExpandedBelow(tree, index);
            }
        }
    }

    void restoreExpanded()
    {
        auto *tree = qobject_cast<QTreeView *>(m_view.data());
        if (!tree) {
            return;
        }
        tree->collapseAll();
        for (const QPersistentModelIndex &index : std::as_const(m_expanded)) {
            if (index.isValid()) {
                tree->expand(m_proxy->mapFromSource(index));
            }
        }
        m_expanded.clear();
    }

    bool matches(const QModelIndex &index) const
    {
        return index.data().toString().contains(m_text, Qt::CaseInsensitive);
    }

    // Rows that do not match are only visible because a descendant does: open them.
    void expandMatches()
    {
        auto *tree = qobject_cast<QTreeView *>(m_view.data());
        if (m_active && tree) {
            expandBelow(tree, m_view->rootIndex());
        }
    }

    void expandBelow(QTreeView *tree, const QModelIndex &parent)
    {
        const int rows = m_proxy->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (!matches(index) && m_proxy->rowCount(index) > 0) {
                tree->expand(index);
                expandBelow(tree, index);
            }
        }
    }

    QModelIndex firstMatch(const QModelIndex &parent) const
    {
        const int rows = m_proxy->rowCount(parent);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_proxy->index(row, 0, parent);
            if (matches(index)) {
                return index;
            }
            const QModelIndex below = firstMatch(index);
            if (below.isValid()) {
                return below;
            }
        }
        return {};
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress) {
            return false;
        }
        auto *key = static_cast<QKeyEvent *>(event);

        if (watched == m_edit) {
            switch (key->key()) {
            case Qt::Key_Escape:
                if (m_edit->text().isEmpty()) {
                    return false;
                }
                m_edit->clear();
                return true;
            case Qt::Key_Down:
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (!m_view || !m_proxy) {
                    return false;
                }
                m_debounce.stop();
                apply();
                m_view->setFocus();
                if (const QModelIndex match = firstMatch(m_view->rootIndex()); match.isValid()) {
                    m_view->setCurrentIndex(match);
                    m_view->scrollTo(match);
                }
                return true;
            default:
                return false;
            }
        }

        if (watched == m_view) {
            if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
                return false;
            }
            if (key->key() == Qt::Key_Backspace && !m_edit->text().isEmpty()) {
                m_edit->setFocus();
                m_edit->backspace();
                return true;
            }
            // Typing in the tree feeds the filter instead of the view's type-ahead.
            const QString text = key->text();
            if (!text.isEmpty() && text.at(0).isPrint() && !text.at(0).isSpace()) {
                m_edit->setFocus();
                m_edit->insert(text);
                return true;
            }
        }
        return false;
    }

    QLineEdit *m_edit = nullptr;
    QPointer<QAbstractItemView> m_view;
    QSortFilterProxyModel *m_proxy = nullptr;
    QAbstractItemModel *m_source = nullptr;
    QTimer m_debounce;
    QTimer m_expand;
    QString m_text;
    QPersistentModelIndex m_crawlRoot;
    QList<QPersistentModelIndex> m_expanded;
    bool m_active = false;
    bool m_savedRecursive = false;
    bool m_savedAutoAccept = false;
    int m_listings = 0;
    const int m_maxDepth;
    const int m_maxListings;
    const QStringList m_skip;
};

class Hook : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Show || !watched->isWidgetType() || watched->property("kfilefilter").toBool()) {
            return false;
        }
        auto *dialog = static_cast<QWidget *>(watched);
        if (dialog->inherits("KFileWidget")) {
            dialog->setProperty("kfilefilter", true);
            if (QWidget *dirOperator = childOf(dialog, "KDirOperator")) {
                attach(dialog, dirOperator, Filter::findView(dirOperator), dirOperator);
            }
        } else if (dialog->inherits("KDirSelectDialog")) {
            dialog->setProperty("kfilefilter", true);
            if (QWidget *tree = childOf(dialog, "KFileTreeView")) {
                attach(dialog, tree, qobject_cast<QAbstractItemView *>(tree), nullptr);
            }
        }
        return false;
    }

private:
    static QWidget *childOf(QWidget *parent, const char *className)
    {
        const auto children = parent->findChildren<QWidget *>();
        for (QWidget *child : children) {
            if (child->inherits(className)) {
                return child;
            }
        }
        return nullptr;
    }

    // Anything unexpected leaves the dialog exactly as KDE built it.
    static void attach(QWidget *dialog, QWidget *anchor, QAbstractItemView *view, QObject *dirOperator)
    {
        auto *layout = anchor->parentWidget() ? qobject_cast<QBoxLayout *>(anchor->parentWidget()->layout()) : nullptr;
        // KDirOperator may create its view only after showing; viewChanged brings it.
        if (layout && layout->indexOf(anchor) >= 0 && (dirOperator || Filter::isDirView(view))) {
            new Filter(dialog, anchor, layout, view, dirOperator);
        }
    }
};

// Child processes (kioworkers, launched apps) must not inherit the hook.
void stripPreload()
{
    const QByteArray preload = qgetenv("LD_PRELOAD");
    QByteArrayList kept;
    for (const QByteArray &entry : preload.split(':')) {
        for (const QByteArray &part : entry.split(' ')) {
            if (!part.isEmpty() && !part.contains("kfilefilter")) {
                kept << part;
            }
        }
    }
    if (kept.isEmpty()) {
        qunsetenv("LD_PRELOAD");
    } else {
        qputenv("LD_PRELOAD", kept.join(':'));
    }
}

void startup()
{
    stripPreload();
    if (qEnvironmentVariableIsSet("KFILEFILTER_DISABLE") || !qobject_cast<QApplication *>(QCoreApplication::instance())) {
        return;
    }
    qApp->installEventFilter(new Hook(qApp));
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(startup)

#include "kfilefilter.moc"
