#include "OrderView.h"
#include "Theme.h"
#include "SdlKeyMap.h"
#include "UndoStack.h"
#include "CoreEvents.h"
#include "Speech.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTableView>
#include <QSpinBox>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QKeyEvent>
#include <QPainter>
#include <QInputDialog>
#include <QMenu>
#include <QAction>
#include <QShortcut>
#include <QLineEdit>
#include <QRegularExpressionValidator>

extern "C" {
#include "gcommon.h"
#include "gorder.h"
#include "gplay.h"
extern unsigned char songorder[MAX_SONGS][MAX_CHN][MAX_SONGLEN+2];
extern int songlen[MAX_SONGS][MAX_CHN];
extern int esnum;
extern int espos[MAX_CHN];
extern int eschn;
extern int escolumn;
extern unsigned char pattern[MAX_PATT][MAX_PATTROWS*4+4];
extern int pattlen[MAX_PATT];
extern char songname[MAX_STR];
extern int epchn;
extern int epnum[MAX_CHN];
extern int editmode;
extern int lastsonginit;
extern CHN chn[MAX_CHN];
extern int stereo_mode;
int isplaying(void);
static int orderShownChannels() { return stereo_mode ? MAX_CHN : 3; }
void orderlistcommands(void);
// Shared note-name lookup defined in qt_globals.c.
extern char *notename[];
}

