#include "InstrumentView.h"
#include "Theme.h"
#include "UndoStack.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QSpinBox>
#include <QSlider>
#include <QTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QGroupBox>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QFontMetrics>
#include <cstring>

extern "C" {
#include "gcommon.h"
#include "gplay.h"
extern INSTR instr[MAX_INSTR];
extern int einum;
extern int epoctave;
extern int epchn;
extern int highestusedinstr;
void gototable(int t, int pos);
void playtestnote(int note, int ins, int chnnum);
void releasenote(int chnnum);
void sid_getlevels(unsigned char *out);
extern int editmode;
extern unsigned char ltable[MAX_TABLES][MAX_TABLELEN];
extern unsigned char rtable[MAX_TABLES][MAX_TABLELEN];
}

// Defined further down beside markDirty; forward-declared so refresh()
// (above markDirty in this TU) can call it on the not-dirty path.
static void clearDirtyChrome(class QPushButton *b);

// ---------------------------------------------------------------------------
// ADSR envelope preview widget
// ---------------------------------------------------------------------------
class AdsrPreview : public QWidget {
    Q_OBJECT
public:
    explicit AdsrPreview(QWidget *parent) : QWidget(parent) {
        setMinimumSize(280, 140);
        Theme::applyDarkPalette(this);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::C::bgAlt);
        setPalette(pal);
        setMouseTracking(true);
        setCursor(Qt::ArrowCursor);
    }
    void setAdsr(int a, int d, int s, int r) {
        a_ = a; d_ = d; s_ = s; r_ = r;
        update();
    }
    // Live amplitude during playback, 0..255. Anything > 0 enables the
    // moving-bar marker; 0 disables it (and the parent will also skip
    // calling update() to save frames).
    void setLiveLevel(int level) {
        if (level == liveLevel_) return;
        liveLevel_ = level;
        update();
    }

signals:
    void adsrChanged(int a, int d, int s, int r);

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        computeGeometry();
        int W = width(), H = height();
        (void)W; (void)H;

        // Fill under envelope
        QPolygon poly;
        poly << QPoint(p0_.x(), baseY_) << p0_ << p1_ << p2_ << p3_ << p4_
             << QPoint(p4_.x(), baseY_);
        QColor fill = Theme::C::highlight;
        fill.setAlpha(60);
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawPolygon(poly);

        // Envelope stroke
        p.setPen(QPen(Theme::C::highlight, 2));
        p.drawLine(p0_, p1_);
        p.drawLine(p1_, p2_);
        p.drawLine(p2_, p3_);
        p.drawLine(p3_, p4_);

        // Draggable point handles — only the three that are wired to
        // spinboxes get a marker so the user knows what to grab.
        auto handle = [&](const QPoint &pt, bool active) {
            p.setBrush(active ? Theme::C::highlight : Theme::C::bgAlt);
            p.setPen(QPen(Theme::C::highlight, 1.5));
            p.drawEllipse(pt, 4, 4);
        };
        handle(p1_, dragPoint_ == 1);
        handle(p2_, dragPoint_ == 2);
        handle(p4_, dragPoint_ == 4);

        // Moving-bar marker showing live envelope amplitude during
        // playback. Horizontal position scales with current level
        // relative to the envelope peak (255). The marker is suppressed
        // when liveLevel_ == 0 (gate off / silent) so the idle UI looks
        // exactly like it always did.
        if (liveLevel_ > 0) {
            int span = p4_.x() - p0_.x();
            int barX = p0_.x() + (liveLevel_ * span) / 255;
            if (barX < p0_.x()) barX = p0_.x();
            if (barX > p4_.x()) barX = p4_.x();
            QColor bar = Theme::C::playRow;
            bar.setAlpha(220);
            p.setPen(QPen(bar, 2));
            p.drawLine(QPoint(barX, peakY_ - 4),
                       QPoint(barX, baseY_ + 4));
        }

        // Labels for phases
        p.setPen(Theme::C::textDim);
        QFont f = font();
        f.setPointSize(8);
        p.setFont(f);
        p.drawText(QPoint(p0_.x(), baseY_ + 10), "0");
        p.drawText(QRect(p0_.x(), peakY_ - 12, p1_.x() - p0_.x(), 10),
                   Qt::AlignCenter, "A");
        p.drawText(QRect(p1_.x(), peakY_ - 12, p2_.x() - p1_.x(), 10),
                   Qt::AlignCenter, "D");
        p.drawText(QRect(p2_.x(), sustY_ - 12, p3_.x() - p2_.x(), 10),
                   Qt::AlignCenter, "S");
        p.drawText(QRect(p3_.x(), sustY_ - 12, p4_.x() - p3_.x(), 10),
                   Qt::AlignCenter, "R");
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        computeGeometry();
        QPoint pos = e->pos();
        const int hit = 10; // pixel-grab radius
        // Tested in painted-z order — p4 / p2 / p1, so dragging the
        // release handle still works even if it visually overlaps the
        // sustain handle on tiny envelopes.
        if (manhattan(pos, p4_) <= hit) dragPoint_ = 4;
        else if (manhattan(pos, p2_) <= hit) dragPoint_ = 2;
        else if (manhattan(pos, p1_) <= hit) dragPoint_ = 1;
        else dragPoint_ = 0;
        if (dragPoint_) {
            applyDrag(pos);
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (dragPoint_) {
            applyDrag(e->pos());
            update();
            return;
        }
        // Hover cursor cue — turn into an open hand near any drag
        // handle. Keeps the affordance discoverable without a label.
        computeGeometry();
        const int hit = 10;
        QPoint pos = e->pos();
        bool over = manhattan(pos, p1_) <= hit
                 || manhattan(pos, p2_) <= hit
                 || manhattan(pos, p4_) <= hit;
        setCursor(over ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        dragPoint_ = 0;
        update();
    }

private:
    static int manhattan(const QPoint &a, const QPoint &b) {
        return qAbs(a.x() - b.x()) + qAbs(a.y() - b.y());
    }

    // Compute p0..p4 / sustY using the current a/d/s/r. Mirrors the
    // arithmetic of the original paint code so visual position and
    // hit-test position match exactly.
    void computeGeometry() {
        int W = width(), H = height();
        int aw = 20 + a_ * 12;
        int dw = 20 + d_ * 10;
        int rw = 20 + r_ * 10;
        baseY_ = H - 12;
        peakY_ = 14;
        sustY_ = baseY_ - (s_ * (baseY_ - peakY_) / 15);
        p0_ = QPoint(10, baseY_);
        p1_ = QPoint(p0_.x() + aw, peakY_);
        p2_ = QPoint(p1_.x() + dw, sustY_);
        p3_ = QPoint(p2_.x() + 80, sustY_);
        p4_ = QPoint(p3_.x() + rw, baseY_);
        if (p4_.x() > W - 8) p4_.setX(W - 8);
    }

    // Translate a mouse position into nybble values for the active
    // drag point and emit adsrChanged so the spinboxes update (which
    // in turn write to instr[einum] via the existing slots).
    void applyDrag(const QPoint &pos) {
        // Recompute with current values so the geometry the inverse
        // math uses matches what was drawn.
        computeGeometry();
        int newA = a_, newD = d_, newS = s_, newR = r_;
        switch (dragPoint_) {
        case 1: {
            // p1.x = p0.x + (20 + a*12) → solve for a in 0..15
            int dx = pos.x() - p0_.x() - 20;
            newA = qBound(0, dx / 12, 15);
            break;
        }
        case 2: {
            // p2.x = p1.x + (20 + d*10) → d in 0..15
            int dx = pos.x() - p1_.x() - 20;
            newD = qBound(0, dx / 10, 15);
            // p2.y / sustY = baseY - s * (baseY-peakY)/15 → s in 0..15
            int range = baseY_ - peakY_;
            if (range > 0) {
                int dy = baseY_ - qBound(peakY_, pos.y(), baseY_);
                newS = qBound(0, (dy * 15 + range / 2) / range, 15);
            }
            break;
        }
        case 4: {
            // p4.x = p3.x + (20 + r*10) → r in 0..15
            int dx = pos.x() - p3_.x() - 20;
            newR = qBound(0, dx / 10, 15);
            break;
        }
        default:
            return;
        }
        if (newA == a_ && newD == d_ && newS == s_ && newR == r_) return;
        a_ = newA; d_ = newD; s_ = newS; r_ = newR;
        emit adsrChanged(a_, d_, s_, r_);
    }

    int a_ = 0, d_ = 0, s_ = 0, r_ = 0;
    int liveLevel_ = 0;
    int dragPoint_ = 0; // 0 = none, 1 = p1, 2 = p2, 4 = p4

    // Cached geometry from the last computeGeometry() call. paintEvent /
    // mouse handlers all use these so the picture and the hit areas
    // stay in lockstep.
    QPoint p0_, p1_, p2_, p3_, p4_;
    int baseY_ = 0, peakY_ = 0, sustY_ = 0;
};

