#include "TablesView.h"
#include "Theme.h"
#include "SdlKeyMap.h"
#include "UndoStack.h"
#include "Speech.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTableView>
#include <QScrollBar>
#include <QHeaderView>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QGroupBox>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QMenu>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <functional>

extern "C" {
#include "gcommon.h"
extern unsigned char ltable[MAX_TABLES][MAX_TABLELEN];
extern unsigned char rtable[MAX_TABLES][MAX_TABLELEN];
extern int etnum;
extern int einum;
extern int etpos;
extern int etcolumn;
extern int etview[MAX_TABLES];
extern int etlock;
extern INSTR instr[MAX_INSTR];
extern int key;
extern int rawkey;
extern int shiftpressed;
void tablecommands(void);
}

static const char *tableName[4] = {"Wave", "Pulse", "Filter", "Speed"};
static const QColor tableTint[4] = {
    QColor(0xA1, 0xC1, 0x81),
    QColor(0x6F, 0xA6, 0xCE),
    QColor(0xD8, 0x9B, 0x8C),
    QColor(0xE0, 0xC1, 0x6E)
};

// ---------------------------------------------------------------------------
// Cell decoder (kept from previous incarnation)
// ---------------------------------------------------------------------------
static QString decodeCell(int t, unsigned char L, unsigned char R) {
    auto hex = [](unsigned v, int w = 2) {
        return QString("%1").arg(v, w, 16, QLatin1Char('0')).toUpper();
    };
    QString line;
    if (t == 0) {
        if (L == 0x00) line = "no change";
        else if (L >= 0x01 && L <= 0x0F) line = QString("delay %1 frames").arg(L);
        else if (L >= 0xE0 && L <= 0xEF) line = QString("inaudible $%1").arg(hex(L - 0xE0));
        else if (L == 0xFF) line = R == 0 ? "stop" : QString("jump to step $%1").arg(hex(R));
        else if (L >= 0xF0 && L <= 0xFE) line = QString("execute cmd %1XY param $%2")
                                                .arg(L - 0xF0, 1, 16).arg(hex(R));
        else {
            QStringList bits;
            if (L & 0x80) bits << "noise";
            if (L & 0x40) bits << "pulse";
            if (L & 0x20) bits << "saw";
            if (L & 0x10) bits << "tri";
            if (L & 0x08) bits << "test";
            if (L & 0x04) bits << "ring";
            if (L & 0x02) bits << "sync";
            if (L & 0x01) bits << "gate";
            line = QString("wave $%1 [%2]").arg(hex(L)).arg(bits.join('+'));
        }
        // R is only a note byte for waveform / delay rows. For 'jump'
        // (L=$FF, R = target step) and 'execute cmd' (L=$F0..$FE, R = cmd
        // param), R is data; don't tack on a bogus 'note: rel +xx'.
        const bool rIsNote = !(L == 0xFF || (L >= 0xF0 && L <= 0xFE));
        if (rIsNote) {
            QString rs;
            if (R == 0x80) rs = "hold freq";
            else if (R >= 0x81) rs = QString("abs note %1").arg(R - 0x81);
            else if (R >= 0x60) rs = QString("neg rel %1").arg(R - 0x60);
            else rs = QString("rel +%1").arg(R);
            line += "    note: " + rs;
        }
    } else if (t == 1) {
        if (L == 0xFF) line = R == 0 ? "stop" : QString("jump to step $%1").arg(hex(R));
        else if (L >= 0x80) {
            int pw = ((L & 0x0F) << 8) | R;
            line = QString("set pulse width $%1").arg(hex(pw, 3));
        } else {
            int spd = (signed char)R;
            line = QString("modulate %1 ticks @ %2%3").arg(L)
                   .arg(spd >= 0 ? "+" : "").arg(spd);
        }
    } else if (t == 2) {
        if (L == 0x00) line = QString("set cutoff $%1").arg(hex(R));
        else if (L == 0xFF) line = R == 0 ? "stop" : QString("jump to step $%1").arg(hex(R));
        else if (L >= 0x80) {
            const char *pass = "??";
            switch (L & 0xF0) {
                case 0x80: pass = "off";      break;
                case 0x90: pass = "lowpass";  break;
                case 0xA0: pass = "bandpass"; break;
                case 0xB0: pass = "low+band"; break;
                case 0xC0: pass = "highpass"; break;
                case 0xD0: pass = "low+high"; break;
                case 0xE0: pass = "high+bnd"; break;
                case 0xF0: pass = "all-pass"; break;
            }
            int res = (R >> 4) & 0xf;
            int mask = R & 0xf;
            line = QString("filter %1, res=$%2, ch-mask=%3")
                   .arg(pass).arg(res, 1, 16).arg(mask, 1, 16);
        } else {
            int spd = (signed char)R;
            line = QString("modulate %1 ticks @ %2%3").arg(L)
                   .arg(spd >= 0 ? "+" : "").arg(spd);
        }
    } else {
        bool noteIndep = (L & 0x80) != 0;
        if (noteIndep) {
            line = QString("note-indep, divisor %1").arg(L & 0x7f);
        } else {
            line = QString("vib spd=%1 depth=%2  porta=%3  funk evn=%4 odd=%5")
                   .arg(L, 1, 16).arg(R, 1, 16)
                   .arg(((unsigned)L << 8) | R, 4, 16, QLatin1Char('0'))
                   .arg(L, 2, 16, QLatin1Char('0'))
                   .arg(R, 2, 16, QLatin1Char('0')).toUpper();
        }
    }
    return line;
}

