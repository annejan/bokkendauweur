#include "PatternView.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollBar>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QToolTip>
#include <QHelpEvent>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QMenu>
#include <QContextMenuEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "SdlKeyMap.h"
#include "Theme.h"
#include "UndoStack.h"
#include "MainWindow.h"
#include "InstrColors.h"

extern "C" {
#include "gcommon.h"
#include "gplay.h"
#include "gsid.h"
extern unsigned char pattern[MAX_PATT][MAX_PATTROWS*4+4];
extern int pattlen[MAX_PATT];
extern int epnum[MAX_CHN];
extern int eppos;
extern int epoctave;
extern int epview;
extern int epcolumn;
extern int epchn;
extern int recordmode;
extern int followplay;
extern int epmarkchn;
extern int epmarkstart;
extern int epmarkend;
extern int eseditpos;
extern int esview;
extern int eschn;
extern int esnum;
extern int songlen[MAX_SONGS][MAX_CHN];
extern CHN chn[MAX_CHN];
extern unsigned char sidreg[];
extern unsigned char sidreg2[];
extern int stereo_mode;
extern int hexnybble;
extern int autoadvance;
int isplaying(void);
void patterncommands(void);
void generalcommands(void);
void converthex(void);
void mutechannel(int chnnum);
void countpatternlengths(void);
extern char *notename[];
extern INSTR instr[MAX_INSTR];
}

static int shownChannels() { return stereo_mode ? MAX_CHN : 3; }

// Boomwhacker / handbell palette — used for the optional pitch-coloured
// note column. Indices 0..11 = C..B (semitones from C). The seven natural
// notes get the canonical Boomwhacker colours sampled from the user's
// reference image; sharps (C#, D#, F#, G#, A#) use a dim slate so the
// row still reads as 'a note', just without a pitch class.
struct BoomColor { QColor bg; QColor fg; };
static BoomColor boomwhackerColor(int semitone) {
    switch (((semitone % 12) + 12) % 12) {
    case 0:  return { QColor(0xD0, 0x3A, 0x2C), QColor(0xFF, 0xFF, 0xFF) }; // C  red
    case 2:  return { QColor(0xE6, 0x8A, 0x3E), QColor(0x00, 0x00, 0x00) }; // D  orange
    case 4:  return { QColor(0xF4, 0xD6, 0x3B), QColor(0x00, 0x00, 0x00) }; // E  yellow
    case 5:  return { QColor(0x3F, 0x9B, 0x3F), QColor(0xFF, 0xFF, 0xFF) }; // F  green
    case 7:  return { QColor(0x7C, 0xC9, 0xF0), QColor(0x00, 0x00, 0x00) }; // G  light blue
    case 9:  return { QColor(0x2B, 0x3D, 0xA0), QColor(0xFF, 0xFF, 0xFF) }; // A  dark blue
    case 11: return { QColor(0x5A, 0x28, 0x95), QColor(0xFF, 0xFF, 0xFF) }; // B  purple
    default: return { QColor(0x55, 0x60, 0x70), QColor(0xE1, 0xE5, 0xEA) }; // sharps
    }
}

PatternView::PatternView(QWidget *parent) : QAbstractScrollArea(parent) {
    QFont mono = Theme::monoFont(11);
    setFont(mono);
    QFontMetrics fm(mono);
    rowHeight = fm.height() + 3;
    colWidth = fm.horizontalAdvance('0');
    rowNumW_ = 6 * colWidth;
    chnW_ = 16 * colWidth;   // widened so header (Ch P L M) fits without overlap
    // 22 px so the SID waveform indicator glyphs (T S P N y r F) painted
    // at the right edge of the strip have enough vertical room — at the
    // old 14 px the text was getting clipped by the toolbar above.
    vuStripH_ = 22;
    scopeStripH_ = 32;
    headerStripH_ = rowHeight + 4;
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Theme::applyDarkPalette(viewport());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusPolicy(Qt::StrongFocus);
    for (auto &row : scope_) row.fill(0);
    updateScrollRange();
}

int PatternView::visibleRows() const {
    return (viewport()->height() - gridTopOffset()) / rowHeight;
}

int PatternView::gridTopOffset() const {
    return vuStripH_ + scopeStripH_ + headerStripH_;
}

void PatternView::updateScrollRange() {
    int maxRows = 0;
    for (int c = 0; c < shownChannels(); c++)
        if (pattlen[epnum[c]] > maxRows) maxRows = pattlen[epnum[c]];
    verticalScrollBar()->setRange(0, std::max(0, maxRows - visibleRows()));
    verticalScrollBar()->setPageStep(visibleRows());
}

void PatternView::resizeEvent(QResizeEvent *) {
    updateScrollRange();
}

void PatternView::refresh() {
    bool playing = isplaying() != 0;

    // Snapshot chn[c].pattptr/4 ONCE per refresh. paintEvent reads
    // playRow_[c] — so the editor cursor row (eppos = playRow_[epchn] in
    // follow-play) and the per-channel red play-row highlight read the
    // same chn[].pattptr value. Without this, the audio thread advanced
    // pattptr between the cursor-pull in refresh() and the play-row read
    // in paintEvent — the red row appeared one step ahead of the cursor.
    for (int c = 0; c < MAX_CHN; c++) playRow_[c] = chn[c].pattptr / 4;

    if (followplay && playing) {
        for (int c = 0; c < shownChannels(); c++) {
            if (chn[c].advance) epnum[c] = chn[c].pattnum;
            int newpos = playRow_[c];
            if (newpos > pattlen[epnum[c]]) newpos = pattlen[epnum[c]];
            if (c == epchn) eppos = newpos;
            int songpos = chn[c].songptr - 1;
            if (songpos < 0) songpos = 0;
            if (songpos > songlen[esnum][c]) songpos = songlen[esnum][c];
            if (c == eschn && chn[c].advance) eseditpos = songpos;
        }
    }

    updateScrollRange();
    int rowOffset = verticalScrollBar()->value();
    int rows = visibleRows();
    // Only auto-pull the scrollbar to keep the cursor visible when the
    // cursor actually moved this refresh — otherwise a mouse-wheel scroll
    // past eppos kept getting yanked back because the periodic refresh()
    // (tickScope at ~50 Hz) re-evaluated the visibility clause every tick.
    bool cursorMoved = (eppos != lastEppos_) || (epchn != lastEpchn_);
    if (followplay && playing) {
        int prow = playRow_[epchn];
        int center = std::max(0, prow - rows / 2);
        verticalScrollBar()->setValue(center);
    } else if (cursorMoved && rows > 0) {
        if (eppos < rowOffset) verticalScrollBar()->setValue(eppos);
        else if (eppos >= rowOffset + rows)
            verticalScrollBar()->setValue(eppos - rows + 1);
    }
    lastEppos_ = eppos;
    lastEpchn_ = epchn;
    viewport()->update();
}