// ---------------------------------------------------------------------------
// Wavetable preview strip
// ---------------------------------------------------------------------------
class WavetablePreview : public QWidget {
    Q_OBJECT
public:
    explicit WavetablePreview(QWidget *parent) : QWidget(parent) {
        setMinimumHeight(28);
        Theme::applyDarkPalette(this);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::C::bgAlt);
        setPalette(pal);
    }
    void setStart(int s) { start_ = s; update(); }
    // Set the currently-active wavetable step (1-based row index from
    // chn[].ptr[WTBL]). 0 means 'idle' — no highlight. The widget will
    // only repaint when the value actually changes, keeping the timer-
    // driven refresh from spinning the CPU.
    void setActiveStep(int step) {
        if (step == activeStep_) return;
        activeStep_ = step;
        update();
    }
    int activeStep() const { return activeStep_; }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const int N = 24; // show 24 wavetable steps
        int W = width(), H = height();
        int cellW = W / N;
        for (int i = 0; i < N; i++) {
            int idx = start_ + i - 1;
            if (idx < 0 || idx >= MAX_TABLELEN) continue;
            unsigned char L = ltable[WTBL][idx];
            QColor c = Theme::C::textDim;
            if (L == 0xff) c = Theme::C::rst;
            else if (L >= 0xf0) c = Theme::C::cmdDigit;
            else if (L >= 0xe0) c = Theme::C::textDim;
            else if (L >= 0x80) c = QColor(0xC0, 0x70, 0xC0);
            else if (L >= 0x40) c = QColor(0x6F, 0xA6, 0xCE);
            else if (L >= 0x20) c = QColor(0xE0, 0xA8, 0x4A);
            else if (L >= 0x10) c = QColor(0xA1, 0xC1, 0x81);
            else if (L >= 0x01) c = Theme::C::transpose;
            // Reserve bottom strip for the legend; cells live above it.
            const int legendH = 12;
            int cellsTop = 2;
            int cellsH = qMax(8, H - legendH - 2);
            QRect r(i * cellW + 1, cellsTop, cellW - 2, cellsH);
            p.fillRect(r, c);
            p.setPen(Theme::C::sep);
            p.drawRect(r);
            // Highlight the step the playback engine is currently
            // executing. activeStep_ is 1-based (matches chn[].ptr[WTBL]
            // semantics — 0 means idle). Bright outline overlays the
            // sep border so it reads as 'live cursor' even at small
            // cell sizes. Stays inside the existing cell rect so the
            // visual hierarchy isn't disturbed.
            if (activeStep_ != 0 && (idx + 1) == activeStep_) {
                p.setPen(QPen(Theme::C::highlight, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(1, 1, -1, -1));
            }
        }
        // Legend brightness matches the section header above so the user
        // reads it as 'caption for these tiles', not 'disabled metadata'.
        // Small font size keeps the visual hierarchy: header > legend in
        // size, same brightness.
        p.setPen(Theme::C::text);
        QFont f = font();
        f.setPointSize(8);
        p.setFont(f);
        p.drawText(QRect(4, H - 12, W - 8, 12),
                   Qt::AlignVCenter,
                   "noise=purple  pulse=blue  saw=orange  tri=green  cmd=red  jump=orange");
    }
private:
    int start_ = 1;
    int activeStep_ = 0;
};

// ---------------------------------------------------------------------------
// Helper: hex spinbox 0..255
// ---------------------------------------------------------------------------
static QSpinBox *makeHexSpin(QWidget *parent) {
    auto *s = new QSpinBox(parent);
    s->setRange(0, 255);
    s->setDisplayIntegerBase(16);
    s->setPrefix("$");
    QFont f = Theme::monoFont(11);
    s->setFont(f);
    s->setMinimumWidth(80);
    return s;
}