// ---------------------------------------------------------------------------
// Model — columns: 0 = index, 1 = left byte, 2 = right byte
// ---------------------------------------------------------------------------
SidTableModel::SidTableModel(int tableIndex, QObject *parent)
    : QAbstractTableModel(parent), t_(tableIndex) {}

int SidTableModel::rowCount(const QModelIndex &) const { return MAX_TABLELEN; }
int SidTableModel::columnCount(const QModelIndex &) const { return 4; }

QVariant SidTableModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid()) return {};
    int row = i.row();
    int col = i.column();
    unsigned char L = ltable[t_][row];
    unsigned char R = rtable[t_][row];

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (col == 0) return QString("%1").arg(row + 1, 2, 16, QLatin1Char('0')).toUpper();
        if (col == 1) return QString("%1").arg(L, 2, 16, QLatin1Char('0')).toUpper();
        if (col == 2) return QString("%1").arg(R, 2, 16, QLatin1Char('0')).toUpper();
        if (col == 3 && role == Qt::DisplayRole)
            return (L == 0 && R == 0) ? QString() : decodeCell(t_, L, R);
    }
    if (role == Qt::TextAlignmentRole) {
        return (col == 3) ? int(Qt::AlignVCenter | Qt::AlignLeft)
                          : int(Qt::AlignCenter);
    }
    if (role == Qt::FontRole) {
        return (col == 3) ? Theme::uiFont(10) : Theme::monoFont(11);
    }
    if (role == Qt::ForegroundRole) {
        if (col == 0) return QBrush(Theme::C::textDim);
        if (col == 1) return QBrush(L ? Theme::C::cmdDigit : Theme::C::textDim);
        if (col == 2) return QBrush(R ? Theme::C::cmdParam : Theme::C::textDim);
        if (col == 3) return QBrush(Theme::C::text);
    }
    return {};
}

QVariant SidTableModel::headerData(int section, Qt::Orientation o, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (o == Qt::Horizontal) {
        switch (section) {
            case 0: return QString("Idx");
            case 1: return QString("L");
            case 2: return QString("R");
            case 3: return QString("What it does");
        }
    }
    return {};
}

bool SidTableModel::setData(const QModelIndex &i, const QVariant &v, int role) {
    if (role != Qt::EditRole || !i.isValid()) return false;
    int col = i.column(); int row = i.row();
    if (col == 0 || col == 3) return false;
    bool ok = false;
    int val = v.toString().toInt(&ok, 16);
    if (!ok || val < 0 || val > 255) return false;
    if (col == 1) ltable[t_][row] = (unsigned char)val;
    else if (col == 2) rtable[t_][row] = (unsigned char)val;
    emit dataChanged(index(row, 0), index(row, 3));
    return true;
}