void PatternView::tickScope() {
    unsigned char levels[MAX_CHN] = {0};
    sid_getlevels(levels);
    for (int c = 0; c < MAX_CHN; c++) scope_[c][scopeHead_] = levels[c];
    // Capture filter cutoff alongside the envelope so paintEvent can
    // draw a second yellow trace on top of the scope curve when the
    // voice is routed through the filter. Cutoff is the 11-bit value
    // formed from sidreg[$15] low 3 bits + sidreg[$16] << 3; we scale
    // it into the same 0..255 range the envelope uses. Voices 0..2 read
    // sidreg, 3..5 read sidreg2. Filter is per-SID (not per-voice), so
    // the cutoff value is the same across all routed voices of one chip;
    // the per-channel store lets us key paint on $17 voice-route bits.
    auto cap = [&](const unsigned char *regs, int c) {
        int v = c % 3;
        unsigned char filt = regs[0x17];
        if ((filt >> v) & 1) {
            unsigned cutoff = ((unsigned)regs[0x16] << 3) | (regs[0x15] & 7);
            filtScope_[c][scopeHead_] = (unsigned char)(cutoff >> 3); // 0..255
        } else {
            filtScope_[c][scopeHead_] = 0;
        }
    };
    for (int c = 0; c < 3; c++) cap(sidreg, c);
    for (int c = 3; c < MAX_CHN; c++) cap(sidreg2, c);
    scopeHead_ = (scopeHead_ + 1) % kScopeLen;
    viewport()->update();
}

bool PatternView::event(QEvent *e) {
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent*>(e);
        QPoint pos = he->pos();
        // Row-number column hover: show pattern step in hex + dec, the row
        // type (beat / downbeat) and pattern length context.
        if (pos.x() < rowNumW_ && pos.y() >= gridTopOffset()) {
            int row = verticalScrollBar()->value()
                      + (pos.y() - gridTopOffset()) / rowHeight;
            QString tag;
            if (row % 16 == 0)     tag = "downbeat";
            else if (row % 4 == 0) tag = "beat";
            else                   tag = "step";
            int patnum = epnum[epchn];
            // Tooltip also advertises the Backspace shortcut — it silences
            // a note that's still ringing on the active channel without
            // inserting a row, and it's not bound anywhere else in the
            // pattern editor.
            QToolTip::showText(he->globalPos(),
                QString("Row $%1 (%2) — %3, pattern $%4 length $%5\n"
                        "Backspace: silence note on Ch%6")
                    .arg(row, 2, 16, QLatin1Char('0')).toUpper()
                    .arg(row)
                    .arg(tag)
                    .arg(patnum, 2, 16, QLatin1Char('0')).toUpper()
                    .arg(pattlen[patnum], 2, 16, QLatin1Char('0')).toUpper()
                    .arg(epchn + 1),
                this);
            return true;
        }
        QToolTip::hideText();
    }
    return QAbstractScrollArea::event(e);
}

void PatternView::keyPressEvent(QKeyEvent *e) {
    // Nav keys don't mutate; everything else gets wrapped in an undo command.
    int k = e->key();
    // Backspace = silence the currently-playing note on the active channel
    // without inserting a pattern row or moving the edit cursor. The C core
    // exposes releasenote(chnnum) which writes the gate-off bit on the next
    // playroutine tick — same code path as the existing 'Silence' button in
    // the instrument editor. We swallow the key before patterncommands() so
    // it can't be picked up as a pattern edit, and skip the snapshot/undo
    // wrap because nothing about the song data changes.
    if (k == Qt::Key_Backspace && !(e->modifiers() & Qt::ControlModifier)) {
        releasenote(epchn);
        e->accept();
        return;
    }

    // Selection + clipboard shortcuts. These take precedence over the
    // C-core pattern key handler so Ctrl+C / Ctrl+X / Ctrl+V can't be
    // mistaken for note input.
    if (e->modifiers() & Qt::ControlModifier) {
        if (k == Qt::Key_C) { copySelectionToClipboard(false); e->accept(); return; }
        if (k == Qt::Key_X) { copySelectionToClipboard(true);  e->accept(); return; }
        if (k == Qt::Key_V) { pasteFromClipboard(false);       e->accept(); return; }
        if (k == Qt::Key_A) { selectAllInChannel();            e->accept(); return; }
        if (k == Qt::Key_Delete) {
            if (hasSelection()) removeSelectedRows();
            e->accept();
            return;
        }
        if (k == Qt::Key_Backspace) {
            deleteRowAtCursor();
            e->accept();
            return;
        }
        // Ctrl+Shift+Up / Down -> transpose selection by an octave.
        // Plain Ctrl+Up / Down stays free for any future per-row nudge,
        // and Shift+arrows stay reserved for the C core's existing
        // extend-cursor bindings.
        if (e->modifiers() & Qt::ShiftModifier) {
            if (k == Qt::Key_Up)   { transposeSelection(12);  e->accept(); return; }
            if (k == Qt::Key_Down) { transposeSelection(-12); e->accept(); return; }
        }
        // Ctrl+0 / Ctrl+1 / Ctrl+2 -> set EDIT SKIP (autoadvance) to the
        // matching value. Protracker convention; the C core caps
        // autoadvance at 0..2:
        //   0 = stay put after a note / hex digit
        //   1 = step one row after a note
        //   2 = step one row after EVERY column write (note + hex)
        // Useful when entering dense patterns with the keyboard.
        if (k == Qt::Key_0 || k == Qt::Key_1 || k == Qt::Key_2) {
            int newSkip = k - Qt::Key_0;
            autoadvance = newSkip;
            const char *label = (newSkip == 0) ? "EDIT SKIP 0 (no advance)"
                              : (newSkip == 1) ? "EDIT SKIP 1 (advance after note)"
                                               : "EDIT SKIP 2 (advance after every column)";
            // Push the message via the parent's status strip if we can
            // reach it. window() walks up to MainWindow.
            if (auto *mw = window()) {
                if (auto *strip = mw->findChild<QWidget*>("statusStripMessage")) {
                    (void)strip; // no public setter — fall through to update
                }
            }
            refresh();
            e->accept();
            return;
        }
    }
    if (k == Qt::Key_Delete && hasSelection()) {
        clearSelectedCells();
        e->accept();
        return;
    }
    if (k == Qt::Key_Insert) {
        insertRowAtCursor();
        e->accept();
        return;
    }
    if (k == Qt::Key_Escape && hasSelection()) {
        clearSelection();
        e->accept();
        return;
    }
    bool isNav = (k == Qt::Key_Up || k == Qt::Key_Down
                  || k == Qt::Key_Left || k == Qt::Key_Right
                  || k == Qt::Key_PageUp || k == Qt::Key_PageDown
                  || k == Qt::Key_Home || k == Qt::Key_End
                  || k == Qt::Key_Tab || k == Qt::Key_Backtab);
    QByteArray before;
    if (!isNav) before = captureSongSnapshot();
    setGoatKeys(e);
    // Alternative keyboard layouts (Dvorak, AZERTY, Colemak, ...) shuffle
    // the QWERTY note row, so e->key() lands on the wrong SDL keysym and
    // notekeytbl[] picks up the wrong note. Override rawkey from the
    // physical scancode when the scan code corresponds to a known QWERTY
    // note position. key / shiftpressed stay derived from the logical
    // key so hex-digit entry still tracks the user's layout.
    if (physicalKeyLayout_) {
        int qwertySDL = qwertyScancodeToNoteSDL((int)e->nativeScanCode());
        if (qwertySDL != 0) rawkey = qwertySDL;
    }
    converthex();
    int preCol = epcolumn;
    int prePos = eppos;
    patterncommands();
    // 2-digit hex field UX: typing the first nybble at instr-hi (col 1) or
    // param-hi (col 4) should advance INTO the next nybble instead of
    // dropping to the next row. Typing the lo nybble (col 2 or 5) advances
    // a row and resets back to the hi nybble — so the user can stream byte
    // pairs at the row cadence. We let gpattern.c run its default advance
    // and then patch the result here, instead of forking the C core.
    if (hexnybble >= 0 && recordmode && autoadvance < 2 && preCol > 0) {
        if (preCol == 1 || preCol == 4) {
            // Hi nybble was just written; gpattern advanced eppos. Roll it
            // back and step epcolumn to the lo nybble.
            eppos = prePos;
            epcolumn = preCol + 1;
        } else if (preCol == 2 || preCol == 5) {
            // Lo nybble done; row already advanced, snap back to the hi
            // nybble so the next keystroke starts a fresh byte.
            epcolumn = preCol - 1;
        }
    }
    // generalcommands carries the cross-editor hotkeys: + / - cycle
    // instrument, * / / cycle octave, ; and ' alternates. Without this the
    // Qt build couldn't reach any of those from the pattern view.
    generalcommands();
    clearGoatKeys();
    refresh();
    if (!isNav) pushEditIfChanged(this, std::move(before), "Pattern edit");
    emit patternEdited();
}