static QSpinBox *makeNybbleSpin(QWidget *parent) {
    auto *s = new QSpinBox(parent);
    s->setRange(0, 15);
    s->setDisplayIntegerBase(16);
    QFont f = Theme::monoFont(11);
    s->setFont(f);
    s->setMinimumWidth(60);
    return s;
}

// Wrap a 0..15 ADSR spinbox in a row with a horizontal slider. Slider and
// spinbox stay in sync via two-way valueChanged forwarding — dragging the
// slider drives the spinbox (which in turn fires the InstrumentView's
// onAdChanged / onSrChanged slot), so the engine wiring stays unchanged.
static QWidget *makeNybbleRow(QSpinBox *spin, QWidget *parent) {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto *slider = new QSlider(Qt::Horizontal, w);
    slider->setRange(0, 15);
    slider->setPageStep(1);
    slider->setTickPosition(QSlider::TicksBelow);
    slider->setTickInterval(1);
    slider->setMinimumWidth(160);
    spin->setMaximumWidth(70);
    h->addWidget(spin);
    h->addWidget(slider, 1);
    QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                     slider, &QSlider::setValue);
    QObject::connect(slider, &QSlider::valueChanged,
                     spin, &QSpinBox::setValue);
    return w;
}

// ---------------------------------------------------------------------------