Qt::ItemFlags SidTableModel::flags(const QModelIndex &i) const {
    if (!i.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (i.column() == 1 || i.column() == 2) f |= Qt::ItemIsEditable;
    return f;
}

void SidTableModel::refresh() {
    beginResetModel();
    endResetModel();
}

// ---------------------------------------------------------------------------
// Delegate paints semantic backgrounds + cursor
// ---------------------------------------------------------------------------
class TableCellDelegate : public QStyledItemDelegate {
public:
    TableCellDelegate(int tableIdx, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), t_(tableIdx) {}
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                          const QModelIndex &i) const override {
        // L / R columns: full 2-char hex editor. The default delegate fed
        // single-keystroke edits straight into the model and committed on
        // the first character, leaving the user's typed digit (and a
        // padded second digit '.') visible mid-edit. Same fix as OrderView:
        // QLineEdit with maxLength 2 + hex validator, stays open until
        // Return / Escape / focus loss.
        if (i.column() != 1 && i.column() != 2) return nullptr;
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
        int row = i.row();
        unsigned char L = ltable[t_][row];
        QColor bg = Theme::C::bgBase;
        // Empty rows dim
        if (L == 0 && rtable[t_][row] == 0) bg = Theme::C::bgBase;
        else if (L == 0xFF) bg = QColor(70, 30, 25);            // jump
        else if (L >= 0xF0 && t_ == 0) bg = QColor(70, 45, 20); // wave-cmd exec
        else if (L >= 0x80 && t_ == 0) bg = QColor(50, 25, 50); // noise
        else if (L >= 0x40 && t_ == 0) bg = QColor(20, 40, 60); // pulse
        else if (L >= 0x20 && t_ == 0) bg = QColor(60, 45, 20); // saw
        else if (L >= 0x10 && t_ == 0) bg = QColor(30, 50, 30); // tri
        else if (L >= 0x01 && t_ == 0 && L <= 0x0F) bg = QColor(20, 30, 50); // delay
        else if (t_ == 1 && L >= 0x80 && L < 0xFF) bg = QColor(20, 40, 55); // pulse-pw
        else if (t_ == 2 && L >= 0x80 && L < 0xFF) bg = QColor(55, 35, 30); // filter params
        if (row % 4 == 0 && bg == Theme::C::bgBase) bg = Theme::C::beat;

        p->fillRect(opt.rect, bg);
        // Cursor highlight (matches the global etnum / etpos)
        if (t_ == etnum && row == etpos) {
            p->fillRect(opt.rect, Theme::C::editRow);
        }
        QStyledItemDelegate::paint(p, opt, i);
        if (t_ == etnum && row == etpos) {
            p->setPen(QPen(Theme::C::highlight, 1));
            p->drawRect(opt.rect.adjusted(0, 0, -1, -1));
        }
    }
private:
    int t_;
};

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
TablesView::TablesView(QWidget *parent) : QWidget(parent) {
    setAccessibleName("Tables editor");
    Theme::applyDarkPalette(this);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // ---- Left: tabs containing the 4 tables --------------------------------
    auto *leftCol = new QVBoxLayout();
    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    for (int t = 0; t < MAX_TABLES; t++) {
        views_[t]  = new QTableView(this);
        views_[t]->setAccessibleName(QString("%1 table").arg(tableName[t]));
        models_[t] = new SidTableModel(t, this);
        views_[t]->setModel(models_[t]);
        views_[t]->setItemDelegate(new TableCellDelegate(t, views_[t]));
        // AnyKeyPressed + SelectedClicked so the user can click a cell
        // and start typing the hex value directly. The custom QLineEdit
        // delegate above has max-length 2 so the original 'first digit
        // commits' concern doesn't apply. Enter commits + Qt's default
        // tab/enter behaviour advances the selection to the next row,
        // so the workflow becomes: click row $00 -> type FA <Enter>
        // -> type 09 <Enter> -> arrow-key navigate / type next.
        views_[t]->setEditTriggers(QAbstractItemView::DoubleClicked
                                   | QAbstractItemView::EditKeyPressed
                                   | QAbstractItemView::AnyKeyPressed
                                   | QAbstractItemView::SelectedClicked);
        // Enter commits the active cell and moves selection one row down
        // (the default Qt behaviour) so streaming values into successive
        // rows works without reaching for the arrow keys after every
        // commit.
        views_[t]->setTabKeyNavigation(true);
        views_[t]->setShowGrid(false);
        views_[t]->verticalHeader()->hide();
        // Per-pixel wheel scroll feels right for the 255-row tables —
        // the default per-item step lurched a single 22-px row at a
        // time. The visible scroll bar stays in place; only the wheel
        // increment / kinetic feel change.
        views_[t]->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        views_[t]->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        views_[t]->verticalScrollBar()->setSingleStep(22);
        auto *hh = views_[t]->horizontalHeader();
        hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        hh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        hh->setSectionResizeMode(3, QHeaderView::Stretch);
        views_[t]->verticalHeader()->setDefaultSectionSize(22);
        views_[t]->setSelectionBehavior(QAbstractItemView::SelectItems);
        views_[t]->setSelectionMode(QAbstractItemView::SingleSelection);
        views_[t]->setStyleSheet(Theme::tableViewSheet());
        connect(views_[t]->selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this](const QModelIndex &, const QModelIndex &){ onCellSelectionChanged(); });
        tabs_->addTab(views_[t], tableName[t]);
        // Colour each tab title
        tabs_->tabBar()->setTabTextColor(t, tableTint[t]);
    }
    connect(tabs_, &QTabWidget::currentChanged, this, &TablesView::onTabChanged);
    leftCol->addWidget(tabs_, 1);

    // "Add step…" dropdown gives typed templates per active table.
    auto *btnRow = new QHBoxLayout();
    auto *addBtn = new QToolButton(this);
    addBtn->setText("+ Add step…");
    addBtn->setPopupMode(QToolButton::InstantPopup);
    addBtn->setToolTip("Insert a typed step at cursor (waveform, delay, jump, "
                       "modulation, etc.) Picks the right L/R bytes for you.");
    auto *addMenu = new QMenu(addBtn);
    addBtn->setMenu(addMenu);
    btnRow->addWidget(addBtn);
    auto buildAddMenu = [this, addMenu]() {
        addMenu->clear();
        auto seed = [this, addMenu](const QString &label, std::function<void()> f) {
            auto *a = addMenu->addAction(label);
            connect(a, &QAction::triggered, this, [this, f]{
                QByteArray b = captureSongSnapshot();
                f();
                refresh();
                pushEditIfChanged(this, std::move(b), "Add table step");
                emit edited();
            });
        };
        auto set = [](int t, int row, unsigned char L, unsigned char R){
            ltable[t][row] = L; rtable[t][row] = R;
        };
        const int t = etnum, r = etpos;
        if (t == 0) {
            seed("Sawtooth at original note",     [=]{ set(0,r,0x21,0x00); });
            seed("Pulse at original note",        [=]{ set(0,r,0x41,0x00); });
            seed("Triangle at original note",     [=]{ set(0,r,0x11,0x00); });
            seed("Noise at original note",        [=]{ set(0,r,0x81,0x00); });
            addMenu->addSeparator();
            seed("Delay 1 frame",                 [=]{ set(0,r,0x01,0x00); });
            seed("Hold frequency",                [=]{ set(0,r,0x00,0x80); });
            seed("Run pattern cmd (set ADSR)",    [=]{ set(0,r,0xF5,0xAA); });
            addMenu->addSeparator();
            seed("Jump to step 1 (loop)",         [=]{ set(0,r,0xFF,0x01); });
            seed("Stop wavetable (FF 00)",        [=]{ set(0,r,0xFF,0x00); });
        } else if (t == 1) {
            seed("Set pulse width = $800 (square)", [=]{ set(1,r,0x88,0x00); });
            seed("Set pulse width = $400 (narrow)", [=]{ set(1,r,0x84,0x00); });
            seed("Set pulse width = $000 (thin)",   [=]{ set(1,r,0x80,0x00); });
            addMenu->addSeparator();
            seed("Modulate +32 / tick for 64 ticks", [=]{ set(1,r,0x40,0x20); });
            seed("Modulate -32 / tick for 64 ticks", [=]{ set(1,r,0x40,0xE0); });
            addMenu->addSeparator();
            seed("Jump to step 1 (loop)",        [=]{ set(1,r,0xFF,0x01); });
            seed("Stop pulsetable (FF 00)",      [=]{ set(1,r,0xFF,0x00); });
        } else if (t == 2) {
            seed("Lowpass, res F, ch1 only",     [=]{ set(2,r,0x90,0xF1); });
            seed("Bandpass, res 8, ch1+2+3",     [=]{ set(2,r,0xA0,0x87); });
            seed("Highpass, res F, ch1 only",    [=]{ set(2,r,0xC0,0xF1); });
            addMenu->addSeparator();
            seed("Set cutoff = $40",             [=]{ set(2,r,0x00,0x40); });
            seed("Set cutoff = $80",             [=]{ set(2,r,0x00,0x80); });
            seed("Set cutoff = $F0",             [=]{ set(2,r,0x00,0xF0); });
            addMenu->addSeparator();
            seed("Sweep cutoff +1 for 127 ticks", [=]{ set(2,r,0x7F,0x01); });
            seed("Sweep cutoff -1 for 127 ticks", [=]{ set(2,r,0x7F,0xFF); });
            addMenu->addSeparator();
            seed("Jump to step 1 (loop)",        [=]{ set(2,r,0xFF,0x01); });
            seed("Stop filtertable (FF 00)",     [=]{ set(2,r,0xFF,0x00); });
        } else {
            seed("Vibrato: speed 3, depth $40",  [=]{ set(3,r,0x03,0x40); });
            seed("Vibrato: speed 5, depth $04 (slow)", [=]{ set(3,r,0x05,0x04); });
            seed("Vibrato: note-indep, divisor 4", [=]{ set(3,r,0x83,0x04); });
            addMenu->addSeparator();
            seed("Portamento $0020 / tick",      [=]{ set(3,r,0x00,0x20); });
            seed("Portamento $0100 / tick",      [=]{ set(3,r,0x01,0x00); });
            seed("Portamento note-indep, shift 1", [=]{ set(3,r,0x80,0x01); });
            addMenu->addSeparator();
            seed("Funktempo: 9 / 6",             [=]{ set(3,r,0x09,0x06); });
            seed("Funktempo: 24 / 18 (multispd)",[=]{ set(3,r,0x24,0x18); });
        }
        (void)seed;
    };
    buildAddMenu();
    connect(addMenu, &QMenu::aboutToShow, this, [buildAddMenu]{ buildAddMenu(); });

    auto addPlainBtn = [&](const QString &label, const QString &tip, void (TablesView::*slot)()) {
        auto *b = new QPushButton(label, this);
        b->setToolTip(tip);
        b->setMinimumWidth(64);
        connect(b, &QPushButton::clicked, this, slot);
        btnRow->addWidget(b);
        return b;
    };
    addPlainBtn("Insert",  "Insert empty row above cursor (INS)",      &TablesView::insertRow);
    addPlainBtn("Delete",  "Delete row at cursor (DEL)",               &TablesView::deleteRow);
    addPlainBtn("Clear",   "Clear current cell to zero",               &TablesView::clearCell);
    addPlainBtn("Negate",  "Negate speed parameter / relative note (Shift+N)", &TablesView::negate);
    addPlainBtn("Limit→Time","Convert limit-based modulation to time (Shift+L)", &TablesView::limitToTime);
    addPlainBtn("Optimize","Remove unused entries (Shift+O)",          &TablesView::optimize);
    btnRow->addStretch();
    leftCol->addLayout(btnRow);
    root->addLayout(leftCol, 3);

    // ---- Right: legend + cursor decoder + used-by --------------------------
    auto *rightCol = new QVBoxLayout();

    legend_ = new QLabel(this);
    legend_->setTextFormat(Qt::RichText);
    legend_->setWordWrap(true);
    legend_->setMinimumHeight(120);
    legend_->setAutoFillBackground(true);
    legend_->setContentsMargins(10, 8, 10, 8);
    QPalette lp = legend_->palette();
    lp.setColor(QPalette::Window, Theme::C::bgAlt);
    legend_->setPalette(lp);
    rightCol->addWidget(legend_);

    auto *decodeBox = new QGroupBox("Cursor decode", this);
    auto *dLay = new QVBoxLayout(decodeBox);
    decode_ = new QLabel(decodeBox);
    decode_->setWordWrap(true);
    decode_->setMinimumHeight(60);
    decode_->setFont(Theme::monoFont(11));
    QPalette dp = decode_->palette();
    dp.setColor(QPalette::WindowText, Theme::C::highlight);
    decode_->setPalette(dp);
    dLay->addWidget(decode_);
    rightCol->addWidget(decodeBox);

    auto *usedBox = new QGroupBox("Pointers to this row", this);
    auto *uLay = new QVBoxLayout(usedBox);
    usedBy_ = new QLabel(usedBox);
    usedBy_->setWordWrap(true);
    usedBy_->setFont(Theme::monoFont(10));
    QPalette up = usedBy_->palette();
    up.setColor(QPalette::WindowText, Theme::C::text);
    usedBy_->setPalette(up);
    uLay->addWidget(usedBy_);
    rightCol->addWidget(usedBox);

    rightCol->addStretch();
    root->addLayout(rightCol, 2);

    refresh();
}