int PatternView::channelAtX(int x) const {
    int rx = x - rowNumW_;
    if (rx < 0) return -1;
    int c = rx / chnW_;
    if (c < 0 || c >= shownChannels()) return -1;
    return c;
}

int PatternView::rowAtY(int y) const {
    int gy = y - gridTopOffset();
    if (gy < 0) return -1;
    return verticalScrollBar()->value() + gy / rowHeight;
}

PatternView::SelRect PatternView::normalisedSelection() const {
    SelRect r;
    r.chLo  = qMin(selChAnchor_,  selChCursor_);
    r.chHi  = qMax(selChAnchor_,  selChCursor_);
    r.rowLo = qMin(selRowAnchor_, selRowCursor_);
    r.rowHi = qMax(selRowAnchor_, selRowCursor_);
    return r;
}

QRect PatternView::cellRect(int chan, int row) const {
    int rowOffset = verticalScrollBar()->value();
    int y = gridTopOffset() + (row - rowOffset) * rowHeight;
    int x = rowNumW_ + chan * chnW_;
    return QRect(x, y, chnW_, rowHeight);
}

// MIME type for our binary-precise clipboard payload. Plain text fallback
// also goes on the clipboard so users can paste pattern selections into a
// text editor, but pasting back in here prefers the binary form.
static constexpr const char *kPatternMime = "application/x-goattracker2-pattern";

static QString cellToText(unsigned char note, unsigned char ins,
                          unsigned char cmd,  unsigned char param) {
    extern char *notename[];
    QString n;
    if (note == REST)            n = "...";
    else if (note == KEYOFF)     n = "===";
    else if (note == KEYON)      n = "+++";
    else if (note >= FIRSTNOTE && note <= LASTNOTE)
        n = QString::fromLatin1(notename[note - FIRSTNOTE]);
    else n = "...";
    QString i = ins ? QString("%1").arg(ins, 2, 16, QLatin1Char('0')).toUpper()
                    : QString("..");
    QString c = (cmd || param)
        ? QString("%1%2").arg(cmd, 1, 16, QLatin1Char('0'))
                         .arg(param, 2, 16, QLatin1Char('0')).toUpper()
        : QString("...");
    return QString("%1 %2 %3").arg(n).arg(i).arg(c);
}

void PatternView::copySelectionToClipboard(bool cut) {
    if (!hasSelection()) return;
    SelRect s = normalisedSelection();
    int channels = s.chHi - s.chLo + 1;
    int rows     = s.rowHi - s.rowLo + 1;

    // Binary-ish JSON payload — cells[r][c] = [note, ins, cmd, param].
    QJsonArray cells;
    QString textLines;
    for (int r = 0; r < rows; r++) {
        QJsonArray rowArr;
        QStringList textCells;
        int srcRow = s.rowLo + r;
        for (int c = 0; c < channels; c++) {
            int chan = s.chLo + c;
            int patnum = epnum[chan];
            QJsonArray cell;
            if (srcRow >= 0 && srcRow < pattlen[patnum]) {
                const unsigned char *p = &pattern[patnum][srcRow * 4];
                cell.append(p[0]); cell.append(p[1]);
                cell.append(p[2]); cell.append(p[3]);
                textCells << cellToText(p[0], p[1], p[2], p[3]);
            } else {
                cell.append(REST); cell.append(0); cell.append(0); cell.append(0);
                textCells << "... .. ...";
            }
            rowArr.append(cell);
        }
        cells.append(rowArr);
        textLines += textCells.join(" | ") + "\n";
    }
    QJsonObject doc;
    doc["type"]     = "goattracker2-pattern-selection";
    doc["version"]  = 1;
    doc["channels"] = channels;
    doc["rows"]     = rows;
    doc["cells"]    = cells;
    QJsonDocument jd(doc);

    auto *mime = new QMimeData();
    mime->setData(kPatternMime, jd.toJson(QJsonDocument::Compact));
    mime->setText(QString("# GoatTracker Qt pattern selection %1ch x %2r\n")
                      .arg(channels).arg(rows) + textLines);
    QApplication::clipboard()->setMimeData(mime);

    if (cut) clearSelectedCells();
}

void PatternView::clearSelectedCells() {
    if (!hasSelection()) return;
    QByteArray before = captureSongSnapshot();
    SelRect s = normalisedSelection();
    for (int c = s.chLo; c <= s.chHi; c++) {
        int patnum = epnum[c];
        for (int r = s.rowLo; r <= s.rowHi; r++) {
            if (r < 0 || r >= pattlen[patnum]) continue;
            unsigned char *p = &pattern[patnum][r * 4];
            p[0] = REST; p[1] = 0; p[2] = 0; p[3] = 0;
        }
    }
    countpatternlengths();
    refresh();
    pushEditIfChanged(this, std::move(before), "Clear cells");
    emit patternEdited();
}

void PatternView::removeSelectedRows() {
    if (!hasSelection()) return;
    QByteArray before = captureSongSnapshot();
    SelRect s = normalisedSelection();
    int rowsToRemove = s.rowHi - s.rowLo + 1;
    for (int c = s.chLo; c <= s.chHi; c++) {
        int patnum = epnum[c];
        int plen = pattlen[patnum];
        // Shift rows [s.rowHi+1 .. plen-1] up by rowsToRemove.
        for (int r = s.rowLo; r + rowsToRemove < plen; r++) {
            unsigned char *dst = &pattern[patnum][r * 4];
            const unsigned char *src = &pattern[patnum][(r + rowsToRemove) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
        // Tail rows become empty.
        for (int r = plen - rowsToRemove; r < plen; r++) {
            if (r < 0) continue;
            unsigned char *p = &pattern[patnum][r * 4];
            p[0] = REST; p[1] = 0; p[2] = 0; p[3] = 0;
        }
    }
    countpatternlengths();
    clearSelection();
    refresh();
    pushEditIfChanged(this, std::move(before), "Remove rows");
    emit patternEdited();
}

void PatternView::pasteFromClipboard(bool repeatFill) {
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime) return;
    QByteArray raw;
    if (mime->hasFormat(kPatternMime)) {
        raw = mime->data(kPatternMime);
    } else {
        return;  // text-only paste is lossy; skip for now
    }
    QJsonParseError err;
    QJsonDocument jd = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !jd.isObject()) return;
    QJsonObject doc = jd.object();
    if (doc.value("type").toString() != "goattracker2-pattern-selection") return;
    int srcCh   = doc.value("channels").toInt();
    int srcRows = doc.value("rows").toInt();
    QJsonArray cells = doc.value("cells").toArray();
    if (srcCh <= 0 || srcRows <= 0) return;

    QByteArray before = captureSongSnapshot();
    int startRow = eppos;
    int startCh  = epchn;
    for (int c = 0; c < srcCh; c++) {
        int chan = startCh + c;
        if (chan >= shownChannels()) break;
        int patnum = epnum[chan];
        int plen = pattlen[patnum];
        int targetRows = repeatFill ? qMax(0, plen - startRow) : srcRows;
        for (int r = 0; r < targetRows; r++) {
            int dstRow = startRow + r;
            if (dstRow >= plen) break;
            int srcRow = r % srcRows;
            QJsonArray rowArr = cells[srcRow].toArray();
            if (rowArr.size() <= c) continue;
            QJsonArray cell = rowArr[c].toArray();
            if (cell.size() < 4) continue;
            unsigned char *p = &pattern[patnum][dstRow * 4];
            p[0] = (unsigned char)cell[0].toInt();
            p[1] = (unsigned char)cell[1].toInt();
            p[2] = (unsigned char)cell[2].toInt();
            p[3] = (unsigned char)cell[3].toInt();
        }
    }
    countpatternlengths();
    refresh();
    pushEditIfChanged(this, std::move(before),
                      repeatFill ? "Paste (repeat)" : "Paste");
    emit patternEdited();
}

