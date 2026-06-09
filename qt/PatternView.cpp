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
    vuStripH_ = 14;
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
    if (k == Qt::Key_Backspace) {
        releasenote(epchn);
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
        }
        refresh();
        emit patternEdited();
    }
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
        // Waveform / flag indicator block sits at the right edge of the
        // VU bar. Reads the live SID control register for this voice
        // (sidreg / sidreg2 for SID1 / SID2) — bits 4..7 are TRI / SAW
        // / PUL / NOI, bits 1..2 are SYNC / RING. The $17 filter
        // register's voice-enable bits show as F. Lit glyphs use a hue
        // per waveform family so the user can tell at a glance which
        // generators a patch is actually driving each frame.
        //
        // Block reserves ~6 glyphs × ~7 px = ~42 px on the right; the
        // VU fill below tops out at that boundary so the meter never
        // paints over the glyphs.
        const int indGlyphs = 7;     // T S P N | S R F
        const int indGlyphW = 7;
        const int indW = indGlyphs * indGlyphW;
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
        // SID register window. Voices 0..2 -> sidreg, 3..5 -> sidreg2.
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
        struct Ind { const char *g; bool on; QColor lit; };
        Ind inds[] = {
            { "T", tri,    QColor(0x6E, 0xC8, 0xFF) },
            { "S", saw,    QColor(0xFF, 0xC4, 0x6E) },
            { "P", pul,    QColor(0x9C, 0xFF, 0x9C) },
            { "N", noi,    QColor(0xC9, 0xA0, 0xFF) },
            { "y", sync,   QColor(0xFF, 0x9E, 0x9E) },
            { "r", ring,   QColor(0xFF, 0x9E, 0x9E) },
            { "F", filtOn, QColor(0xFF, 0xD4, 0x40) },
        };
        int indX0 = x + w - indW;
        QFont gf = p.font();
        gf.setBold(true);
        p.setFont(gf);
        for (int i = 0; i < indGlyphs; i++) {
            QRect gr(indX0 + i * indGlyphW, vuPad, indGlyphW, vuH);
            p.setPen(inds[i].on ? inds[i].lit : Theme::C::textDim);
            p.drawText(gr, Qt::AlignCenter, inds[i].g);
        }
        p.setFont(font());
    }

    // ---- Mini scope strip -----------------------------------------------
    int scopeY = vuStripH_;
    for (int c = 0; c < shownChannels(); c++) {
        int x = rowNumW_ + c * chnW_ + 2;
        int w = chnW_ - 6;
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

        if (refRow % 16 == 0)
            p.fillRect(QRect(0, lineRect.y(), rowNumW_ + chnW_ * MAX_CHN, rowHeight),
                       Theme::C::downbeat);
        else if (refRow % 4 == 0)
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

            // Selection block overlay (edit-mode only — selections don't
            // make visual sense when each channel scrolls independently).
            if (!followCenter && epmarkchn == c) {
                int lo = qMin(epmarkstart, epmarkend);
                int hi = qMax(epmarkstart, epmarkend);
                if (globalRow >= lo && globalRow <= hi) {
                    QColor sel(Theme::C::highlight);
                    sel.setAlpha(40);
                    p.fillRect(cellRect, sel);
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