void TablesView::onTabChanged(int idx) {
    if (updating_ || idx < 0 || idx >= MAX_TABLES) return;
    etnum = idx;
    // Auto-scroll the newly-shown table to the active instrument's
    // pointer row so the user doesn't have to manually find row $XX
    // every time they switch tabs. ins.ptr[t] is the start row of the
    // current instrument's program in table t.
    if (einum >= 1 && einum < MAX_INSTR) {
        int row = instr[einum].ptr[idx];
        if (row >= 0 && row < MAX_TABLELEN) {
            QModelIndex target = models_[idx]->index(row, 0);
            views_[idx]->scrollTo(target, QAbstractItemView::PositionAtCenter);
            views_[idx]->setCurrentIndex(target);
        }
    }
    onCellSelectionChanged();
    emit edited();
}

void TablesView::onCellSelectionChanged() {
    auto *v = views_[etnum];
    if (!v) return;
    auto idx = v->currentIndex();
    if (idx.isValid()) {
        etpos = idx.row();
        etcolumn = idx.column() >= 1 ? idx.column() - 1 : 0;
    }
    updateDecoder();
    updateUsedBy();
    updateLegend();
    // Self-voice the table step under the cursor, reusing the same decode the
    // visual "what it does" panel shows.
    if (Speech::instance().enabled()) {
        unsigned char L = ltable[etnum][etpos], R = rtable[etnum][etpos];
        QString d = decodeCell(etnum, L, R);
        Speech::instance().say(
            QString("%1 step %2. %3")
                .arg(tableName[etnum])
                .arg(etpos + 1, 2, 16, QLatin1Char('0')).toUpper()
                .arg(d.isEmpty() ? QStringLiteral("empty") : d),
            Speech::Priority::Cursor);
    }
}