// Active playback row per channel — mirrors OrderMiniMap math.
static int orderPlayRow(int c) {
    if (!isplaying()) return -1;
    int r;
    if (lastsonginit == PLAY_PATTERN) r = espos[c];
    else                              r = (int)chn[c].songptr - 1;
    if (r < 0) r = 0;
    return r;
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

OrderListModel::OrderListModel(QObject *parent) : QAbstractTableModel(parent) {}

int OrderListModel::rowCount(const QModelIndex &) const {
    int n = 0;
    int nc = orderShownChannels();
    for (int c = 0; c < nc; c++)
        if (songlen[esnum][c] > n) n = songlen[esnum][c];
    return n + 2; // include RST + restart slot
}
int OrderListModel::columnCount(const QModelIndex &) const { return orderShownChannels(); }

QVariant OrderListModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid()) return {};
    int r = i.row(), c = i.column();
    if (c < 0 || c >= MAX_CHN) return {};
    unsigned char v = songorder[esnum][c][r];

    if (role == Qt::DisplayRole) {
        if (v == LOOPSONG)       return QString("RST");
        if (v >= TRANSUP)        return QString("+%1").arg(v - TRANSUP, 1, 16).toUpper();
        if (v >= TRANSDOWN)      return QString("-%1").arg(v - TRANSDOWN, 1, 16).toUpper();
        if (v >= REPEAT)         return QString("R%1").arg(v - REPEAT, 1, 16).toUpper();
        if (r >= songlen[esnum][c] + 2) return QString("--");
        return QString("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }
    if (role == Qt::ForegroundRole) {
        // Bright purple background needs a high-contrast foreground —
        // Theme::C::rst / transpose / repeatCol are darker tints that
        // chosen against the previous near-black background and got
        // unreadable when the cells went bright. Force white on all
        // three special types.
        if (v >= REPEAT) return QBrush(QColor(255, 255, 255));
        if (r >= songlen[esnum][c] + 2) return QBrush(Theme::C::textDim);
        if (v == LOOPSONG)       return QBrush(Theme::C::rst);
        if (v >= TRANSDOWN)      return QBrush(Theme::C::transpose);
        if (v >= REPEAT)         return QBrush(Theme::C::repeatCol);
        return QBrush(Theme::C::text);
    }
    if (role == Qt::TextAlignmentRole) return int(Qt::AlignCenter);
    if (role == Qt::FontRole)          return Theme::monoFont(11);
    if (role == Qt::EditRole) {
        if (v == LOOPSONG) return QString();
        if (v >= REPEAT) return QString();
        return QString("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }
    return {};
}

QVariant OrderListModel::headerData(int section, Qt::Orientation o, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (o == Qt::Horizontal) return QString("Ch %1").arg(section + 1);
    return QString("%1").arg(section, 3, 16, QLatin1Char('0')).toUpper();
}

bool OrderListModel::setData(const QModelIndex &i, const QVariant &v, int role) {
    if (role != Qt::EditRole) return false;
    if (!i.isValid()) return false;
    bool ok = false;
    int newVal = v.toString().toInt(&ok, 16);
    if (!ok) return false;
    if (newVal < 0 || newVal > 0xCF) return false; // raw pattern numbers only
    int r = i.row(), c = i.column();
    songorder[esnum][c][r] = (unsigned char)newVal;
    if (r >= songlen[esnum][c]) songlen[esnum][c] = r + 1;
    emit dataChanged(i, i);
    return true;
}

Qt::ItemFlags OrderListModel::flags(const QModelIndex &i) const {
    if (!i.isValid()) return Qt::NoItemFlags;
    int r = i.row(), c = i.column();
    unsigned char v = songorder[esnum][c][r];
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // Only raw pattern numbers are editable inline; specials use the buttons.
    if (v < REPEAT) f |= Qt::ItemIsEditable;
    return f;
}

void OrderListModel::refresh() {
    beginResetModel();
    endResetModel();
}

// ---------------------------------------------------------------------------
// Delegate — paints semantic backgrounds + playback / cursor markers.
// ---------------------------------------------------------------------------
class OrderDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Custom editor: QLineEdit with 2-char hex validator. Default delegate's
    // editor would commit / re-edit on each keystroke because the model
    // accepts a 1-char hex value as valid, closing the editor before the user
    // can type the second nybble. With max-length 2 the QLineEdit keeps focus
    // until Enter / Escape / focus-out — same UX as the SDL build.
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &) const override {
        auto *e = new QLineEdit(parent);
        e->setMaxLength(2);
        e->setFont(Theme::monoFont(11));
        e->setAlignment(Qt::AlignCenter);
        auto *v = new QRegularExpressionValidator(
            QRegularExpression("[0-9a-fA-F]{1,2}"), e);
        e->setValidator(v);
        return e;
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &i) const override {
        QStyleOptionViewItem o = opt;
        int r = i.row(), c = i.column();
        unsigned char v = songorder[esnum][c][r];
        const bool isSpecial = (v >= REPEAT);   // RST / TRANSUP/DOWN / REPEAT
        // Background semantic tint — RST, transpose and repeat all share a
        // bright purple band so control blocks pop out of the order list
        // at a glance. Three shades keep the sub-types distinguishable.
        if (r >= songlen[esnum][c] + 2)      p->fillRect(opt.rect, Theme::C::bgBase);
        else if (v == LOOPSONG)              p->fillRect(opt.rect, QColor(180, 80, 220));
        else if (v >= TRANSDOWN)             p->fillRect(opt.rect, QColor(150, 70, 210));
        else if (v >= REPEAT)                p->fillRect(opt.rect, QColor(130, 60, 200));
        else if (r % 4 == 0)                 p->fillRect(opt.rect, Theme::C::beat);
        // Playback underlay (red) — drawn before the edit cursor so the
        // yellow cursor outline still wins when both land on the same cell.
        int pr = orderPlayRow(c);
        if (pr == r) {
            p->fillRect(opt.rect, Theme::C::playRow);
        }
        // Edit cursor underlay. Skip on special cells — clicking RST or
        // a transpose / repeat command shouldn't drop the yellow edit
        // border onto it; those values aren't hex-editable inline anyway.
        const bool cursorHere = (r == espos[c] && c == eschn && !isSpecial);
        if (cursorHere) {
            p->fillRect(opt.rect, Theme::C::editRow);
        }
        QStyledItemDelegate::paint(p, o, i);
        if (pr == r) {
            p->setPen(QPen(Theme::C::vuRed, 1));
            p->drawRect(opt.rect.adjusted(0, 0, -1, -1));
        }
        if (cursorHere) {
            p->setPen(QPen(Theme::C::highlight, 2));
            p->drawRect(opt.rect.adjusted(0, 0, -1, -1));
        }
    }
};

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

OrderView::OrderView(QWidget *parent) : QWidget(parent) {
    setAccessibleName("Order and song editor");
    Theme::applyDarkPalette(this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // --- Top bar: subtune + buttons ---
    auto *topBar = new QHBoxLayout();
    auto *subLbl = new QLabel("Subtune", this);
    subtuneSpin_ = new QSpinBox(this);
    subLbl->setBuddy(subtuneSpin_);  // names the spin box for screen readers
    subtuneSpin_->setRange(1, MAX_SONGS);
    subtuneSpin_->setMinimumWidth(60);
    subtuneSpin_->setToolTip("Current subtune (1..32). Each subtune has its own "
                             "3 orderlists.");
    connect(subtuneSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &OrderView::onSubtuneChanged);
    topBar->addWidget(subLbl);
    topBar->addWidget(subtuneSpin_);
    topBar->addSpacing(20);

    auto addBtn = [&](const QString &label, const char *tip, void (OrderView::*slot)()) {
        auto *b = new QPushButton(label, this);
        b->setToolTip(tip);
        // Several labels are cryptic glyphs (+1, -1, R, RST, Go, Swap 1↔2); the
        // tip is a full description, so reuse it as the accessible name.
        b->setAccessibleName(QString::fromLatin1(tip));
        b->setMinimumWidth(60);
        connect(b, &QPushButton::clicked, this, slot);
        topBar->addWidget(b);
        return b;
    };
    addBtn("Ins cell",  "Insert empty cell at cursor (this channel only). Key: Insert.",
           &OrderView::insertRow);
    addBtn("Ins row",   "Insert empty row across ALL channels at the cursor's row. Key: Shift+Insert.",
           &OrderView::insertRowAllChannels);
    addBtn("Del cell",  "Delete cell at cursor (this channel only). Key: Delete.",
           &OrderView::deleteRow);
    addBtn("Del row",   "Delete row across ALL channels at the cursor's row. Key: Shift+Delete.",
           &OrderView::deleteRowAllChannels);
    addBtn("+1",      "Insert transpose-up command",   &OrderView::insertTransposeUp);
    addBtn("-1",      "Insert transpose-down command", &OrderView::insertTransposeDown);
    addBtn("R",       "Insert repeat command",         &OrderView::insertRepeat);
    addBtn("RST",     "Insert RST endmark",            &OrderView::insertRst);
    addBtn("Go →",    "Jump to pattern (open in pattern editor)", &OrderView::gotoPattern);
    topBar->addSpacing(20);
    addBtn("Swap 1↔2","SHIFT+1: swap ch1 with current",&OrderView::swap12);
    addBtn("Swap 1↔3","SHIFT+2",                       &OrderView::swap13);
    addBtn("Swap 2↔3","SHIFT+3",                       &OrderView::swap23);
    topBar->addStretch();
    root->addLayout(topBar);

    // --- Main: table + pattern preview side by side ---
    auto *mid = new QHBoxLayout();
    table_ = new QTableView(this);
    table_->setAccessibleName("Order list");
    model_ = new OrderListModel(this);
    table_->setModel(model_);
    table_->setItemDelegate(new OrderDelegate(table_));
    table_->setShowGrid(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectItems);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    // AnyKeyPressed so typing on a selected cell opens the editor at the
    // first hex digit — the custom QLineEdit delegate has max-length 2 so
    // the second nybble fits before commit. The original concern (a
    // single digit committing too early) was fixed by the bounded editor,
    // not by the trigger; re-enabling AnyKeyPressed matches the SDL
    // build's behaviour of just typing the pattern number on a cell.
    table_->setEditTriggers(QAbstractItemView::DoubleClicked
                            | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::AnyKeyPressed
                            | QAbstractItemView::SelectedClicked);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->setDefaultSectionSize(20);
    table_->setAlternatingRowColors(false);
    table_->setFont(Theme::monoFont(11));
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &){ onSelectionChanged(); });
    table_->installEventFilter(this);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos){
                auto idx = table_->indexAt(pos);
                if (!idx.isValid()) return;
                table_->setCurrentIndex(idx);
                QMenu m(this);
                m.addAction("Insert empty cell (this channel)\tIns",
                            this, &OrderView::insertRow);
                m.addAction("Insert empty row (all channels)\tShift+Ins",
                            this, &OrderView::insertRowAllChannels);
                m.addAction("Delete cell (this channel)\tDel",
                            this, &OrderView::deleteRow);
                m.addAction("Delete row (all channels)\tShift+Del",
                            this, &OrderView::deleteRowAllChannels);
                m.addSeparator();
                m.addAction("Insert +1 transpose",  this, &OrderView::insertTransposeUp);
                m.addAction("Insert -1 transpose",  this, &OrderView::insertTransposeDown);
                m.addAction("Insert repeat",        this, &OrderView::insertRepeat);
                m.addAction("Insert RST endmark",   this, &OrderView::insertRst);
                m.addSeparator();
                m.addAction("Go to pattern",        this, &OrderView::gotoPattern);
                m.exec(table_->viewport()->mapToGlobal(pos));
            });
    mid->addWidget(table_, 2);

    // Pattern preview pane
    auto *previewBox = new QVBoxLayout();
    auto *previewHdr = new QLabel("Pattern preview", this);
    QFont ph = previewHdr->font(); ph.setBold(true); previewHdr->setFont(ph);
    previewBox->addWidget(previewHdr);
    patternPreview_ = new QLabel(this);
    patternPreview_->setFont(Theme::monoFont(10));
    patternPreview_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    patternPreview_->setAutoFillBackground(true);
    QPalette pp = patternPreview_->palette();
    pp.setColor(QPalette::Window, Theme::C::bgAlt);
    pp.setColor(QPalette::WindowText, Theme::C::text);
    patternPreview_->setPalette(pp);
    patternPreview_->setMinimumWidth(180);
    patternPreview_->setContentsMargins(8, 8, 8, 8);
    previewBox->addWidget(patternPreview_, 1);
    mid->addLayout(previewBox, 1);
    root->addLayout(mid, 1);

    // Bottom: tip strip
    auto *tip = new QLabel(
        "Tip: RET = open pattern in editor. SHIFT+R = repeat. "
        "TRANSPOSE must come before REPEAT in a sequence.",
        this);
    tip->setStyleSheet(QString("color:%1").arg(Theme::C::textDim.name()));
    root->addWidget(tip);

    // Repaint the table viewport so the play-row tint follows chn[c].songptr.
    // Event-driven via CoreEvents: orderPosChanged fires when the song order
    // pointer advances, transportChanged on play/stop so the tint clears.
    // Replaces the old ~30 Hz polling timer.
    if (auto *ev = CoreEvents::instance()) {
        connect(ev, &CoreEvents::orderPosChanged, this, [this]{
            if (table_) table_->viewport()->update();
        });
        connect(ev, &CoreEvents::transportChanged, this, [this](bool){
            if (table_) table_->viewport()->update();
        });
    }

    refresh();
}

