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

signals:
    void patternEdited();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
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
};