static QString tableLegendHtml(int t) {
    // Common style tags. Most lines: "[Range] — what it does. Example."
    // The descriptor / 'Examples:' lines used #5A6470 — way too dark
    // against the dock background, so the explanatory text was almost
    // unreadable. Bumped to a brighter neutral grey (#B0BCC8) so the
    // text contrasts properly while staying clearly secondary to the
    // bold opcode entries.
    const QString dim   = "color:#B0BCC8";
    const QString hi    = "color:#FFD93D";
    const QString mono  = "font-family:'JetBrains Mono','monospace'";
    switch (t) {
        case 0: return QString(
            "<p style='margin:0 0 6px 0'><b style='color:#A1C181;font-size:13px'>"
            "Wave table</b> &nbsp;<span style='%1'>"
            "shapes the instrument timbre over time</span></p>"
            "<p style='margin:0 0 4px 0'>"
            "Each row plays one frame (1/50 s PAL). The instrument's <b>Wave-ptr</b>"
            " points at the first row; execution falls through row-by-row until it hits"
            " a jump (<b>$FF</b>)."
            "</p>"
            "<p style='margin:0 0 4px 0'><b style='%2'>Left byte (waveform / opcode)</b></p>"
            "<table style='%3;font-size:11px'>"
            "<tr><td><b>$00</b></td><td>keep the previous waveform</td></tr>"
            "<tr><td><b>$01-$0F</b></td><td>delay 1..15 frames (then continue)</td></tr>"
            "<tr><td><b>$10-$DF</b></td><td>set waveform — bits: noise $80, pulse $40,"
            " saw $20, tri $10, gate $01, test $08, ring $04, sync $02</td></tr>"
            "<tr><td><b>$E0-$EF</b></td><td>inaudible $00-$0F (silent step)</td></tr>"
            "<tr><td><b>$F0-$FE</b></td><td>run pattern command 0XY-EXY with R as param</td></tr>"
            "<tr><td><b>$FF</b></td><td>jump to step in R ($00 = stop wavetable)</td></tr>"
            "</table>"
            "<p style='margin:6px 0 4px 0'><b style='%2'>Right byte (note)</b></p>"
            "<table style='%3;font-size:11px'>"
            "<tr><td><b>$00-$5F</b></td><td>relative note +0..+95 semitones</td></tr>"
            "<tr><td><b>$60-$7F</b></td><td>negative relative -1..-32</td></tr>"
            "<tr><td><b>$80</b></td><td>hold note's frequency unchanged</td></tr>"
            "<tr><td><b>$81-$DF</b></td><td>absolute notes C#0..B-7</td></tr>"
            "</table>"
            "<p style='margin:6px 0 0 0;%1'>Examples:</p>"
            "<p style='margin:0;%3;font-size:11px'>"
            "<b>21 00</b> sawtooth at original note<br>"
            "<b>41 03</b> pulse +3 semitones (minor 3rd arp step)<br>"
            "<b>FF 01</b> loop back to row 1<br>"
            "<b>F6 5A</b> run cmd 6XY (sustain/release) with $5A"
            "</p>"
        ).arg(dim).arg(hi).arg(mono);

        case 1: return QString(
            "<p style='margin:0 0 6px 0'><b style='color:#6FA6CE;font-size:13px'>"
            "Pulse table</b> &nbsp;<span style='%1'>"
            "sweeps pulse-width while a pulse-wave note plays</span></p>"
            "<p style='margin:0 0 4px 0'>"
            "Started by the instrument's <b>Pulse-ptr</b>. Runs in parallel with the"
            " wavetable. Pulse width is 12-bit, $000=thin square..$800=square..$FFF=inverse."
            "</p>"
            "<table style='%2;font-size:11px'>"
            "<tr><td><b>$01-$7F</b></td><td>modulate for L ticks; R is signed 8-bit speed"
            " (positive = wider, negative = narrower)</td></tr>"
            "<tr><td><b>$8X-$FX</b></td><td>set pulse width directly to <b>X|R</b>"
            " (12-bit value — X is hi nybble, R is low byte)</td></tr>"
            "<tr><td><b>$FF</b></td><td>jump to step R ($00 = stop)</td></tr>"
            "</table>"
            "<p style='margin:6px 0 0 0;%1'>Examples:</p>"
            "<p style='margin:0;%2;font-size:11px'>"
            "<b>88 00</b> set pulse to $800 (square)<br>"
            "<b>80 10</b> set pulse to $010 (very thin)<br>"
            "<b>40 E0</b> for 64 ticks, decrease pw by 32/tick<br>"
            "<b>FF 03</b> loop to step 3"
            "</p>"
        ).arg(dim).arg(mono);

        case 2: return QString(
            "<p style='margin:0 0 6px 0'><b style='color:#D89B8C;font-size:13px'>"
            "Filter table</b> &nbsp;<span style='%1'>"
            "drives SID cutoff + resonance over time</span></p>"
            "<p style='margin:0 0 4px 0'>"
            "Started by the instrument's <b>Filter-ptr</b>. <i>Only one channel</i>"
            " should drive the filter at a time. Run cmd <b>B00</b> to disable."
            "</p>"
            "<table style='%2;font-size:11px'>"
            "<tr><td><b>$00</b></td><td>set cutoff to R (8-bit, hi byte of $D415/$D416)</td></tr>"
            "<tr><td><b>$01-$7F</b></td><td>modulate cutoff L ticks at signed speed R</td></tr>"
            "<tr><td><b>$8X</b></td><td>set passband + bitmask: $80 off, <b>$90</b> lowpass,"
            " <b>$A0</b> bandpass, <b>$B0</b> low+band, <b>$C0</b> highpass,"
            " <b>$D0</b> low+high, <b>$E0</b> high+band, <b>$F0</b> all-pass.<br>"
            "R = (resonance hi-nyb) | (channel mask lo-nyb 1=ch1, 2=ch2, 4=ch3)</td></tr>"
            "<tr><td><b>$FF</b></td><td>jump to step R ($00 = stop)</td></tr>"
            "</table>"
            "<p style='margin:6px 0 0 0;%1'>Examples:</p>"
            "<p style='margin:0;%2;font-size:11px'>"
            "<b>90 F1</b> lowpass, res $F, ch1 only<br>"
            "<b>00 40</b> set cutoff to $40<br>"
            "<b>7F 01</b> for 127 ticks, raise cutoff by 1/tick<br>"
            "<b>B00</b> in pattern: disable filter"
            "</p>"
        ).arg(dim).arg(mono);

        case 3: return QString(
            "<p style='margin:0 0 6px 0'><b style='color:#E0C16E;font-size:13px'>"
            "Speed table</b> &nbsp;<span style='%1'>"
            "shared parameter pool for vibrato, portamento, funktempo</span></p>"
            "<p style='margin:0 0 4px 0'>"
            "Pattern commands <b>1XY</b> portamento up, <b>2XY</b> down,"
            " <b>3XY</b> toneporta, <b>4XY</b> vibrato, <b>EXY</b> funktempo"
            " — and the instrument's <b>Vibrato param</b> — all index into this table."
            " <i>No jump opcode</i>. Same row reused across uses."
            "</p>"
            "<table style='%2;font-size:11px'>"
            "<tr><td><b>Vibrato (4XY)</b></td><td>L = ticks before direction flip,"
            " R = pitch delta per tick</td></tr>"
            "<tr><td><b>Portamento (1XY/2XY/3XY)</b></td><td>signed 16-bit value <b>L:R</b>"
            " added to pitch each tick</td></tr>"
            "<tr><td><b>Funktempo (EXY)</b></td><td>L = even-row tempo, R = odd-row tempo</td></tr>"
            "<tr><td><b>L bit $80</b></td><td>enables note-independent calc;"
            " R then specifies the right-shift divisor</td></tr>"
            "</table>"
            "<p style='margin:6px 0 0 0;%1'>Examples:</p>"
            "<p style='margin:0;%2;font-size:11px'>"
            "<b>03 40</b> classic vibrato (depth 40, speed 3)<br>"
            "<b>00 20</b> portamento $0020 / tick<br>"
            "<b>09 06</b> funktempo 9-then-6<br>"
            "<b>83 04</b> note-indep vibrato, divisor 4"
            "</p>"
        ).arg(dim).arg(mono);
    }
    return {};
}

