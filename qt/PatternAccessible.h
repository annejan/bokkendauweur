#pragma once
#include <QAccessible>
#include <QAccessibleWidget>
#include <QAccessibleInterface>
#include <QHash>

class PatternView;

// QAccessible factory: returns a PatternAccessible for PatternView instances,
// nullptr for anything else (so Qt's default factory still serves every other
// widget). Installed once from PatternView's constructor.
QAccessibleInterface *patternAccessibleFactory(const QString &key, QObject *object);

// ---------------------------------------------------------------------------
// PatternAccessible — exposes the hand-painted pattern grid to screen readers
// (Orca / NVDA / VoiceOver, and braille) as a table: rows = pattern rows,
// columns = channels (3 mono / 6 stereo). Complements the self-voicing path;
// both are inert unless an AT is active.
// ---------------------------------------------------------------------------
class PatternAccessible : public QAccessibleWidget,
                          public QAccessibleTableInterface {
public:
    explicit PatternAccessible(PatternView *view);
    ~PatternAccessible() override;

    PatternView *view() const { return view_; }

    // --- QAccessibleInterface (override only what differs from QAccessibleWidget) ---
    QAccessible::Role role() const override { return QAccessible::Table; }
    int childCount() const override;
    QAccessibleInterface *child(int logicalIndex) const override;
    int indexOfChild(const QAccessibleInterface *child) const override;
    QAccessibleInterface *childAt(int x, int y) const override;
    void *interface_cast(QAccessible::InterfaceType t) override;

    // --- QAccessibleTableInterface (all pure virtuals) ---
    QAccessibleInterface *cellAt(int row, int column) const override;
    QAccessibleInterface *caption() const override { return nullptr; }
    QAccessibleInterface *summary() const override { return nullptr; }
    int columnCount() const override;
    int rowCount() const override;
    // selection (a single editor cursor — never whole rows/columns)
    int selectedCellCount() const override { return 1; }
    int selectedColumnCount() const override { return 0; }
    int selectedRowCount() const override { return 0; }
    QList<QAccessibleInterface *> selectedCells() const override;
    QList<int> selectedColumns() const override { return {}; }
    QList<int> selectedRows() const override { return {}; }
    QString columnDescription(int column) const override;
    QString rowDescription(int row) const override;
    bool isColumnSelected(int column) const override { return false; }
    bool isRowSelected(int row) const override { return false; }
    bool selectRow(int row) override { return false; }
    bool selectColumn(int column) override { return false; }
    bool unselectRow(int row) override { return false; }
    bool unselectColumn(int column) override { return false; }
    void modelChange(QAccessibleTableModelChangeEvent *event) override;

private:
    void flushCells();

    PatternView *view_;
    // logicalIndex (row*columnCount()+col) -> registered cell Id. Qt's
    // QAccessibleCache owns the cell objects once registered; this map is the
    // sole reference, flushed on ~PatternAccessible / modelChange(ModelReset).
    mutable QHash<int, QAccessible::Id> childToId_;
};

// ---------------------------------------------------------------------------
// PatternCellAccessible — one table cell = one (row, channel). NOT a QObject;
// derives directly from QAccessibleInterface + QAccessibleTableCellInterface.
// ---------------------------------------------------------------------------
class PatternCellAccessible : public QAccessibleInterface,
                              public QAccessibleTableCellInterface {
public:
    PatternCellAccessible(PatternAccessible *table, int row, int col)
        : table_(table), row_(row), col_(col) {}

    // --- QAccessibleInterface ---
    bool isValid() const override;
    QObject *object() const override { return nullptr; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }
    QAccessibleInterface *parent() const override;
    QAccessibleInterface *child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QString text(QAccessible::Text t) const override;
    void setText(QAccessible::Text, const QString &) override {}
    QRect rect() const override;
    QAccessible::Role role() const override { return QAccessible::Cell; }
    QAccessible::State state() const override;
    void *interface_cast(QAccessible::InterfaceType t) override;

    // --- QAccessibleTableCellInterface ---
    bool isSelected() const override;
    QList<QAccessibleInterface *> columnHeaderCells() const override { return {}; }
    QList<QAccessibleInterface *> rowHeaderCells() const override { return {}; }
    int columnIndex() const override { return col_; }
    int rowIndex() const override { return row_; }
    int columnExtent() const override { return 1; }
    int rowExtent() const override { return 1; }
    QAccessibleInterface *table() const override;

private:
    PatternAccessible *table_;
    int row_;
    int col_;
};