InstrumentView::InstrumentView(QWidget *parent) : QWidget(parent) {
    Theme::applyDarkPalette(this);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // Left-side instrument list removed — the 'Instruments' side dock on
    // the right already provides the same selector and was duplicating
    // both the screen real-estate and the keep-in-sync churn.

    // Center: editor form --------------------------------------------------
    auto *center = new QVBoxLayout();
    center->setSpacing(8);

    auto *header = new QLabel("Instrument", this);
    QFont hf = header->font(); hf.setPointSize(13); hf.setBold(true);
    header->setFont(hf);
    QPalette hp = header->palette();
    hp.setColor(QPalette::WindowText, Theme::C::highlight);
    header->setPalette(hp);
    center->addWidget(header);

    auto *nameRow = new QHBoxLayout();
    auto *nameLbl = new QLabel("Name:", this);
    name_ = new QLineEdit(this);
    name_->setMaxLength(MAX_INSTRNAMELEN - 1);
    name_->setFont(Theme::monoFont(11));
    name_->setToolTip("Display name (max 16 chars). Saved with the .sng and "
                      "shown in the instrument list.");
    nameRow->addWidget(nameLbl);
    nameRow->addWidget(name_, 1);
    center->addLayout(nameRow);
    connect(name_, &QLineEdit::textEdited, this, &InstrumentView::onNameChanged);

    // Preset envelope picker — sets ADSR + 1stFrame Wave + name to a sensible
    // default for a common instrument archetype. Wavetable / pulse / filter
    // pointers are NOT touched, so existing wavetable programs keep working.
    auto *presetRow = new QHBoxLayout();
    auto *presetLbl = new QLabel("Preset:", this);
    presetBox_ = new QComboBox(this);
    presetBox_->setToolTip("Apply a starter envelope for a common instrument "
                           "archetype. Only ADSR / name / 1stFrame Wave change; "
                           "wavetable pointers stay where they are.");
    presetBox_->addItem("(choose a preset…)");
    presetBox_->addItem("Bass — tight pulse");
    presetBox_->addItem("Bass — triangle, soft");
    presetBox_->addItem("Lead — sawtooth");
    presetBox_->addItem("Pluck — fast decay");
    presetBox_->addItem("Pad — slow attack, sustain");
    presetBox_->addItem("Brass — punchy attack");
    presetBox_->addItem("Organ — full sustain, no release");
    presetBox_->addItem("Bell — instant attack, long decay");
    presetBox_->addItem("Strings — soft attack, full sustain");
    presetBox_->addItem("Drum kit: Kick");
    presetBox_->addItem("Drum kit: Snare");
    presetBox_->addItem("Drum kit: Closed hat");
    presetBox_->addItem("Drum kit: Open hat");
    auto *applyBtn = new QPushButton("Apply", this);
    applyBtn->setToolTip("Overwrite this instrument's envelope with the "
                         "selected preset. (Selecting from the dropdown "
                         "applies immediately; this button re-applies the "
                         "current selection.)");
    connect(applyBtn, &QPushButton::clicked, this,
            [this]{ applyPreset(presetBox_->currentIndex()); });
    // Selecting from the dropdown applies the preset immediately — the
    // separate Apply button is kept for users who want to re-apply after
    // hand-editing the ADSR fields.
    connect(presetBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
                if (updating_) return;
                applyPreset(idx);
            });
    presetRow->addWidget(presetLbl);
    presetRow->addWidget(presetBox_, 1);
    presetRow->addWidget(applyBtn);
    center->addLayout(presetRow);

    auto *envBox = new QGroupBox("Envelope (ADSR)", this);
    envBox->setToolTip("SID envelope shape. $0 is fastest, $F is slowest. "
                       "Attack 0 can make the first wavetable row silent.");
    auto *envForm = new QFormLayout(envBox);
    attack_ = makeNybbleSpin(envBox);
    attack_->setToolTip("Attack rate ($0 fastest = 2ms, $F slowest = 8s). "
                        "Set high on pads/strings, low on bass/lead.");
    decay_ = makeNybbleSpin(envBox);
    decay_->setToolTip("Decay rate from peak to sustain level. "
                       "$0 = 6ms, $F = 24s. Controls how fast the note 'rings down'.");
    sustain_ = makeNybbleSpin(envBox);
    sustain_->setToolTip("Sustain level ($0 silent, $F loudest). "
                         "Held while gate is on. Set to 0 for pluck/percussive.");
    release_ = makeNybbleSpin(envBox);
    release_->setToolTip("Release rate after key-off ($0 fastest, $F slowest). "
                         "Watch out: A=0, R=1 can ADSR-bug on some chips.");
    envForm->addRow("Attack",  makeNybbleRow(attack_,  envBox));
    envForm->addRow("Decay",   makeNybbleRow(decay_,   envBox));
    envForm->addRow("Sustain", makeNybbleRow(sustain_, envBox));
    envForm->addRow("Release", makeNybbleRow(release_, envBox));
    connect(attack_, QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onAdChanged);
    connect(decay_,  QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onAdChanged);
    connect(sustain_,QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onSrChanged);
    connect(release_,QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onSrChanged);
    center->addWidget(envBox);

    auto *tblBox = new QGroupBox("Table pointers", this);
    tblBox->setToolTip("Start-position pointers into the four shared tables. "
                       "$00 means 'no execution' (except Wave, where $00 is unusable).");
    auto *tblForm = new QFormLayout(tblBox);
    wave_ = makeHexSpin(tblBox);
    wave_->setToolTip("Wavetable start step. Drives waveform + arpeggio + "
                      "drum programs. Must be ≥ $01.");
    pulse_ = makeHexSpin(tblBox);
    pulse_->setToolTip("Pulsetable start step. $00 = leave pulse execution "
                       "alone. Used for PWM sweeps.");
    filter_ = makeHexSpin(tblBox);
    filter_->setToolTip("Filtertable start step. $00 = leave filter alone. "
                        "Only one channel should drive the filter at a time.");
    vibParam_ = makeHexSpin(tblBox);
    vibParam_->setToolTip("Speedtable index for instrument vibrato. $00 = no "
                          "vibrato. Vibrato delay sets how many ticks before it starts.");

    auto wrapJump = [this, tblBox](QSpinBox *s, void (InstrumentView::*slot)()) {
        auto *row = new QHBoxLayout();
        row->addWidget(s);
        auto *btn = new QPushButton("→ table", tblBox);
        btn->setMaximumWidth(80);
        btn->setToolTip("Open the Tables editor and jump the cursor to the "
                        "row this pointer references — quick way to find / "
                        "edit the wavetable / pulsetable / filtertable "
                        "program this instrument runs.");
        connect(btn, &QPushButton::clicked, this, slot);
        row->addWidget(btn);
        return row;
    };
    tblForm->addRow("Wavetable Pos",   wrapJump(wave_,   &InstrumentView::onGotoWaveTable));
    tblForm->addRow("Pulsetable Pos",  wrapJump(pulse_,  &InstrumentView::onGotoPulseTable));
    tblForm->addRow("Filtertable Pos", wrapJump(filter_, &InstrumentView::onGotoFilterTable));
    tblForm->addRow("Vibrato Param",   vibParam_);
    connect(wave_,    QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onWtChanged);
    connect(pulse_,   QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onPtChanged);
    connect(filter_,  QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onFtChanged);
    connect(vibParam_,QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onVibParamChanged);
    center->addWidget(tblBox);

    auto *miscBox = new QGroupBox("Misc", this);
    auto *miscForm = new QFormLayout(miscBox);
    vibDelay_ = makeHexSpin(miscBox);
    vibDelay_->setToolTip("Ticks before instrument vibrato kicks in. "
                          "$00 disables vibrato entirely.");
    gateTimer_ = makeHexSpin(miscBox);
    gateTimer_->setToolTip("Hard-restart / gate-off length in ticks. "
                           "Must be < tempo or playback halts. "
                           "Bit $80 disables hard restart, $40 disables gate-off.");
    firstWave_ = makeHexSpin(miscBox);
    firstWave_->setToolTip("Waveform written on the very first frame of a note. "
                           "$09 = gate+test (default). $00/$FE/$FF special-case "
                           "the gate behaviour for legato instruments.");
    miscForm->addRow("Vibrato Delay", vibDelay_);
    miscForm->addRow("HR/Gate Timer", gateTimer_);
    miscForm->addRow("1stFrame Wave", firstWave_);
    connect(vibDelay_, QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onVibDelayChanged);
    connect(gateTimer_,QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onGateChanged);
    connect(firstWave_,QOverload<int>::of(&QSpinBox::valueChanged), this, &InstrumentView::onFirstWaveChanged);
    center->addWidget(miscBox);

    auto *btnRow = new QHBoxLayout();
    auto *testBtn = new QPushButton("Play test note", this);
    testBtn->setToolTip("Play C of the current octave through this instrument "
                        "on the current channel.");
    auto *stopBtn = new QPushButton("Silence", this);
    stopBtn->setToolTip("Release the test note on the current channel.");
    auto *autoBtn = new QPushButton("Auto-test", this);
    autoBtn->setCheckable(true);
    autoBtn->setToolTip("Retrigger the test note every second so any edit to "
                        "ADSR / wave / pulse / filter is audible instantly.");
    applyBtn_ = new QPushButton("Apply", this);
    applyBtn_->setToolTip("Commit the current edits as the new revert baseline. "
                          "After Apply, the Reset button will roll back to "
                          "THIS state. The button highlights amber whenever "
                          "the instrument has unsaved changes.");
    // Default ('clean') stylesheet — Qt's normal button look. Toggled to a
    // bright amber pill by markDirty() / cleared on Apply / Reset / slot
    // switch so the user can see at a glance whether the edits are live.
    applyBtn_->setStyleSheet("");
    resetBtn_ = new QPushButton("Reset", this);
    resetBtn_->setToolTip("Roll the instrument back to the state at the last "
                          "Apply (or to the state it was in when this slot was "
                          "first opened, if you never clicked Apply).");
    connect(applyBtn_, &QPushButton::clicked, this, &InstrumentView::onApplyEdits);
    connect(resetBtn_, &QPushButton::clicked, this, &InstrumentView::onResetEdits);
    connect(testBtn, &QPushButton::clicked, this, &InstrumentView::onTestNote);
    connect(stopBtn, &QPushButton::clicked, this, &InstrumentView::onSilenceTestNote);
    autoTestTimer_ = new QTimer(this);
    autoTestTimer_->setInterval(1000);
    connect(autoTestTimer_, &QTimer::timeout, this, &InstrumentView::onTestNote);
    connect(autoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (on) {
            onTestNote();
            autoTestTimer_->start();
        } else {
            autoTestTimer_->stop();
            onSilenceTestNote();
        }
    });
    btnRow->addWidget(testBtn);
    btnRow->addWidget(stopBtn);
    btnRow->addWidget(autoBtn);
    btnRow->addSpacing(20);
    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(resetBtn_);
    btnRow->addStretch();
    center->addLayout(btnRow);

    center->addStretch();
    root->addLayout(center, 1);

    // Right: previews ------------------------------------------------------
    auto *right = new QVBoxLayout();
    right->setSpacing(8);

    auto *envHdr = new QLabel("ADSR shape", this);
    QFont eh = envHdr->font(); eh.setBold(true);
    envHdr->setFont(eh);
    right->addWidget(envHdr);

    adsr_ = new AdsrPreview(this);
    // Drag-to-edit: dragging the A / D+S / R handles on the preview
    // pushes new nybble values into the spinboxes, which already wire
    // into instr[einum] via onAdChanged / onSrChanged. Wrapping the
    // set calls in updating_=false has no extra effect — the spinbox
    // valueChanged short-circuits when the value is unchanged.
    connect(adsr_, &AdsrPreview::adsrChanged, this,
            [this](int a, int d, int s, int r) {
                attack_->setValue(a);
                decay_->setValue(d);
                sustain_->setValue(s);
                release_->setValue(r);
            });
    adsr_->setToolTip(
        "<b>Heights</b> = SID envelope amplitude.<br>"
        "&nbsp;&nbsp;peak (top) = $FF — SID always attacks to full, A doesn't change this height.<br>"
        "&nbsp;&nbsp;sustain plateau = S × $11 (S=0 silent, S=$F same as peak).<br>"
        "&nbsp;&nbsp;baseline = $00.<br>"
        "<b>Widths</b> = phase duration. A / D / R are schematic linear "
        "scales; real SID timing is exponential (A=$0 ≈ 2 ms, A=$F ≈ 8 s).");
    right->addWidget(adsr_);

    // Compact caption under the graph so the rules are visible without
    // hovering. Uses the same dim subtext style as the rest of the editor.
    auto *adsrLegend = new QLabel(
        "<span style='color:#B0BCC8;font-size:10px'>"
        "Heights: peak (top) = $FF (fixed) · sustain = S×$11 · "
        "baseline = 0  ·  Widths = phase duration"
        "</span>", this);
    adsrLegend->setTextFormat(Qt::RichText);
    adsrLegend->setWordWrap(true);
    right->addWidget(adsrLegend);

    auto *waveHdr = new QLabel("Wavetable program (first 24 steps)", this);
    waveHdr->setFont(eh);
    right->addWidget(waveHdr);

    wavePrev_ = new WavetablePreview(this);
    wavePrev_->setMinimumHeight(40);
    right->addWidget(wavePrev_);

    // Compact summary card — fits in its own bordered frame, kept short.
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::RichText);
    summary_->setMinimumHeight(80);
    summary_->setContentsMargins(10, 8, 10, 8);
    summary_->setAutoFillBackground(true);
    QPalette sp = summary_->palette();
    sp.setColor(QPalette::Window, Theme::C::bgAlt);
    sp.setColor(QPalette::WindowText, Theme::C::text);
    summary_->setPalette(sp);
    right->addWidget(summary_);

    // Pointer preview pane — shows 16 rows of the target table starting
    // at the currently-focused pointer field. Sticky: stays visible
    // after focus moves away; replaced only when a different pointer
    // gets focused.
    pointerPrev_ = new QLabel(this);
    pointerPrev_->setTextFormat(Qt::RichText);
    pointerPrev_->setWordWrap(false);
    pointerPrev_->setMinimumHeight(220);
    pointerPrev_->setContentsMargins(10, 8, 10, 8);
    pointerPrev_->setAutoFillBackground(true);
    pointerPrev_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QPalette pp = pointerPrev_->palette();
    pp.setColor(QPalette::Window, Theme::C::bgAlt);
    pp.setColor(QPalette::WindowText, Theme::C::text);
    pointerPrev_->setPalette(pp);
    pointerPrev_->setText("<i style='color:#B0BCC8'>Click a Wavetable / "
                          "Pulsetable / Filtertable Pos field to see the "
                          "table rows starting at that pointer.</i>");
    right->addWidget(pointerPrev_);

    // Focus-driven pointer preview. installEventFilter on the three
    // pointer spinboxes; the eventFilter override resolves the focused
    // widget to the target table index and calls updatePointerPreview.
    wave_->installEventFilter(this);
    pulse_->installEventFilter(this);
    filter_->installEventFilter(this);
    connect(wave_,   QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (pointerTable_ == WTBL) updatePointerPreview(WTBL, v); });
    connect(pulse_,  QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (pointerTable_ == PTBL) updatePointerPreview(PTBL, v); });
    connect(filter_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (pointerTable_ == FTBL) updatePointerPreview(FTBL, v); });

    right->addStretch();
    root->addLayout(right, 1);

    // Playback-monitor timer: 33 Hz heartbeat (~30 ms) that pulls the
    // current envelope level and active wavetable step for chn[epchn]
    // and pushes them into the preview widgets. The widgets
    // short-circuit redraws when the value hasn't changed, so an idle
    // SID stays idle on the GUI side too (zero repaints / frame).
    playbackTimer_ = new QTimer(this);
    playbackTimer_->setInterval(30); // ~33 Hz
    connect(playbackTimer_, &QTimer::timeout, this,
            &InstrumentView::tickPlayback);
    // Started/stopped by showEvent/hideEvent — only runs while this editor is
    // the visible page (see header). Not started here: the view is hidden
    // behind the pattern editor at startup.

    refresh();
}