void TablesView::updateLegend() {
    legend_->setText(tableLegendHtml(etnum));
}

void TablesView::updateDecoder() {
    unsigned char L = ltable[etnum][etpos];
    unsigned char R = rtable[etnum][etpos];
    QString head = QString("%1 step $%2   L=$%3  R=$%4")
        .arg(tableName[etnum])
        .arg(etpos + 1, 2, 16, QLatin1Char('0')).toUpper()
        .arg(L, 2, 16, QLatin1Char('0')).toUpper()
        .arg(R, 2, 16, QLatin1Char('0')).toUpper();
    decode_->setText(head + "\n→ " + decodeCell(etnum, L, R));
}

void TablesView::updateUsedBy() {
    QStringList who;
    int row = etpos + 1; // table-pointer values are 1-based
    for (int i = 1; i < MAX_INSTR; i++) {
        if (instr[i].ptr[etnum] == row)
            who << QString("ins $%1").arg(i, 2, 16, QLatin1Char('0')).toUpper();
    }
    if (who.isEmpty()) usedBy_->setText("(no instruments point here)");
    else usedBy_->setText(QString("Used by: %1").arg(who.join(", ")));
}

void TablesView::refresh() {
    if (updating_) return;
    updating_ = true;
    for (int t = 0; t < MAX_TABLES; t++) models_[t]->refresh();
    if (tabs_->currentIndex() != etnum) tabs_->setCurrentIndex(etnum);
    auto *v = views_[etnum];
    if (v) v->setCurrentIndex(models_[etnum]->index(etpos, 1));
    updateDecoder();
    updateUsedBy();
    updateLegend();
    updating_ = false;
}