void OrderView::refresh() {
    if (updating_) return;
    updating_ = true;
    model_->refresh();
    if (subtuneSpin_->value() != esnum + 1)
        subtuneSpin_->setValue(esnum + 1);
    syncCursorFromGlobal();
    updatePreview();
    updating_ = false;
}

void OrderView::syncCursorFromGlobal() {
    int row = espos[eschn];
    int col = eschn;
    table_->setCurrentIndex(model_->index(row, col));
}

void OrderView::syncGlobalFromCursor() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    eschn = idx.column();
    if (eschn < 0 || eschn >= MAX_CHN) return;
    espos[eschn] = idx.row();
}

void OrderView::onSubtuneChanged(int v) {
    if (updating_) return;
    esnum = v - 1;
    refresh();
    Speech::instance().say(QString("Subtune %1").arg(v), Speech::Priority::Status);
    emit edited();
}

// Concise self-voicing of the order cell under the cursor: channel, row, and
// either the pattern number or the special-command name. updatePreview() shows
// the full visual preview; this is the short spoken form.
void OrderView::announceCursor() {
    if (!Speech::instance().enabled()) return;
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int c = idx.column(), r = idx.row();
    unsigned char v = songorder[esnum][c][r];
    QString s = QString("Channel %1, row %2, ").arg(c + 1).arg(r);
    if (v >= REPEAT) {
        if (v == LOOPSONG)       s += "RST endmark";
        else if (v >= TRANSUP)   s += QString("transpose up %1").arg(v - TRANSUP);
        else if (v >= TRANSDOWN) s += QString("transpose down %1").arg(v - TRANSDOWN);
        else                     s += QString("repeat %1 times")
                                          .arg(v - REPEAT == 0 ? 16 : v - REPEAT);
    } else {
        s += QString("pattern %1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
    }
    Speech::instance().say(s, Speech::Priority::Cursor);
}

void OrderView::onSelectionChanged() {
    if (updating_) return;
    syncGlobalFromCursor();
    updatePreview();
    announceCursor();
    emit edited();
}

void OrderView::updatePreview() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) { patternPreview_->setText("(no row selected)"); return; }
    int c = idx.column();
    int r = idx.row();
    unsigned char v = songorder[esnum][c][r];
    if (v >= REPEAT) {
        QString kind;
        if (v == LOOPSONG)       kind = "RST endmark";
        else if (v >= TRANSUP)   kind = QString("Transpose +%1 halftones").arg(v - TRANSUP);
        else if (v >= TRANSDOWN) kind = QString("Transpose -%1 halftones").arg(v - TRANSDOWN);
        else                     kind = QString("Repeat next pattern %1× ")
                                          .arg(v - REPEAT == 0 ? 16 : v - REPEAT);
        patternPreview_->setText(QString("<b>%1</b><br>Not a pattern — special command.")
                                  .arg(kind));
        patternPreview_->setTextFormat(Qt::RichText);
        return;
    }
    int patnum = v;
    int plen = pattlen[patnum];
    // notename[] now lives in qt_globals.c — picks up microtonal renames.
    QString body;
    body += QString("Pattern $%1 — %2 rows on Ch %3\n")
              .arg(patnum, 2, 16, QLatin1Char('0')).toUpper()
              .arg(plen).arg(c + 1);
    int shown = qMin(16, plen);
    for (int row = 0; row < shown; row++) {
        const unsigned char *cell = &pattern[patnum][row * 4];
        unsigned char note = cell[0];
        unsigned char ins = cell[1];
        unsigned char cmd = cell[2];
        unsigned char par = cell[3];
        QString n;
        if (note == REST)        n = "...";
        else if (note == KEYOFF) n = "===";
        else if (note == KEYON)  n = "+++";
        else if (note >= FIRSTNOTE && note <= LASTNOTE) n = notename[note - FIRSTNOTE];
        else n = "...";
        QString insStr = ins ? QString("%1").arg(ins, 2, 16, QLatin1Char('0')).toUpper() : "..";
        QString cmdStr = (cmd || par)
            ? QString("%1%2").arg(cmd, 1, 16).arg(par, 2, 16, QLatin1Char('0')).toUpper()
            : "...";
        body += QString("%1  %2 %3 %4\n")
                  .arg(row, 3, 16, QLatin1Char('0')).toUpper()
                  .arg(n).arg(insStr).arg(cmdStr);
    }
    if (plen > shown) body += QString("… (%1 more rows)").arg(plen - shown);
    patternPreview_->setTextFormat(Qt::PlainText);
    patternPreview_->setText(body);
}

