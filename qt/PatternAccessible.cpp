#include "PatternAccessible.h"
#include "PatternView.h"

#include <QAccessible>
#include <algorithm>

extern "C" {
#include "gcommon.h"
extern int eppos;
extern int epchn;
extern int stereo_mode;
extern int pattlen[MAX_PATT];
extern int epnum[MAX_CHN];
}

// shownChannels() is a file-static helper inside PatternView.cpp; mirror the
// same rule here (3 voices mono, 6 stereo) rather than widening its linkage.
static int accColumns() { return stereo_mode ? MAX_CHN : 3; }

// ===========================================================================
// Factory
// ===========================================================================
QAccessibleInterface *patternAccessibleFactory(const QString &key, QObject *object) {
    Q_UNUSED(key);
    if (auto *pv = qobject_cast<PatternView *>(object))
        return new PatternAccessible(pv);
    return nullptr;
}

// ===========================================================================
// PatternAccessible
// ===========================================================================
PatternAccessible::PatternAccessible(PatternView *view)
    : QAccessibleWidget(view, QAccessible::Table), view_(view) {}

PatternAccessible::~PatternAccessible() {
    flushCells();
}

void PatternAccessible::flushCells() {
    for (QAccessible::Id id : std::as_const(childToId_))
        QAccessible::deleteAccessibleInterface(id);
    childToId_.clear();
}

int PatternAccessible::columnCount() const { return accColumns(); }

int PatternAccessible::rowCount() const {
    int n = 0;
    const int cols = columnCount();
    for (int c = 0; c < cols; c++) {
        int pn = epnum[c];
        if (pn >= 0 && pn < MAX_PATT)
            n = std::max(n, pattlen[pn]);
    }
    return n;
}

int PatternAccessible::childCount() const { return rowCount() * columnCount(); }

QAccessibleInterface *PatternAccessible::child(int logicalIndex) const {
    if (logicalIndex < 0 || logicalIndex >= childCount())
        return nullptr;
    auto it = childToId_.constFind(logicalIndex);
    if (it != childToId_.constEnd())
        return QAccessible::accessibleInterface(it.value());
    const int cols = columnCount();
    auto *cell = new PatternCellAccessible(const_cast<PatternAccessible *>(this),
                                           logicalIndex / cols, logicalIndex % cols);
    QAccessible::registerAccessibleInterface(cell);     // QAccessibleCache now owns it
    childToId_.insert(logicalIndex, QAccessible::uniqueId(cell));
    return cell;
}

QAccessibleInterface *PatternAccessible::cellAt(int row, int column) const {
    if (row < 0 || row >= rowCount() || column < 0 || column >= columnCount())
        return nullptr;
    return child(row * columnCount() + column);
}

int PatternAccessible::indexOfChild(const QAccessibleInterface *c) const {
    if (const auto *cell = dynamic_cast<const PatternCellAccessible *>(c))
        return cell->rowIndex() * columnCount() + cell->columnIndex();
    return -1;
}

QAccessibleInterface *PatternAccessible::childAt(int, int) const {
    // No hit-testing — the grid is keyboard-driven; AT reads cells by index.
    return nullptr;
}

QList<QAccessibleInterface *> PatternAccessible::selectedCells() const {
    QList<QAccessibleInterface *> out;
    if (auto *c = cellAt(eppos, epchn))
        out.append(c);
    return out;
}

QString PatternAccessible::columnDescription(int column) const {
    return QStringLiteral("Channel %1").arg(column + 1);
}

QString PatternAccessible::rowDescription(int row) const {
    return QStringLiteral("Row %1").arg(row, 2, 16, QLatin1Char('0')).toUpper();
}

void PatternAccessible::modelChange(QAccessibleTableModelChangeEvent *event) {
    if (!event || event->modelChangeType() == QAccessibleTableModelChangeEvent::ModelReset)
        flushCells();
}

void *PatternAccessible::interface_cast(QAccessible::InterfaceType t) {
    if (t == QAccessible::TableInterface)
        return static_cast<QAccessibleTableInterface *>(this);
    // Chain to the widget base (Action/Attributes/…) — our base is
    // QAccessibleWidget, not QAccessibleObject, so we must NOT return nullptr.
    return QAccessibleWidget::interface_cast(t);
}

// ===========================================================================
// PatternCellAccessible
// ===========================================================================
bool PatternCellAccessible::isValid() const {
    return table_ && row_ >= 0 && col_ >= 0
        && row_ < table_->rowCount() && col_ < table_->columnCount();
}

QAccessibleInterface *PatternCellAccessible::parent() const {
    return QAccessible::queryAccessibleInterface(table_->view());
}

QAccessibleInterface *PatternCellAccessible::table() const {
    return QAccessible::queryAccessibleInterface(table_->view());
}

QString PatternCellAccessible::text(QAccessible::Text t) const {
    if (t != QAccessible::Name) return {};
    if (col_ < 0 || col_ >= MAX_CHN) return {};
    int pn = epnum[col_];
    if (pn < 0 || pn >= MAX_PATT) return {};
    // Reuse the same decode the self-voicing path uses.
    QString s = table_->view()->cellSpeech(pn, row_);
    return s.isEmpty() ? QStringLiteral("rest") : s;
}

QRect PatternCellAccessible::rect() const {
    if (!isValid()) return {};
    PatternView *v = table_->view();
    QRect r = v->cellRect(col_, row_);   // viewport coordinates
    if (!r.isValid()) return {};
    return QRect(v->viewport()->mapToGlobal(r.topLeft()), r.size());
}

QAccessible::State PatternCellAccessible::state() const {
    QAccessible::State st;
    st.selectable = true;
    st.focusable = true;
    if (isSelected()) {
        st.selected = true;
        st.focused = true;
    }
    return st;
}

bool PatternCellAccessible::isSelected() const {
    return row_ == eppos && col_ == epchn;
}

void *PatternCellAccessible::interface_cast(QAccessible::InterfaceType t) {
    if (t == QAccessible::TableCellInterface)
        return static_cast<QAccessibleTableCellInterface *>(this);
    return nullptr;
}