void PatternView::insertRowAtCursor() {
    int chan = epchn;
    int patnum = epnum[chan];
    int plen = pattlen[patnum];
    int row = eppos;
    if (row < 0 || row > plen) return;
    QByteArray before = captureSongSnapshot();

    if (insertGrows_) {
        if (plen >= MAX_PATTROWS) return;
        // Shift ENDPATT marker down by one; rows [row..plen-1] -> [row+1..plen].
        for (int r = plen; r > row; r--) {
            unsigned char *dst = &pattern[patnum][r * 4];
            const unsigned char *src = &pattern[patnum][(r - 1) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
        // Replant ENDPATT at the new plen.
        unsigned char *e = &pattern[patnum][(plen + 1) * 4];
        e[0] = ENDPATT; e[1] = 0; e[2] = 0; e[3] = 0;
    } else {
        // Push-off mode: rows [row..plen-2] shift down; row plen-1 dropped.
        for (int r = plen - 1; r > row; r--) {
            unsigned char *dst = &pattern[patnum][r * 4];
            const unsigned char *src = &pattern[patnum][(r - 1) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
    }
    unsigned char *p = &pattern[patnum][row * 4];
    p[0] = REST; p[1] = 0; p[2] = 0; p[3] = 0;
    countpatternlengths();
    refresh();
    pushEditIfChanged(this, std::move(before), "Insert row");
    emit patternEdited();
}

void PatternView::deleteRowAtCursor() {
    int chan = epchn;
    int patnum = epnum[chan];
    int plen = pattlen[patnum];
    int row = eppos;
    if (row < 0 || row >= plen) return;
    QByteArray before = captureSongSnapshot();

    if (insertGrows_) {
        // Symmetric to grow: shrink pattern by 1, drop the cursor row.
        if (plen <= 1) return;
        for (int r = row; r < plen - 1; r++) {
            unsigned char *dst = &pattern[patnum][r * 4];
            const unsigned char *src = &pattern[patnum][(r + 1) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
        unsigned char *e = &pattern[patnum][(plen - 1) * 4];
        e[0] = ENDPATT; e[1] = 0; e[2] = 0; e[3] = 0;
    } else {
        // Push-up mode: rows [row+1..plen-1] shift up; tail becomes REST.
        for (int r = row; r < plen - 1; r++) {
            unsigned char *dst = &pattern[patnum][r * 4];
            const unsigned char *src = &pattern[patnum][(r + 1) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
        }
        unsigned char *p = &pattern[patnum][(plen - 1) * 4];
        p[0] = REST; p[1] = 0; p[2] = 0; p[3] = 0;
    }
    countpatternlengths();
    refresh();
    pushEditIfChanged(this, std::move(before), "Delete row");
    emit patternEdited();
}

void PatternView::transposeSelection(int semis) {
    if (!hasSelection()) return;
    QByteArray before = captureSongSnapshot();
    SelRect s = normalisedSelection();
    bool changed = false;
    for (int c = s.chLo; c <= s.chHi; c++) {
        int patnum = epnum[c];
        for (int r = s.rowLo; r <= s.rowHi; r++) {
            if (r < 0 || r >= pattlen[patnum]) continue;
            unsigned char *p = &pattern[patnum][r * 4];
            // Only real pitched notes shift. REST / KEYOFF / KEYON / 0
            // stay put — transposing a release would silently rewrite
            // it to a note byte and break the song.
            if (p[0] >= FIRSTNOTE && p[0] <= LASTNOTE) {
                int n = (int)p[0] + semis;
                if (n < FIRSTNOTE) n = FIRSTNOTE;
                if (n > LASTNOTE)  n = LASTNOTE;
                if ((unsigned char)n != p[0]) {
                    p[0] = (unsigned char)n;
                    changed = true;
                }
            }
        }
    }
    if (changed) {
        refresh();
        pushEditIfChanged(this, std::move(before),
                          semis > 0 ? "Transpose up" : "Transpose down");
        emit patternEdited();
    }
}

void PatternView::selectAllInChannel() {
    int patnum = epnum[epchn];
    selChAnchor_ = selChCursor_ = epchn;
    selRowAnchor_ = 0;
    selRowCursor_ = qMax(0, pattlen[patnum] - 1);
    viewport()->update();
}

void PatternView::selectAllRows() {
    selChAnchor_ = 0;
    selChCursor_ = shownChannels() - 1;
    int maxLen = 0;
    for (int c = 0; c < shownChannels(); c++)
        if (pattlen[epnum[c]] > maxLen) maxLen = pattlen[epnum[c]];
    selRowAnchor_ = 0;
    selRowCursor_ = qMax(0, maxLen - 1);
    viewport()->update();
}

void PatternView::mousePressEvent(QMouseEvent *e) {
    const int y = e->pos().y();

    // Channel header strip — click on pattern# / mute toggle
    int headerY = vuStripH_ + scopeStripH_;
    if (y >= headerY && y < headerY + headerStripH_) {
        int c = channelAtX(e->pos().x());
        if (c >= 0) {
            int xInChan = e->pos().x() - (rowNumW_ + c * chnW_);
            int muteX = chnW_ - 32;
            if (xInChan >= muteX) {
                mutechannel(c);
                refresh();
                emit patternEdited();
                return;
            }
            // Click pattern# field — let user pick a new pattern
            if (xInChan >= 4 * colWidth && xInChan < 8 * colWidth) {
                bool ok = false;
                int n = QInputDialog::getInt(this, "Pattern number",
                    QString("Channel %1 pattern (hex):").arg(c + 1),
                    epnum[c], 0, MAX_PATT - 1, 1, &ok);
                if (ok) {
                    epnum[c] = n;
                    refresh();
                    emit patternEdited();
                }
                return;
            }
            // Click L## (length) — let user pick a new length in hex.
            if (xInChan >= 8 * colWidth && xInChan < chnW_ - 32 - 8) {
                int p = epnum[c];
                bool ok = false;
                QString cur = QString("%1").arg(pattlen[p], 0, 16).toUpper();
                QString s = QInputDialog::getText(this, "Pattern length",
                    QString("Pattern $%1 length (hex 1..%2):")
                        .arg(p, 2, 16, QLatin1Char('0')).toUpper()
                        .arg(MAX_PATTROWS, 0, 16).toUpper(),
                    QLineEdit::Normal, cur, &ok);
                if (ok) {
                    int newLen = s.trimmed().toInt(nullptr, 16);
                    if (newLen < 1) newLen = 1;
                    if (newLen > MAX_PATTROWS) newLen = MAX_PATTROWS;
                    int curLen = pattlen[p];
                    if (newLen != curLen) {
                        if (newLen > curLen) {
                            // Grow: REST-pad current ENDPATT slot through
                            // newLen-1, then plant ENDPATT at newLen.
                            for (int r = curLen; r < newLen; r++) {
                                pattern[p][r*4+0] = REST;
                                pattern[p][r*4+1] = 0;
                                pattern[p][r*4+2] = 0;
                                pattern[p][r*4+3] = 0;
                            }
                        }
                        pattern[p][newLen*4+0] = ENDPATT;
                        pattern[p][newLen*4+1] = 0;
                        pattern[p][newLen*4+2] = 0;
                        pattern[p][newLen*4+3] = 0;
                        countpatternlengths();
                        refresh();
                        emit patternEdited();
                    }
                }
                return;
            }
            // Plain click on header — switch active channel
            epchn = c;
            epcolumn = 0;
            refresh();
            emit patternEdited();
            return;
        }
    }

    // Click in row-number column: select that whole row across all
    // shown channels. Drag updates the row span across the column.
    if (e->pos().x() < rowNumW_ && y >= gridTopOffset()) {
        int row = rowAtY(y);
        if (row < 0) row = 0;
        if (row >= MAX_PATTROWS) row = MAX_PATTROWS - 1;
        eppos = row;
        selChAnchor_  = 0;
        selChCursor_  = shownChannels() - 1;
        selRowAnchor_ = selRowCursor_ = row;
        dragActive_ = (e->button() == Qt::LeftButton);
        refresh();
        emit patternEdited();
        return;
    }

    // Grid area
    if (y >= gridTopOffset()) {
        int row = verticalScrollBar()->value()
                  + (y - gridTopOffset()) / rowHeight;
        if (row < 0) row = 0;
        if (row >= MAX_PATTROWS) row = MAX_PATTROWS - 1;
        eppos = row;
        int c = channelAtX(e->pos().x());
        if (c >= 0) {
            epchn = c;
            // Map x-within-channel to epcolumn. Layout mirrors paintEvent's
            // colRect math: note=3w@0, instr=2w@4w, cmd=1w@7w, param=2w@8w.
            int xInChan = e->pos().x() - (rowNumW_ + c * chnW_);
            int col = xInChan / colWidth;
            // Paint code offsets text by +colWidth (tx = x + colWidth), so
            // the visible digit columns are shifted one col right of the raw
            // channel-local x. Hit-box layout (col = xInChan/colWidth):
            //   1..4  note (3 chars at cols 1-3, +slack)
            //   5     instr hi    (col 5)
            //   6     instr lo    (col 6)
            //   7..8  cmd         (col 8 + slack on the left)
            //   9     param hi    (col 9)
            //   >=10  param lo    (col 10)
            if      (col < 5)  epcolumn = 0;
            else if (col == 5) epcolumn = 1;
            else if (col == 6) epcolumn = 2;
            else if (col <= 8) epcolumn = 3;
            else if (col == 9) epcolumn = 4;
            else               epcolumn = 5;

            // Selection bookkeeping. Shift-click extends an existing
            // selection's cursor end; plain click starts a fresh anchor
            // at the current cell and lets mouseMoveEvent grow it.
            if (e->button() == Qt::LeftButton) {
                if (e->modifiers() & Qt::ShiftModifier && hasSelection()) {
                    selChCursor_  = c;
                    selRowCursor_ = row;
                } else {
                    selChAnchor_  = selChCursor_  = c;
                    selRowAnchor_ = selRowCursor_ = row;
                }
                dragActive_ = true;
            }
        }
        refresh();
        emit patternEdited();
    }
}

void PatternView::mouseMoveEvent(QMouseEvent *e) {
    if (!dragActive_) return;
    if (!(e->buttons() & Qt::LeftButton)) return;
    int row = rowAtY(e->pos().y());
    int chan = channelAtX(e->pos().x());
    // If the cursor leaves the grid sideways or upward, clamp instead of
    // dropping the drag — the user can still finish the rectangle by
    // continuing to drag.
    if (row < 0) row = verticalScrollBar()->value();
    if (chan < 0) {
        if (e->pos().x() < rowNumW_) chan = 0;
        else                          chan = shownChannels() - 1;
    }
    if (row >= MAX_PATTROWS) row = MAX_PATTROWS - 1;
    if (row != selRowCursor_ || chan != selChCursor_) {
        selRowCursor_ = row;
        selChCursor_  = chan;
        viewport()->update();
    }
}

void PatternView::mouseReleaseEvent(QMouseEvent *e) {
    Q_UNUSED(e);
    dragActive_ = false;
    // Keep the 1-cell selection from a plain click so a subsequent
    // Shift+click can extend it into a range. Escape clears.
}

void PatternView::contextMenuEvent(QContextMenuEvent *e) {
    QMenu menu(this);
    const bool sel = hasSelection();
    const QMimeData *cm = QApplication::clipboard()->mimeData();
    const bool clip = cm && cm->hasFormat(kPatternMime);

    auto *copy = menu.addAction("&Copy\tCtrl+C");
    copy->setEnabled(sel);
    connect(copy, &QAction::triggered, this, [this]{ copySelectionToClipboard(false); });

    auto *cut = menu.addAction("Cu&t (copy + clear cells)\tCtrl+X");
    cut->setEnabled(sel);
    connect(cut, &QAction::triggered, this, [this]{ copySelectionToClipboard(true); });

    auto *paste = menu.addAction("&Paste\tCtrl+V");
    paste->setEnabled(clip);
    connect(paste, &QAction::triggered, this, [this]{ pasteFromClipboard(false); });

    auto *pasteRep = menu.addAction("Paste &repeating to end of pattern");
    pasteRep->setEnabled(clip);
    connect(pasteRep, &QAction::triggered, this, [this]{ pasteFromClipboard(true); });

    menu.addSeparator();

    auto *clearC = menu.addAction("Clear cells (&keep rows)\tDel");
    clearC->setEnabled(sel);
    connect(clearC, &QAction::triggered, this, [this]{ clearSelectedCells(); });

    auto *removeR = menu.addAction("Remove rows (&shift up)\tCtrl+Del");
    removeR->setEnabled(sel);
    connect(removeR, &QAction::triggered, this, [this]{ removeSelectedRows(); });

    menu.addSeparator();

    auto *insR = menu.addAction("Insert row at cursor\tIns");
    connect(insR, &QAction::triggered, this, [this]{ insertRowAtCursor(); });
    auto *delR = menu.addAction("Delete row at cursor\tCtrl+Backspace");
    connect(delR, &QAction::triggered, this, [this]{ deleteRowAtCursor(); });

    menu.addSeparator();

    auto *selChan = menu.addAction("Select entire channel\tCtrl+A");
    connect(selChan, &QAction::triggered, this, [this]{ selectAllInChannel(); });
    auto *selAll = menu.addAction("Select all channels");
    connect(selAll, &QAction::triggered, this, [this]{ selectAllRows(); });

    menu.addSeparator();

    auto *trUp = menu.addAction("Transpose octave &up (+12)\tCtrl+Shift+Up");
    trUp->setEnabled(sel);
    connect(trUp, &QAction::triggered, this, [this]{ transposeSelection(12); });
    auto *trDn = menu.addAction("Transpose octave &down (-12)\tCtrl+Shift+Down");
    trDn->setEnabled(sel);
    connect(trDn, &QAction::triggered, this, [this]{ transposeSelection(-12); });

    menu.exec(e->globalPos());
}

void PatternView::paintEvent(QPaintEvent *) {
    QPainter p(viewport());
    p.setFont(font());

    const int rowOffset = verticalScrollBar()->value();
    const int W = viewport()->width();

    // ---- VU bars strip --------------------------------------------------
    const int vuPad = 4;
    const int vuH = vuStripH_ - vuPad * 2;
    unsigned char levels[MAX_CHN] = {0};
    sid_getlevels(levels);
    for (int c = 0; c < shownChannels(); c++) {
        int x = rowNumW_ + c * chnW_ + 2;
        int w = chnW_ - 6;
        QRect frame(x, vuPad, w, vuH);
        p.fillRect(frame, Theme::C::vuBg);
        p.setPen(Theme::C::sep);
        p.drawRect(frame);
        if (chn[c].mute) {
            // Bright red so the muted channel actually pops out of the VU
            // bar; the previous dark muted red blended into the strip
            // background. Bold for extra weight in case the rest of the
            // VU strip is busy.
            QFont mf = p.font();
            mf.setBold(true);
            p.setFont(mf);
            p.setPen(QColor(255, 80, 80));
            p.drawText(QPoint(x + 4, vuPad + vuH - 2), "MUTE");
            p.setFont(font());
            continue;
        }
        // Waveform / flag indicators sit in a fixed-width block at the
        // right edge, spanning the FULL height of vu strip + scope strip
        // (rendered after both loops below). VU bar shrinks by indW so
        // the meter fill never paints over the indicator block. When
        // the View ▸ SID indicators toggle is off, indW=0 and the
        // meter reclaims the full cell width.
        const int indColW = 16;
        const int indW = sidIndOn_ ? indColW * 2 : 0;
        const int meterW = qMax(8, w - indW - 4);
        int filled = (int)((double)levels[c] / 255.0 * (meterW - 2));
        if (filled > 0) {
            int seg1 = (int)((meterW - 2) * 0.6);
            int seg2 = (int)((meterW - 2) * 0.9);
            int rem = filled;
            int xx = x + 1;
            int g = qMin(rem, seg1);
            if (g > 0) { p.fillRect(QRect(xx, vuPad + 1, g, vuH - 2), Theme::C::vuGreen); rem -= g; xx += g; }
            int y = qMin(rem, seg2 - seg1);
            if (y > 0) { p.fillRect(QRect(xx, vuPad + 1, y, vuH - 2), Theme::C::vuAmber); rem -= y; xx += y; }
            if (rem > 0) p.fillRect(QRect(xx, vuPad + 1, rem, vuH - 2), Theme::C::vuRed);
        }
    }

    // ---- SID waveform / flag indicator block ----------------------------
    // Skipped when the user toggles them off via View ▸ SID indicators.
    if (sidIndOn_)
    // Two columns of lit boxes, one block per channel, spanning the full
    // height of (vu strip + scope strip):
    //   Col 1 (3 cells, top->bot): T  triangle
    //                              S  sawtooth
    //                              P  pulse
    //   Col 2 (4 cells, top->bot): N  noise
    //                              y  sync
    //                              r  ring mod
    //                              F  filter route
    // Box LIGHTS UP (filled with the family colour, white glyph) when
    // the bit is set in the live SID control register; off boxes paint a
    // dim background + dim glyph. Reads sidreg for voices 0..2 and
    // sidreg2 for voices 3..5 (stereo build).
    {
        const int indColW = 16;
        const int indW = indColW * 2;
        const int indH = vuStripH_ + scopeStripH_;
        QFont gf = p.font();
        gf.setBold(true);
        p.setFont(gf);
        for (int c = 0; c < shownChannels(); c++) {
            int x = rowNumW_ + c * chnW_ + 2;
            int w = chnW_ - 6;
            int x0 = x + w - indW;

            const unsigned char *regs = (c < 3) ? sidreg : sidreg2;
            int v = c % 3;
            unsigned char ctrl = regs[v * 7 + 4];
            unsigned char filt = regs[0x17];
            bool tri  = (ctrl >> 4) & 1;
            bool saw  = (ctrl >> 5) & 1;
            bool pul  = (ctrl >> 6) & 1;
            bool noi  = (ctrl >> 7) & 1;
            bool sync = (ctrl >> 1) & 1;
            bool ring = (ctrl >> 2) & 1;
            bool filtOn = (filt >> v) & 1;

            struct Cell { const char *g; bool on; QColor lit; };
            Cell col1[3] = {
                { "T", tri, QColor(0x6E, 0xC8, 0xFF) },
                { "S", saw, QColor(0xFF, 0xC4, 0x6E) },
                { "P", pul, QColor(0x9C, 0xFF, 0x9C) },
            };
            Cell col2[4] = {
                { "N", noi,    QColor(0xC9, 0xA0, 0xFF) },
                { "y", sync,   QColor(0xFF, 0x9E, 0x9E) },
                { "r", ring,   QColor(0xFF, 0x9E, 0x9E) },
                { "F", filtOn, QColor(0xFF, 0xD4, 0x40) },
            };
            auto drawCol = [&](int colX, Cell *cells, int n) {
                int cellH = indH / n;
                for (int i = 0; i < n; i++) {
                    QRect cell(colX + 1, i * cellH + 1, indColW - 2, cellH - 2);
                    QColor bg = cells[i].on ? cells[i].lit : Theme::C::bgAlt;
                    QColor border = cells[i].on ? cells[i].lit.darker(140)
                                                : Theme::C::sep;
                    QColor fg = cells[i].on ? QColor(0x10, 0x14, 0x1A)
                                            : Theme::C::textDim;
                    p.fillRect(cell, bg);
                    p.setPen(border);
                    p.drawRect(cell);
                    p.setPen(fg);
                    p.drawText(cell, Qt::AlignCenter, cells[i].g);
                }
            };
            drawCol(x0,            col1, 3);
            drawCol(x0 + indColW,  col2, 4);
        }
        p.setFont(font());
    }

    // ---- Mini scope strip -----------------------------------------------
    int scopeY = vuStripH_;
    for (int c = 0; c < shownChannels(); c++) {
        int x = rowNumW_ + c * chnW_ + 2;
        // Reserve the right edge for the SID waveform indicator block
        // (drawn above) so the scope curve doesn't stretch under it.
        // When the user hides those indicators, the scope reclaims the
        // full width.
        int w = chnW_ - 6 - (sidIndOn_ ? (16 * 2) : 0);
        QRect frame(x, scopeY + 2, w, scopeStripH_ - 4);
        p.fillRect(frame, Theme::C::vuBg);
        p.setPen(Theme::C::sep);
        p.drawRect(frame);
        QPainterPath path;
        int n = kScopeLen;
        bool first = true;
        for (int i = 0; i < n; i++) {
            int idx = (scopeHead_ + i) % n;
            float v = (float)scope_[c][idx] / 255.0f;
            float fx = frame.x() + 1 + (frame.width() - 2) * (float)i / (n - 1);
            float fy = frame.bottom() - 1 - v * (frame.height() - 2);
            if (first) { path.moveTo(fx, fy); first = false; }
            else        path.lineTo(fx, fy);
        }
        QColor stroke = chn[c].mute ? Theme::C::textDim : Theme::C::vuGreen;
        p.setPen(QPen(stroke, 1.5));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPath(path);

        // Filter cutoff trace — same yellow as the [F] indicator box, only
        // painted across the window where the voice was actually routed
        // through the filter ($17 voice bit set). Zero-valued samples
        // (route off) split the trace so the line doesn't span the
        // un-filtered gap.
        QPainterPath fpath;
        bool fopen = false;
        for (int i = 0; i < n; i++) {
            int idx = (scopeHead_ + i) % n;
            unsigned char fv = filtScope_[c][idx];
            if (fv == 0) { fopen = false; continue; }
            float v = (float)fv / 255.0f;
            float fx = frame.x() + 1 + (frame.width() - 2) * (float)i / (n - 1);
            float fy = frame.bottom() - 1 - v * (frame.height() - 2);
            if (!fopen) { fpath.moveTo(fx, fy); fopen = true; }
            else         fpath.lineTo(fx, fy);
        }
        if (!fpath.isEmpty()) {
            p.setPen(QPen(QColor(0xFF, 0xD4, 0x40), 1.5));
            p.drawPath(fpath);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ---- Channel header strip -------------------------------------------
    int headerY = vuStripH_ + scopeStripH_;
    const int muteW = 24;
    const int muteGap = 8;
    for (int c = 0; c < shownChannels(); c++) {
        int x = rowNumW_ + c * chnW_ + 4;
        bool active = (c == epchn);
        QRect headRect(x - 4, headerY + 2, chnW_ - 4, headerStripH_ - 4);
        if (active) p.fillRect(headRect, Theme::C::bgAlt);

        // Reserve right side for the mute toggle so the labels never collide.
        int muteX = rowNumW_ + (c + 1) * chnW_ - muteW - muteGap;

        p.setPen(active ? Theme::C::highlight : Theme::C::textDim);
        QFont hf = font();
        hf.setBold(active);
        p.setFont(hf);
        p.drawText(QPoint(x, headerY + headerStripH_ - 6),
                   QString("Ch%1").arg(c + 1));
        p.setFont(font());

        p.setPen(Theme::C::instr);
        p.drawText(QPoint(x + 4 * colWidth, headerY + headerStripH_ - 6),
                   QString("P%1").arg(epnum[c], 2, 16, QLatin1Char('0')).toUpper());

        p.setPen(Theme::C::textDim);
        p.drawText(QPoint(x + 8 * colWidth, headerY + headerStripH_ - 6),
                   QString("L%1").arg(pattlen[epnum[c]], 2, 16, QLatin1Char('0')).toUpper());

        // Mute toggle: 1-char glyph fits the narrow muteW box without
        // clipping. Red filled M = muted; green outlined ♪ = playing.
        QRect muteRect(muteX, headerY + 4, muteW, headerStripH_ - 8);
        QColor muteColor = chn[c].mute ? Theme::C::vuRed : Theme::C::vuGreen;
        p.fillRect(muteRect, chn[c].mute ? QColor(60, 20, 20)
                                         : QColor(20, 50, 25));
        p.setPen(muteColor);
        p.drawRect(muteRect);
        QFont mf = font();
        mf.setBold(true);
        p.setFont(mf);
        p.drawText(muteRect, Qt::AlignCenter, chn[c].mute ? "M" : "♪");
        p.setFont(font());
    }
    p.setPen(Theme::C::sep);
    p.drawLine(0, headerY + headerStripH_, W, headerY + headerStripH_);

    // ---- Pattern grid ---------------------------------------------------
    const int topOffset = gridTopOffset();
    const int rows = (viewport()->height() - topOffset) / rowHeight;
    const bool playing = isplaying() != 0;

    // Follow-play centre lock: when followplay && playing, the active
    // channel's current row is pinned to the vertical centre of the
    // viewport. EVERY channel's column scrolls independently so its own
    // chn[c].pattptr/4 lands at that same centre line — channels with
    // shorter patterns or independent loops still show their own playhead
    // and the rows around it. Row numbers display the active channel's
    // (wrapped) row.
    const bool followCenter = followplay && playing;
    const int  centerR = (rows > 0) ? rows / 2 : 0;
    const int  refPlay = playRow_[epchn];

    for (int r = 0; r < rows; r++) {
        int globalRow = r + rowOffset;
        QRect lineRect(0, topOffset + r * rowHeight, W, rowHeight);

        // Reference row used for beat / downbeat tinting + row number text.
        // Wraps inside the active channel's pattern length while
        // follow-centred so the band patterns stay consistent across the
        // viewport even when the song crosses a pattern boundary.
        int refRow;
        if (followCenter) {
            int plen0 = pattlen[epnum[epchn]];
            int vrow = refPlay + (r - centerR);
            refRow = (plen0 > 0) ? (((vrow % plen0) + plen0) % plen0) : vrow;
        } else {
            refRow = globalRow;
        }

        const int barRows = beatRows_ * barBeats_;
        if (refRow % barRows == 0)
            p.fillRect(QRect(0, lineRect.y(), rowNumW_ + chnW_ * MAX_CHN, rowHeight),
                       Theme::C::downbeat);
        else if (refRow % beatRows_ == 0)
            p.fillRect(QRect(0, lineRect.y(), rowNumW_ + chnW_ * MAX_CHN, rowHeight),
                       Theme::C::beat);

        // Edit cursor row — centre line during follow-centre, eppos otherwise.
        bool isEditRow = followCenter ? (r == centerR) : (globalRow == eppos);
        if (isEditRow)
            p.fillRect(lineRect, Theme::C::editRow);

        p.setPen(Theme::C::textDim);
        p.drawText(QRect(0, lineRect.y(), rowNumW_, rowHeight),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString("%1").arg(refRow, 3, 16, QLatin1Char('0')).toUpper());

        for (int c = 0; c < shownChannels(); c++) {
            int patnum = epnum[c];
            int plen = pattlen[patnum];
            int x = rowNumW_ + c * chnW_;
            QRect cellRect(x, lineRect.y(), chnW_, rowHeight);

            // Per-channel virtual row. Follow-centre offsets each channel
            // by (its playRow - epchn's playRow) so the channel's own
            // playhead lands at the viewport centre — independent of
            // pattern lengths or loop alignment between channels.
            //
            // Rows OUTSIDE the channel's current pattern (vrow < 0 or
            // vrow >= plen) render as empty space. The previous wrap
            // showed the same pattern repeating above and below the
            // playhead which made it look like the song was repeating
            // forever — per the user feedback, when the playhead
            // crosses a pattern boundary the new pattern's content
            // should be the only thing rolling under the line, with
            // blank space above it.
            int crow = -1;          // -1 -> sentinel: nothing to paint
            if (followCenter && plen > 0) {
                int vrow = playRow_[c] + (r - centerR);
                if (vrow >= 0 && vrow < plen) crow = vrow;
            } else {
                crow = globalRow;
            }

            // Rectangular selection overlay (edit-mode only — selections
            // don't make visual sense when each channel scrolls
            // independently). Lit semi-transparent fill across the
            // [chLo..chHi] x [rowLo..rowHi] block; a brighter stroke
            // outlines the rectangle so the edges of multi-cell
            // selections stay readable on the row-tinted bands.
            if (!followCenter && hasSelection()) {
                SelRect s = normalisedSelection();
                if (c >= s.chLo && c <= s.chHi
                    && globalRow >= s.rowLo && globalRow <= s.rowHi) {
                    QColor sel(Theme::C::highlight);
                    sel.setAlpha(70);
                    p.fillRect(cellRect, sel);
                    if (globalRow == s.rowLo) {
                        p.setPen(Theme::C::highlight);
                        p.drawLine(cellRect.topLeft(), cellRect.topRight());
                    }
                    if (globalRow == s.rowHi) {
                        p.setPen(Theme::C::highlight);
                        p.drawLine(cellRect.bottomLeft(), cellRect.bottomRight());
                    }
                    if (c == s.chLo) {
                        p.setPen(Theme::C::highlight);
                        p.drawLine(cellRect.topLeft(), cellRect.bottomLeft());
                    }
                    if (c == s.chHi) {
                        p.setPen(Theme::C::highlight);
                        p.drawLine(cellRect.topRight(), cellRect.bottomRight());
                    }
                }
            }

            // Playback row highlight per channel. Follow-centre puts every
            // channel's playhead on the centre line; edit-mode paints per
            // channel at the global row matching that channel's
            // chn[c].pattptr / 4.
            bool isPlayRow = playing && (followCenter ? (r == centerR)
                                                      : (playRow_[c] == globalRow));
            if (isPlayRow) p.fillRect(cellRect, Theme::C::playRow);

            // Edit-column tint on active channel/row. The white focus
            // border around the active nybble is drawn LATER, after the
            // instrument-coloured background — otherwise the per-
            // instrument fill paints over the border and the user can't
            // see which nybble has focus on a coloured cell.
            QRect focusRect;
            bool hasFocusRect = false;
            bool cursorOnThisRow = followCenter ? (r == centerR)
                                                 : (globalRow == eppos);
            if (c == epchn && cursorOnThisRow) {
                int tx = x + colWidth;
                QRect colRect;
                switch (epcolumn) {
                case 0: colRect = focusRect = QRect(tx, lineRect.y(), 3 * colWidth, rowHeight); break;
                case 1: colRect = QRect(tx + 4 * colWidth, lineRect.y(), 2 * colWidth, rowHeight);
                        focusRect = QRect(tx + 4 * colWidth, lineRect.y(), colWidth, rowHeight); break;
                case 2: colRect = QRect(tx + 4 * colWidth, lineRect.y(), 2 * colWidth, rowHeight);
                        focusRect = QRect(tx + 5 * colWidth, lineRect.y(), colWidth, rowHeight); break;
                case 3: colRect = focusRect = QRect(tx + 7 * colWidth, lineRect.y(), colWidth, rowHeight); break;
                case 4: colRect = QRect(tx + 8 * colWidth, lineRect.y(), 2 * colWidth, rowHeight);
                        focusRect = QRect(tx + 8 * colWidth, lineRect.y(), colWidth, rowHeight); break;
                case 5: colRect = QRect(tx + 8 * colWidth, lineRect.y(), 2 * colWidth, rowHeight);
                        focusRect = QRect(tx + 9 * colWidth, lineRect.y(), colWidth, rowHeight); break;
                }
                p.fillRect(colRect, QColor(Theme::C::highlight.red(),
                                           Theme::C::highlight.green(),
                                           Theme::C::highlight.blue(), 35));
                hasFocusRect = true;
            }

            // Follow-centre 'out of pattern' — blank cell. Beat tint /
            // play row / cursor underlay still showed up (those run off
            // the viewport row r, not crow), but the pattern content
            // text doesn't render here.
            if (followCenter && crow < 0) {
                continue;
            }

            // End-of-pattern band — only relevant when displaying the raw
            // pattern (edit mode). Follow-centre clamps crow to inside
            // [0, plen) above and bails before this branch.
            if (!followCenter && crow >= plen) {
                if (crow == plen) {
                    p.fillRect(cellRect, QColor(80, 25, 25));
                    p.setPen(QPen(QColor(255, 80, 80), 1));
                    p.drawLine(cellRect.left(),  cellRect.top(),
                               cellRect.right(), cellRect.top());
                    QFont ef = font();
                    ef.setBold(true);
                    p.setFont(ef);
                    p.setPen(QColor(255, 200, 200));
                    p.drawText(cellRect, Qt::AlignCenter,
                               QString::fromUtf8("══ END ══"));
                    p.setFont(font());
                } else {
                    p.setPen(Theme::C::textDim);
                    p.drawText(cellRect.adjusted(colWidth, 0, 0, 0),
                               Qt::AlignLeft | Qt::AlignVCenter, "---");
                }
                continue;
            }

            const unsigned char *cell = &pattern[patnum][crow * 4];
            unsigned char note = cell[0];
            unsigned char ins = cell[1];
            unsigned char cmd = cell[2];
            unsigned char param = cell[3];

            QString noteStr;
            if (note == REST)       noteStr = "...";
            else if (note == KEYOFF) noteStr = "===";
            else if (note == KEYON)  noteStr = "+++";
            else if (note >= FIRSTNOTE && note <= LASTNOTE)
                noteStr = notename[note - FIRSTNOTE];
            else noteStr = "...";

            QString insStr = (ins == 0) ? ".." :
                QString("%1").arg(ins, 2, 16, QLatin1Char('0')).toUpper();
            QString cmdStr = (cmd == 0 && param == 0) ? "..." :
                QString("%1%2").arg(cmd, 1, 16, QLatin1Char('0'))
                              .arg(param, 2, 16, QLatin1Char('0')).toUpper();

            int tx = x + colWidth;
            int ty = lineRect.y() + rowHeight - 5;
            // Instrument-coloured background — only paints over the 2-digit
            // instr cell so the rest of the row stays readable.
            if (instrColorsOn_ && ins) {
                QRect insBg(tx + 4 * colWidth, lineRect.y(),
                            2 * colWidth, rowHeight);
                p.fillRect(insBg, instrColor(ins));
            }
            // Boomwhacker / handbell pitch colour on the note cell — only
            // for real pitched notes. REST / KEYOFF / KEYON keep the
            // default style so the user can still distinguish them at a
            // glance.
            QColor noteFg = (note == REST || note == KEYOFF || note == KEYON
                             || note < FIRSTNOTE)
                            ? Theme::C::textDim : Theme::C::note;
            if (noteColorsOn_ && note >= FIRSTNOTE && note <= LASTNOTE) {
                int semi = note - FIRSTNOTE;  // 0 = C-0
                BoomColor bw = boomwhackerColor(semi);
                QRect noteBg(tx, lineRect.y(), 3 * colWidth, rowHeight);
                p.fillRect(noteBg, bw.bg);
                noteFg = bw.fg;
            }
            p.setPen(noteFg);
            p.drawText(QPoint(tx, ty), noteStr);
            // White text on the coloured instr cell for contrast when the
            // toggle is on; default instr colour otherwise.
            p.setPen(instrColorsOn_ && ins ? QColor(255, 255, 255)
                                            : (ins ? Theme::C::instr
                                                   : Theme::C::textDim));
            p.drawText(QPoint(tx + 4 * colWidth, ty), insStr);
            p.setPen(cmd ? Theme::C::cmdDigit : Theme::C::textDim);
            p.drawText(QPoint(tx + 7 * colWidth, ty), QString(cmdStr[0]));
            p.setPen(cmd || param ? Theme::C::cmdParam : Theme::C::textDim);
            p.drawText(QPoint(tx + 8 * colWidth, ty), cmdStr.mid(1));

            // White focus border drawn last so it sits on top of the
            // instrument-coloured background fill. Without this re-order
            // the per-instrument colour fill painted earlier covered the
            // border whenever the cursor landed on a coloured instr cell.
            if (hasFocusRect) {
                p.setPen(QPen(QColor(255, 255, 255), 1));
                p.drawRect(focusRect.adjusted(0, 0, -1, -1));
            }
        }
    }
}