void OrderView::writeAtCursor(unsigned char v) {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int c = idx.column(); int r = idx.row();
    QByteArray before = captureSongSnapshot();
    songorder[esnum][c][r] = v;
    if (r >= songlen[esnum][c]) songlen[esnum][c] = r + 1;
    model_->refresh();
    syncCursorFromGlobal();
    pushEditIfChanged(this, std::move(before), "Order edit");
    emit edited();
}

void OrderView::insertRow() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int c = idx.column(); int r = idx.row();
    if (songlen[esnum][c] >= MAX_SONGLEN) return;
    QByteArray before = captureSongSnapshot();
    // The orderlist stores LOOPSONG + restart pos as two trailing bytes
    // past songlen[]. Snapshot them so the shift loop below doesn't drag
    // LOOPSONG into the new song-data slot (the user-visible 'RST got
    // overwritten' bug) — we replant both at the new tail after the
    // shift completes.
    int N = songlen[esnum][c];
    unsigned char loop    = songorder[esnum][c][N];      // LOOPSONG marker
    unsigned char restart = songorder[esnum][c][N + 1];  // restart row
    for (int i = N - 1; i >= r; i--)
        songorder[esnum][c][i + 1] = songorder[esnum][c][i];
    songorder[esnum][c][r] = 0;
    songlen[esnum][c]++;
    songorder[esnum][c][N + 1] = loop;
    songorder[esnum][c][N + 2] = restart;
    model_->refresh();
    syncCursorFromGlobal();
    pushEditIfChanged(this, std::move(before), "Insert order row");
    emit edited();
}