void InstrumentView::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    if (playbackTimer_) playbackTimer_->start();
}

void InstrumentView::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (playbackTimer_) playbackTimer_->stop();
}

void InstrumentView::tickPlayback() {
    // Pull the live envelope level for the channel the instrument
    // editor is bound to. sid_getlevels returns the SID envelope
    // generator output (0..255), same source used by PatternView's
    // VU bar — so this driver stays consistent with what the rest of
    // the UI reads as 'is the channel sounding'.
    unsigned char levels[MAX_CHN] = {0};
    sid_getlevels(levels);
    int ch = (epchn >= 0 && epchn < MAX_CHN) ? epchn : 0;
    int level = levels[ch];

    // ADSR moving bar — only repaint when transitioning to/from
    // non-zero, or while non-zero. AdsrPreview::setLiveLevel already
    // short-circuits same-value calls, but skipping it entirely when
    // both old and new are 0 saves a function-call+compare per frame.
    if (level != 0 || adsr_) {
        adsr_->setLiveLevel(level);
    }

    // Wavetable highlight — chn[c].ptr[WTBL] is the 1-based row of the
    // current wavetable step (0 = idle). setActiveStep no-ops when the
    // value matches, so when the engine sits on the same step we draw
    // zero extra frames.
    int step = chn[ch].ptr[WTBL];
    wavePrev_->setActiveStep(step);
}

