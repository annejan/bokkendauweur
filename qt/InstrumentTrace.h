#pragma once
#include <QVector>
#include <QWidget>

// Per-frame trace of the SID register values an instrument drives through its
// wave / pulse / filter tables, produced by a pure table-walk simulation (no
// engine globals mutated). Used by RegisterGraphView in the instrument editor.
//
// Scope: faithful for the common table opcodes (set / sweep / delay / jump and
// the wavetable commands that touch pulse / filter / AD / SR). Frequency-only
// commands (porta / vibrato / toneporta) are ignored since they don't move any
// of the four traced values. Not cycle-exact for exotic cases — it's a
// visualisation, not an emulator.
struct InstrTrace {
    QVector<int> wave;    // control-register waveform byte (0..255)
    QVector<int> pulse;   // pulse width, 12-bit (0..4095)
    QVector<int> cutoff;  // filter cutoff register $16 (0..255)
    QVector<int> env;     // envelope amplitude (0..255)
    int frames = 0;
    int releaseFrame = -1; // frame index where the release stage begins
};

// Walk instrument `instrNum` (1..MAX_INSTR-1) for up to maxFrames. A sustained
// envelope is held for at most `sustainCap` frames before release kicks in, so
// the graph can't grow unbounded.
InstrTrace computeInstrumentTrace(int instrNum, int maxFrames = 96, int sustainCap = 20);

// Multi-trace graph: X = frames, Y = each register trace normalised to its own
// range. Fixed per-trace colours, a legend below the plot, a release marker,
// and a hover crosshair + per-frame value readout.
class RegisterGraphView : public QWidget {
public:
    explicit RegisterGraphView(QWidget *parent = nullptr);
    void setInstrument(int instrNum);   // recompute the trace + repaint

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QRect plotRect() const;            // graph area (above the legend strip)
    int   frameAtX(int x) const;       // nearest frame index for a viewport x
    int   xForFrame(int f) const;      // viewport x for a frame index

    InstrTrace tr_;
    int instr_      = -1;
    int hoverFrame_ = -1;
};