bool OrderView::eventFilter(QObject *o, QEvent *e) {
    if (o == table_ && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent*>(e);
        bool shift = ke->modifiers() & Qt::ShiftModifier;
        if (ke->key() == Qt::Key_Insert) {
            if (shift) insertRowAllChannels();
            else       insertRow();
            return true;
        }
        if (ke->key() == Qt::Key_Delete) {
            if (shift) deleteRowAllChannels();
            else       deleteRow();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void OrderView::insertRowAllChannels() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int r = idx.row();
    QByteArray before = captureSongSnapshot();
    int nch = stereo_mode ? MAX_CHN : 3;
    for (int c = 0; c < nch; c++) {
        if (songlen[esnum][c] >= MAX_SONGLEN) continue;
        if (r > songlen[esnum][c]) continue;
        int N = songlen[esnum][c];
        unsigned char loop    = songorder[esnum][c][N];
        unsigned char restart = songorder[esnum][c][N + 1];
        for (int i = N - 1; i >= r; i--)
            songorder[esnum][c][i + 1] = songorder[esnum][c][i];
        songorder[esnum][c][r] = 0;
        songlen[esnum][c]++;
        songorder[esnum][c][N + 1] = loop;
        songorder[esnum][c][N + 2] = restart;
    }
    model_->refresh();
    syncCursorFromGlobal();
    pushEditIfChanged(this, std::move(before), "Insert order row (all channels)");
    emit edited();
}

void OrderView::deleteRow() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int c = idx.column(); int r = idx.row();
    if (songlen[esnum][c] <= 1) return;
    QByteArray before = captureSongSnapshot();
    int N = songlen[esnum][c];
    unsigned char loop    = songorder[esnum][c][N];
    unsigned char restart = songorder[esnum][c][N + 1];
    for (int i = r; i < N - 1; i++)
        songorder[esnum][c][i] = songorder[esnum][c][i + 1];
    songlen[esnum][c]--;
    songorder[esnum][c][N - 1] = loop;
    songorder[esnum][c][N]     = restart;
    if (espos[c] >= songlen[esnum][c]) espos[c] = songlen[esnum][c] - 1;
    model_->refresh();
    syncCursorFromGlobal();
    pushEditIfChanged(this, std::move(before), "Delete order row");
    emit edited();
}

void OrderView::deleteRowAllChannels() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int r = idx.row();
    QByteArray before = captureSongSnapshot();
    int nch = stereo_mode ? MAX_CHN : 3;
    for (int c = 0; c < nch; c++) {
        if (songlen[esnum][c] <= 1) continue;
        if (r >= songlen[esnum][c]) continue;
        int N = songlen[esnum][c];
        unsigned char loop    = songorder[esnum][c][N];
        unsigned char restart = songorder[esnum][c][N + 1];
        for (int i = r; i < N - 1; i++)
            songorder[esnum][c][i] = songorder[esnum][c][i + 1];
        songlen[esnum][c]--;
        songorder[esnum][c][N - 1] = loop;
        songorder[esnum][c][N]     = restart;
        if (espos[c] >= songlen[esnum][c]) espos[c] = songlen[esnum][c] - 1;
    }
    model_->refresh();
    syncCursorFromGlobal();
    pushEditIfChanged(this, std::move(before), "Delete order row (all channels)");
    emit edited();
}