bool InstrumentView::eventFilter(QObject *o, QEvent *e) {
    if (e->type() == QEvent::FocusIn) {
        if (o == wave_)        updatePointerPreview(WTBL, wave_->value());
        else if (o == pulse_)  updatePointerPreview(PTBL, pulse_->value());
        else if (o == filter_) updatePointerPreview(FTBL, filter_->value());
    }
    return QWidget::eventFilter(o, e);
}

void InstrumentView::updatePointerPreview(int t, int startRow) {
    pointerTable_ = t;
    static const char *tname[] = {"Wavetable", "Pulsetable", "Filtertable"};
    QString head = QString("<b style='font-size:12px;color:#E1E5EA'>%1 @ row $%2</b>"
                           " <span style='color:#B0BCC8;font-size:10px'>"
                           "&nbsp;(first 16 rows, stops at $FF)</span>")
                   .arg(tname[t])
                   .arg(qMax(0, startRow), 2, 16, QLatin1Char('0')).toUpper();
    QString body = "<table cellspacing='0' cellpadding='2' "
                   "style='font-family:monospace;font-size:11px'>"
                   "<tr><th align='left' style='color:#B0BCC8'>idx</th>"
                   "<th align='left' style='color:#B0BCC8'>L</th>"
                   "<th align='left' style='color:#B0BCC8'>R</th>"
                   "<th align='left' style='color:#B0BCC8'>note</th></tr>";
    int row = qMax(0, startRow - 1);  // pointer is 1-based, table is 0-based
    int shown = 0;
    while (shown < 16 && row < MAX_TABLELEN) {
        unsigned char L = ltable[t][row];
        unsigned char R = rtable[t][row];
        QString note;
        if (L == 0xFF) {
            note = R == 0 ? QString("<i>stop</i>")
                          : QString("<i>jump to $%1</i>").arg(R, 2, 16, QLatin1Char('0')).toUpper();
        } else if (t == WTBL && L >= 0xF0 && L <= 0xFE) {
            note = QString("cmd %1XY").arg(L - 0xF0, 1, 16).toUpper();
        }
        QString rowCol = (row + 1 == startRow) ? "#FFD93D" : "#E1E5EA";
        body += QString("<tr><td style='color:%5'>$%1</td>"
                        "<td>$%2</td><td>$%3</td>"
                        "<td style='color:#B0BCC8'>%4</td></tr>")
                .arg(row + 1, 2, 16, QLatin1Char('0')).toUpper()
                .arg(L, 2, 16, QLatin1Char('0')).toUpper()
                .arg(R, 2, 16, QLatin1Char('0')).toUpper()
                .arg(note)
                .arg(rowCol);
        shown++;
        if (L == 0xFF) break;       // stop at jump per the spec
        row++;
    }
    body += "</table>";
    pointerPrev_->setText(head + "<br>" + body);
}

void InstrumentView::refresh() {
    updating_ = true;
    if (einum < 1) einum = 1;
    bool slotChanged = (einum != savedSlot_);

    // Slot change while the previous slot has pending edits -> ask the
    // user. Apply commits the in-progress edits as the new baseline for
    // the slot they're leaving; Reset rolls instr[savedSlot_] back to the
    // baseline before continuing; Cancel snaps einum back to the old
    // slot so the switch never happens.
    if (slotChanged && dirty_ && savedSlot_ > 0 && savedSlot_ < MAX_INSTR) {
        updating_ = false;
        int leaving = savedSlot_;
        int target  = einum;
        QMessageBox box(this);
        box.setWindowTitle("Instrument has pending edits");
        box.setText(QString("Instrument $%1 has unsaved changes.")
                    .arg(leaving, 2, 16, QLatin1Char('0')).toUpper());
        box.setInformativeText("Apply commits them as the new baseline.\n"
                               "Reset rolls instrument back to its previous "
                               "state.\nCancel stays on the current slot.");
        auto *applyB  = box.addButton("Apply",  QMessageBox::AcceptRole);
        auto *resetB  = box.addButton("Reset",  QMessageBox::DestructiveRole);
        auto *cancelB = box.addButton("Cancel", QMessageBox::RejectRole);
        box.exec();
        QAbstractButton *clicked = box.clickedButton();
        if (clicked == cancelB) {
            // Snap back to the slot we were on. No need to touch saved_
            // or dirty_ — the user is still editing the same slot.
            einum = leaving;
            updating_ = true;
            readFromGlobals();
            updating_ = false;
            return;
        }
        if (clicked == resetB) {
            instr[leaving] = saved_;
        }
        // For Apply: current instr[leaving] is the new baseline, nothing
        // more to do — the next branch re-snapshots for the new slot.
        (void)applyB; (void)resetB;
        // Continue with the slot switch.
        einum = target;
        slotChanged = true;
        updating_ = true;
    }

    // Re-snapshot the baseline when we land on a different slot OR when
    // we're not in the middle of an edit. The second clause covers song
    // load — instr[einum] just got rewritten with fresh data and we want
    // Reset to roll back to THAT, not to the pre-load state we captured
    // before the song was opened.
    if (slotChanged || !dirty_) {
        saved_ = instr[einum];
        savedSlot_ = einum;
        dirty_ = false;
        if (applyBtn_) applyBtn_->setEnabled(false);
        if (resetBtn_) resetBtn_->setEnabled(false);
        clearDirtyChrome(applyBtn_);
    }
    readFromGlobals();
    updating_ = false;
}

void InstrumentView::markDirty() {
    if (updating_) return;
    if (savedSlot_ != einum) {
        // refresh() hasn't caught up yet — treat the new slot as the
        // baseline so we don't roll back the wrong instrument.
        saved_ = instr[einum];
        savedSlot_ = einum;
    }
    if (dirty_) return;   // already amber-styled; no UI churn per keystroke
    dirty_ = true;
    if (applyBtn_) {
        applyBtn_->setEnabled(true);
        applyBtn_->setText("Apply *");
        applyBtn_->setStyleSheet(
            "QPushButton { background:#E89B3C; color:#1A1A1A; "
            "border:1px solid #FFD93D; font-weight:bold; padding:4px 12px; }"
            "QPushButton:hover { background:#FFD93D; }");
    }
    if (resetBtn_) resetBtn_->setEnabled(true);
}

