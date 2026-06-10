#pragma once
#include <QAbstractScrollArea>
#include <array>
#include <deque>
extern "C" {
#include "gcommon.h"
}

class PatternView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PatternView(QWidget *parent = nullptr);

public slots:
    void refresh();
    void tickScope();
    // Master enable for instrument-colour fills in the pattern grid. With
    // per-instrument opt-in, this is effectively always on; the flag stays
    // around so the painter can still short-circuit a tight loop.
    void setInstrColorsEnabled(bool on) { instrColorsOn_ = on; viewport()->update(); }
    bool instrColorsEnabled() const     { return instrColorsOn_; }
    void setNoteColorsEnabled(bool on)  { noteColorsOn_ = on; viewport()->update(); }
    bool noteColorsEnabled() const      { return noteColorsOn_; }
    // Per-voice SID waveform / flag indicator block in the vu+scope strip.
    void setSidIndicatorsEnabled(bool on) { sidIndOn_ = on; viewport()->update(); }
    bool sidIndicatorsEnabled() const     { return sidIndOn_; }
    // Beat tinting: rowsPerBeat lights the lighter "beat" band every N
    // rows, and rowsPerBeat * beatsPerBar lights the darker "downbeat"
    // band every M rows. Default 4 / 4 -> beat every 4 rows, downbeat
    // every 16 rows (4/4 in 16-th-note tracker grid).
    void setBeatGrid(int rowsPerBeat, int beatsPerBar) {
        beatRows_ = qMax(1, rowsPerBeat);
        barBeats_ = qMax(1, beatsPerBar);
        viewport()->update();
    }
    int rowsPerBeat() const { return beatRows_; }
    int beatsPerBar() const { return barBeats_; }

    // Insert / remove row behaviour. When true, inserting a row at the
    // cursor grows pattlen[] by 1 (no data lost). When false (default),
    // the pattern length stays fixed and the last row falls off the end
    // — Protracker / FastTracker convention.
    void setInsertGrowsPattern(bool on) { insertGrows_ = on; }
    bool insertGrowsPattern() const     { return insertGrows_; }

    // Selection / clipboard API. Selection is a rectangle in
    // (channel, row) space spanning [selChAnchor_..selChCursor_] x
    // [selRowAnchor_..selRowCursor_], normalised on access.
    bool hasSelection() const { return selRowAnchor_ >= 0; }
    void clearSelection() { selRowAnchor_ = selRowCursor_ = -1; viewport()->update(); }
    void copySelectionToClipboard(bool cut);
    void pasteFromClipboard(bool repeatFill);
    void clearSelectedCells();
    void removeSelectedRows();
    void insertRowAtCursor();
    void deleteRowAtCursor();
    void selectAllInChannel();
    void selectAllRows();
    // Transpose every note cell inside the current selection by `semis`
    // semitones, clamped to the GoatTracker note range. Skips REST /
    // KEYOFF / KEYON / instrument-only cells. +12 / -12 are the octave
    // shortcuts bound to Ctrl+Shift+Up / Ctrl+Shift+Down.
    void transposeSelection(int semis);

    // When true (default), the pattern editor remaps QWERTY note-position
    // scan codes onto the SDL keysym table the C-core's notekeytbl[] uses.
    // Lets users on Dvorak / AZERTY / Colemak / etc. play notes from the
    // physical key positions instead of whatever symbol their layout has
    // mapped there. Logical Qt::Key_* is still used for everything else
    // (hex digits, navigation), so they type hex normally.
    void setPhysicalKeyLayout(bool on) { physicalKeyLayout_ = on; }
    bool physicalKeyLayout() const     { return physicalKeyLayout_; }

signals:
    void patternEdited();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    bool event(QEvent *e) override;

private:
    int rowHeight = 16;
    int colWidth = 14;
    int visibleRows() const;
    int gridTopOffset() const;
    void updateScrollRange();

    // Per-channel envelope ring buffer (~60 frames). MAX_CHN comes from the
    // C side via gcommon.h — 3 for mono, 6 for the stereo build.
    static constexpr int kScopeLen = 64;
    std::array<std::array<unsigned char, kScopeLen>, MAX_CHN> scope_{};
    // Filter cutoff history per voice. Mirrors scope_ ring buffer; only
    // captures when the $17 filter-route bit for the voice is set,
    // otherwise stores 0 so the trace flattens (and we paint nothing).
    std::array<std::array<unsigned char, kScopeLen>, MAX_CHN> filtScope_{};
    int scopeHead_ = 0;

    // Layout constants computed at construction
    int rowNumW_ = 0;
    int chnW_ = 0;
    int vuStripH_ = 0;
    int scopeStripH_ = 0;
    int headerStripH_ = 0;

    int channelAtX(int x) const;
    bool instrColorsOn_ = false;
    bool noteColorsOn_ = false;
    bool sidIndOn_ = true;
    int  beatRows_ = 4;
    int  barBeats_ = 4;
    int lastEppos_ = -1;
    int lastEpchn_ = -1;
    // chn[c].pattptr snapshot taken at the start of refresh() and reused
    // by paintEvent() so the editor cursor row (eppos = chn[epchn]/4 in
    // follow-play) and the per-channel red play row highlight read the
    // SAME chn[].pattptr value. Without this the audio thread could
    // advance chn[].pattptr between the refresh() snapshot and the
    // paintEvent read, putting the red play row one step ahead of the
    // edit cursor row in follow-play.
    int playRow_[MAX_CHN] = {0};

    // Rectangular selection state. Both anchor + cursor in (channel, row)
    // coordinates. -1 means no active selection. The 'cursor' end is the
    // one the mouse drag / shift-click updates; 'anchor' stays put.
    int selRowAnchor_ = -1;
    int selRowCursor_ = -1;
    int selChAnchor_  = -1;
    int selChCursor_  = -1;
    bool dragActive_ = false;
    bool insertGrows_ = false;
    bool physicalKeyLayout_ = true;
    struct SelRect { int chLo, chHi, rowLo, rowHi; };
    SelRect normalisedSelection() const;
    int rowAtY(int y) const;
    QRect cellRect(int chan, int row) const;
};