void OrderView::insertTransposeUp()   { writeAtCursor(TRANSUP + 1); }
void OrderView::insertTransposeDown() { writeAtCursor(TRANSDOWN + 1); }
void OrderView::insertRepeat()        { writeAtCursor(REPEAT + 1); }
void OrderView::insertRst()           { writeAtCursor(LOOPSONG); }

void OrderView::gotoPattern() {
    auto idx = table_->currentIndex();
    if (!idx.isValid()) return;
    int c = idx.column(); int r = idx.row();
    unsigned char v = songorder[esnum][c][r];
    if (v >= REPEAT) return;
    epchn = c;
    epnum[c] = v;
    editmode = 0; // EDIT_PATTERN
    emit edited();
}

void OrderView::swap12() {
    if (eschn == 0) return;
    for (int i = 0; i <= MAX_SONGLEN + 1; i++) {
        unsigned char t = songorder[esnum][eschn][i];
        songorder[esnum][eschn][i] = songorder[esnum][0][i];
        songorder[esnum][0][i] = t;
    }
    int t = songlen[esnum][eschn];
    songlen[esnum][eschn] = songlen[esnum][0];
    songlen[esnum][0] = t;
    model_->refresh();
    emit edited();
}
void OrderView::swap13() {
    if (eschn == 1) return;
    for (int i = 0; i <= MAX_SONGLEN + 1; i++) {
        unsigned char t = songorder[esnum][eschn][i];
        songorder[esnum][eschn][i] = songorder[esnum][1][i];
        songorder[esnum][1][i] = t;
    }
    int t = songlen[esnum][eschn];
    songlen[esnum][eschn] = songlen[esnum][1];
    songlen[esnum][1] = t;
    model_->refresh();
    emit edited();
}
void OrderView::swap23() {
    if (eschn == 2) return;
    for (int i = 0; i <= MAX_SONGLEN + 1; i++) {
        unsigned char t = songorder[esnum][eschn][i];
        songorder[esnum][eschn][i] = songorder[esnum][2][i];
        songorder[esnum][2][i] = t;
    }
    int t = songlen[esnum][eschn];
    songlen[esnum][eschn] = songlen[esnum][2];
    songlen[esnum][2] = t;
    model_->refresh();
    emit edited();
}