static void clearDirtyChrome(QPushButton *b) {
    if (!b) return;
    b->setText("Apply");
    b->setStyleSheet("");
}

void InstrumentView::onApplyEdits() {
    if (!dirty_) return;
    saved_ = instr[einum];
    savedSlot_ = einum;
    dirty_ = false;
    if (applyBtn_) applyBtn_->setEnabled(false);
    if (resetBtn_) resetBtn_->setEnabled(false);
    clearDirtyChrome(applyBtn_);
    if (summary_) summary_->setText(QString("<i style='color:#A1C181'>"
                                            "Applied — current state is the "
                                            "new revert baseline.</i>"));
    emit edited();
}

void InstrumentView::onResetEdits() {
    if (!dirty_) return;
    instr[einum] = saved_;
    dirty_ = false;
    if (applyBtn_) applyBtn_->setEnabled(false);
    if (resetBtn_) resetBtn_->setEnabled(false);
    clearDirtyChrome(applyBtn_);
    readFromGlobals();
    if (summary_) summary_->setText(QString("<i style='color:#FFD93D'>"
                                            "Reset — rolled back to the last "
                                            "Apply baseline.</i>"));
    emit edited();
}

void InstrumentView::readFromGlobals() {
    updating_ = true;
    INSTR &ins = instr[einum];
    char buf[MAX_INSTRNAMELEN + 1];
    std::memcpy(buf, ins.name, MAX_INSTRNAMELEN);
    buf[MAX_INSTRNAMELEN] = 0;
    QString incoming = QString::fromLocal8Bit(buf);
    // Don't yank focus / reset the caret on every refresh. The 50 Hz tick
    // re-reads instr[].name via this function; setText() on a focused
    // QLineEdit moves the caret to the end and made the user lose every
    // character they typed mid-word. Skip the write while the user is
    // actively editing (focus held), and otherwise only write when the
    // content actually changed so other consumers (preset Apply, song
    // load) still update the field.
    if (!name_->hasFocus() && name_->text() != incoming) {
        name_->setText(incoming);
    }
    attack_->setValue((ins.ad >> 4) & 0xf);
    decay_->setValue(ins.ad & 0xf);
    sustain_->setValue((ins.sr >> 4) & 0xf);
    release_->setValue(ins.sr & 0xf);
    wave_->setValue(ins.ptr[WTBL]);
    pulse_->setValue(ins.ptr[PTBL]);
    filter_->setValue(ins.ptr[FTBL]);
    vibParam_->setValue(ins.ptr[STBL]);
    vibDelay_->setValue(ins.vibdelay);
    gateTimer_->setValue(ins.gatetimer);
    firstWave_->setValue(ins.firstwave);
    adsr_->setAdsr(attack_->value(), decay_->value(),
                   sustain_->value(), release_->value());
    wavePrev_->setStart(ins.ptr[WTBL]);

    QString fwDesc;
    if (ins.firstwave == 0x09)      fwDesc = "gate+test (std)";
    else if (ins.firstwave == 0x00) fwDesc = "legato (no gate change)";
    else if (ins.firstwave == 0xfe) fwDesc = "gate off";
    else if (ins.firstwave == 0xff) fwDesc = "gate on";
    else                            fwDesc = "custom";

    bool hrDisabled = (ins.gatetimer & 0x80) != 0;
    bool gateOffDisabled = (ins.gatetimer & 0x40) != 0;
    int gateTicks = ins.gatetimer & 0x3f;
    QString gateDesc = QString("%1 ticks").arg(gateTicks);
    if (hrDisabled) gateDesc += ", no HR";
    if (gateOffDisabled) gateDesc += ", no gate-off";

    summary_->setText(QString(
        "<b style='color:#FFD93D'>Instrument $%1</b><br>"
        "ADSR = A%2 D%3 S%4 R%5"
        "<br>1stFrame Wave $%6 — %7"
        "<br>Gate/HR timer $%8 — %9")
        .arg(einum, 2, 16, QLatin1Char('0'))
        .arg(attack_->value(), 1, 16).arg(decay_->value(), 1, 16)
        .arg(sustain_->value(), 1, 16).arg(release_->value(), 1, 16)
        .arg(ins.firstwave, 2, 16, QLatin1Char('0')).arg(fwDesc)
        .arg(ins.gatetimer, 2, 16, QLatin1Char('0')).arg(gateDesc)
        .toUpper());
    updating_ = false;
}

void InstrumentView::onNameChanged(const QString &t) {
    if (updating_) return;
    QByteArray before = captureSongSnapshot();
    QByteArray ba = t.toLocal8Bit();
    INSTR &ins = instr[einum];
    int n = qMin<int>(MAX_INSTRNAMELEN - 1, ba.size());
    std::memcpy(ins.name, ba.constData(), n);
    for (int i = n; i < MAX_INSTRNAMELEN; i++) ins.name[i] = 0;
    char buf[MAX_INSTRNAMELEN + 1];
    std::memcpy(buf, ins.name, MAX_INSTRNAMELEN);
    buf[MAX_INSTRNAMELEN] = 0;
    markDirty();
    pushEditIfChanged(this, std::move(before), "Instrument name");
    emit edited();
}

void InstrumentView::writeAd() {
    instr[einum].ad = (attack_->value() << 4) | decay_->value();
}
void InstrumentView::writeSr() {
    instr[einum].sr = (sustain_->value() << 4) | release_->value();
}

void InstrumentView::onAdChanged(int) {
    if (updating_) return;
    QByteArray before = captureSongSnapshot();
    writeAd();
    adsr_->setAdsr(attack_->value(), decay_->value(),
                   sustain_->value(), release_->value());
    markDirty();
    pushEditIfChanged(this, std::move(before), "Instrument ADSR");
    emit edited();
}
void InstrumentView::onSrChanged(int) {
    if (updating_) return;
    QByteArray before = captureSongSnapshot();
    writeSr();
    adsr_->setAdsr(attack_->value(), decay_->value(),
                   sustain_->value(), release_->value());
    markDirty();
    pushEditIfChanged(this, std::move(before), "Instrument ADSR");
    emit edited();
}
void InstrumentView::onWtChanged(int v) {
    if (updating_) return;
    QByteArray before = captureSongSnapshot();
    instr[einum].ptr[WTBL] = v;
    wavePrev_->setStart(v);
    markDirty();
    pushEditIfChanged(this, std::move(before), "Instrument param");
    emit edited();
}
void InstrumentView::onPtChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].ptr[PTBL] = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}
void InstrumentView::onFtChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].ptr[FTBL] = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}
void InstrumentView::onVibParamChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].ptr[STBL] = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}
void InstrumentView::onVibDelayChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].vibdelay = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}
void InstrumentView::onGateChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].gatetimer = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}
void InstrumentView::onFirstWaveChanged(int v) {
    if (updating_) return;
    QByteArray b = captureSongSnapshot(); instr[einum].firstwave = v;
    markDirty(); pushEditIfChanged(this, std::move(b), "Instrument param"); emit edited();
}