// ---- Actions hook into tablecommands() by setting key/rawkey/shiftpressed --
static void runTableCommand(int rawkeyVal, int asciiKey, bool shift) {
    rawkey = rawkeyVal;
    key = asciiKey;
    shiftpressed = shift ? 1 : 0;
    tablecommands();
    rawkey = 0;
    key = 0;
    shiftpressed = 0;
}

#include <SDL/SDL_keysym.h>

void TablesView::insertRow() {
    QByteArray b = captureSongSnapshot();
    runTableCommand(SDLK_INSERT, 0, false);
    refresh();
    pushEditIfChanged(this, std::move(b), "Insert table row");
    emit edited();
}
void TablesView::deleteRow() {
    QByteArray b = captureSongSnapshot();
    runTableCommand(SDLK_DELETE, 0, false);
    refresh();
    pushEditIfChanged(this, std::move(b), "Delete table row");
    emit edited();
}
void TablesView::negate() {
    QByteArray b = captureSongSnapshot();
    runTableCommand('n', 'n', true);
    refresh();
    pushEditIfChanged(this, std::move(b), "Negate");
    emit edited();
}
void TablesView::optimize() {
    QByteArray b = captureSongSnapshot();
    runTableCommand('o', 'o', true);
    refresh();
    pushEditIfChanged(this, std::move(b), "Optimize table");
    emit edited();
}
void TablesView::limitToTime() {
    QByteArray b = captureSongSnapshot();
    runTableCommand('l', 'l', true);
    refresh();
    pushEditIfChanged(this, std::move(b), "Limit→Time");
    emit edited();
}
void TablesView::clearCell() {
    QByteArray b = captureSongSnapshot();
    ltable[etnum][etpos] = 0;
    rtable[etnum][etpos] = 0;
    refresh();
    pushEditIfChanged(this, std::move(b), "Clear cell");
    emit edited();
}
void TablesView::insertJump() {
    QByteArray b = captureSongSnapshot();
    ltable[etnum][etpos] = 0xFF;
    rtable[etnum][etpos] = 0;
    refresh();
    pushEditIfChanged(this, std::move(b), "Insert FF jump");
    emit edited();
}