void InstrumentView::onGotoWaveTable() {
    gototable(WTBL, instr[einum].ptr[WTBL] > 0 ? instr[einum].ptr[WTBL] - 1 : 0);
    editmode = 3; // EDIT_TABLES
    emit edited();
}
void InstrumentView::onGotoPulseTable() {
    gototable(PTBL, instr[einum].ptr[PTBL] > 0 ? instr[einum].ptr[PTBL] - 1 : 0);
    editmode = 3;
    emit edited();
}
void InstrumentView::onGotoFilterTable() {
    gototable(FTBL, instr[einum].ptr[FTBL] > 0 ? instr[einum].ptr[FTBL] - 1 : 0);
    editmode = 3;
    emit edited();
}

void InstrumentView::onTestNote() {
    int note = FIRSTNOTE + epoctave * 12; // C of current octave
    playtestnote(note, einum, epchn);
}
void InstrumentView::onSilenceTestNote() {
    releasenote(epchn);
}

// Preset starter envelopes from docs/sid-composition.md.
// Tuple: name (display), AD byte, SR byte, 1stFrame, "label written into ins.name".
namespace {
struct Preset {
    const char *label;
    unsigned char ad;
    unsigned char sr;
    unsigned char firstwave;
    const char *insName;
};
static const Preset kPresets[] = {
    {"",                       0,    0,    0x09, ""},          // index 0 placeholder
    {"Bass — tight pulse",     0x09, 0x00, 0x09, "Bass"},
    {"Bass — triangle soft",   0x0A, 0xFC, 0x09, "Bass tri"},
    {"Lead — sawtooth",        0x08, 0x85, 0x09, "Lead"},
    {"Pluck — fast decay",     0x00, 0xC0, 0x09, "Pluck"},
    {"Pad — slow, sustain",    0x84, 0xC5, 0x09, "Pad"},
    {"Brass — punchy",         0x2A, 0xF4, 0x09, "Brass"},
    {"Organ — full sustain",   0x00, 0xF0, 0x09, "Organ"},
    {"Bell — long decay",      0x0B, 0x00, 0x09, "Bell"},
    {"Strings — soft sus",     0x88, 0xCA, 0x09, "Strings"},
    // Drum kit — sensible ADSR seeds. Wavetable still needs to be wired by
    // hand (kick: pitch slide + noise burst; snare: noise+pulse; hat: noise).
    // docs/sid-composition.md §3 covers the wavetable programs.
    {"Drum kit: Kick",         0x00, 0x09, 0x09, "Kick"},
    {"Drum kit: Snare",        0x00, 0x05, 0x09, "Snare"},
    {"Drum kit: Closed hat",   0x00, 0x02, 0x09, "Hat-cl"},
    {"Drum kit: Open hat",     0x00, 0x07, 0x09, "Hat-op"},
};
}

void InstrumentView::applyPreset(int index) {
    if (index <= 0 || index >= (int)(sizeof(kPresets) / sizeof(kPresets[0]))) return;
    QByteArray before = captureSongSnapshot();
    const Preset &p = kPresets[index];
    INSTR &ins = instr[einum];
    ins.ad = p.ad;
    ins.sr = p.sr;
    ins.firstwave = p.firstwave;
    ins.gatetimer = 0x02;    // default hard-restart timing
    ins.vibdelay  = 0x00;
    // Seed a minimal wavetable program so the preset actually makes
    // sound when played. Each preset appends a 2-step program at the
    // next free wavetable slot: 'hold firstwave' then 'jump-back to
    // step 1' (the WTBL JUMP encoding). We claim slots even if the
    // user later edits them; this gets a fresh-from-File-New user a
    // patch that auditions cleanly out of the box.
    // wavetable format: ltable[c]/rtable[c] columns indexed 1.. with
    // ltable=0xff + rtable=jump_target_index terminating the program.
    {
        // Append a minimal 2-step wavetable program at the next free row.
        // Step n: hold firstwave. Step n+1: JUMP (0xff) -> rtable = step n
        // so the wave loops indefinitely after the first frame. ltable[c]
        // is column 0 (left half), rtable[c] is column 1 (right half).
        int wstart = 1;
        while (wstart < MAX_TABLELEN - 2 && ltable[WTBL][wstart] != 0) wstart++;
        if (wstart < MAX_TABLELEN - 2) {
            ltable[WTBL][wstart    ] = p.firstwave;
            rtable[WTBL][wstart    ] = 0x00;
            ltable[WTBL][wstart + 1] = 0xff;            // JUMP marker
            rtable[WTBL][wstart + 1] = (unsigned char)wstart;
            ins.ptr[WTBL] = (unsigned char)wstart;
            ins.ptr[PTBL] = 0;
            ins.ptr[FTBL] = 0;
            ins.ptr[STBL] = 0;
        }
    }
    // Only rename if the slot is empty or already a default placeholder.
    char nameBuf[MAX_INSTRNAMELEN + 1];
    std::memcpy(nameBuf, ins.name, MAX_INSTRNAMELEN);
    nameBuf[MAX_INSTRNAMELEN] = 0;
    QString cur = QString::fromLocal8Bit(nameBuf).trimmed();
    if (cur.isEmpty()) {
        std::strncpy(ins.name, p.insName, MAX_INSTRNAMELEN - 1);
        ins.name[MAX_INSTRNAMELEN - 1] = 0;
    }
    pushEditIfChanged(this, std::move(before), "Apply preset");
    refresh();
    emit edited();
}

// AdsrPreview / WavetablePreview are Q_OBJECT classes defined in this
// .cpp, so MOC needs their definitions to be visible here for the
// generated meta-object to link.
#include "InstrumentView.moc"

