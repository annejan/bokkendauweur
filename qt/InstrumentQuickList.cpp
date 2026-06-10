#include "InstrumentQuickList.h"
#include "Theme.h"
#include "InstrColors.h"
#include "CoreEvents.h"

#include <QBrush>
#include <QColor>
#include <QMouseEvent>
#include <cstring>

extern "C" {
#include "gcommon.h"
#include "gplay.h"
extern INSTR instr[MAX_INSTR];
extern int einum;
extern int song_channels;
extern int songinit;
}

InstrumentQuickList::InstrumentQuickList(QWidget *parent) : QListWidget(parent) {
    setAccessibleName("Instruments");
    setMinimumWidth(140);
    setMaximumWidth(200);
    setFont(Theme::monoFont(10));
    QPalette pal = palette();
    pal.setColor(QPalette::Base, Theme::C::bgAlt);
    pal.setColor(QPalette::Text, Theme::C::text);
    pal.setColor(QPalette::Highlight, Theme::C::editRow);
    setPalette(pal);
    for (int i = 1; i < MAX_INSTR; i++) addItem(QString());
    refresh();
    connect(this, &QListWidget::currentRowChanged, this, &InstrumentQuickList::onRowChanged);

    // Flash samples chn[] at ~33 Hz to drive the per-instrument fade animation.
    // The fade itself is a continuous animation (sampling is the right model),
    // but it only needs to run WHILE the song is playing — gate the timer on
    // the transport notification so it stops free-running when idle/stopped.
    // tickFlash() also self-stops once the last flash has decayed after a stop.
    flashTimer_.setInterval(30);
    connect(&flashTimer_, &QTimer::timeout, this, &InstrumentQuickList::tickFlash);
    if (auto *ev = CoreEvents::instance()) {
        connect(ev, &CoreEvents::transportChanged, this, [this](bool playing){
            if (playing && blinkEnabled_) flashTimer_.start();
        });
    }
}

// Per-channel hot colours. Channels 0..2 = SID1 (primary RGB so blends are
// obvious), 3..5 = SID2 (secondaries — distinct from SID1 but still readable
// on the dark background).
static const QColor chanColor[6] = {
    QColor(255,  80,  80),  // ch1  red
    QColor( 80, 255,  80),  // ch2  green
    QColor( 80, 160, 255),  // ch3  blue
    QColor( 80, 220, 220),  // ch4  cyan
    QColor(220,  80, 220),  // ch5  magenta
    QColor(255, 200,  80),  // ch6  amber
};

void InstrumentQuickList::setBlinkEnabled(bool on) {
    blinkEnabled_ = on;
    if (on) {
        // Only run while the song is actually playing; otherwise wait for the
        // transportChanged notification to start it (nothing flashes when
        // stopped — flash is gated on songinit != PLAY_STOPPED).
        if (songinit != PLAY_STOPPED) flashTimer_.start();
    } else {
        flashTimer_.stop();
        // Clear lingering tinted backgrounds — but keep the per-instrument
        // colour if that toggle is on. paintCold() picks the right base.
        for (int i = 1; i < MAX_INSTR; i++) {
            for (int c = 0; c < 6; c++) flash_[i][c] = 0;
        }
        refresh();
    }
}

void InstrumentQuickList::setAllColored(bool on) {
    setAllInstrColored(on);
    saveInstrColorMask();
    refresh();
    emit colorMaskChanged();
}

void InstrumentQuickList::mouseDoubleClickEvent(QMouseEvent *e) {
    // Double-click a row to toggle that instrument's colour bit. Routes
    // to setInstrColored / saveInstrColorMask in InstrColors.cpp + emits
    // colorMaskChanged so the pattern view repaints with the new state.
    auto idx = indexAt(e->pos());
    if (!idx.isValid()) { QListWidget::mouseDoubleClickEvent(e); return; }
    int slot = idx.row() + 1;
    if (slot < 1 || slot >= MAX_INSTR) {
        QListWidget::mouseDoubleClickEvent(e);
        return;
    }
    setInstrColored((unsigned char)slot, !isInstrColored((unsigned char)slot));
    saveInstrColorMask();
    refresh();
    emit colorMaskChanged();
    e->accept();
}

void InstrumentQuickList::tickFlash() {
    // Bump flash for any instrument currently sounding on the active channels.
    // songinit guards against picking up stale chn[].instr while stopped.
    if (songinit != PLAY_STOPPED) {
        int nc = song_channels;
        if (nc <= 0 || nc > MAX_CHN) nc = MAX_CHN;
        // PortAudio runs at ~10 ms latency — chn[c].instr is close enough to
        // what's audible that we don't need a snapshot ring to compensate.
        for (int c = 0; c < nc; c++) {
            unsigned ins = chn[c].instr;
            if (ins >= 1 && ins < MAX_INSTR) flash_[ins][c] = 12;
        }
    }
    // Paint + decay. Sum weighted channel colours over the base for each row.
    QColor base = Theme::C::bgAlt;
    bool anyActive = false;   // any flash still decaying this frame?
    for (int i = 1; i < MAX_INSTR; i++) {
        auto *it = item(i - 1);
        if (!it) continue;
        float wSum = 0.f;
        float r = 0, g = 0, b = 0;
        for (int c = 0; c < 6; c++) {
            uint8_t f = flash_[i][c];
            if (!f) continue;
            float w = f / 12.0f;
            wSum += w;
            r += chanColor[c].red()   * w;
            g += chanColor[c].green() * w;
            b += chanColor[c].blue()  * w;
            flash_[i][c] = f - 1;
        }
        // Resolve the 'cold' background once — palette colour when this
        // instrument is opted in via instrColor(), theme base otherwise.
        QColor cold = base;
        QColor pc = instrColor((unsigned char)i);
        if (pc.isValid()) cold = pc;
        if (wSum <= 0.f) {
            // Repaint the cold colour once after the last channel's flash
            // hit zero; skip otherwise to avoid setBackground churn.
            if (it->background().color() != cold) it->setBackground(QBrush(cold));
            continue;
        }
        // Lerp cold -> hot mix by min(wSum,1).
        float invW = 1.f / wSum;
        QColor hot(int(r * invW), int(g * invW), int(b * invW));
        float t = qMin(wSum, 1.0f);
        QColor mix(
            int(cold.red()   * (1 - t) + hot.red()   * t),
            int(cold.green() * (1 - t) + hot.green() * t),
            int(cold.blue()  * (1 - t) + hot.blue()  * t));
        it->setBackground(QBrush(mix));
        anyActive = true;
    }
    // Once the song is stopped and the last flash has faded, stop the timer so
    // it no longer free-runs while idle. transportChanged restarts it on play.
    if (songinit == PLAY_STOPPED && !anyActive)
        flashTimer_.stop();
}

void InstrumentQuickList::refresh() {
    updating_ = true;
    for (int i = 1; i < MAX_INSTR; i++) {
        char buf[MAX_INSTRNAMELEN + 1];
        std::memcpy(buf, instr[i].name, MAX_INSTRNAMELEN);
        buf[MAX_INSTRNAMELEN] = 0;
        QString name = QString::fromLocal8Bit(buf).trimmed();
        if (name.isEmpty()) name = "—";
        auto *it = item(i - 1);
        it->setText(QString("%1  %2")
            .arg(i, 2, 16, QLatin1Char('0')).toUpper().arg(name));
        // Per-instrument opt-in: instrColor() returns a valid QColor only
        // for slots the user toggled on via double-click.
        QColor pc = instrColor((unsigned char)i);
        if (pc.isValid()) {
            it->setBackground(QBrush(pc));
            it->setForeground(QBrush(QColor(255, 255, 255)));
        } else {
            it->setBackground(QBrush());
            it->setForeground(QBrush(Theme::C::text));
        }
    }
    if (einum >= 1 && einum < MAX_INSTR)
        setCurrentRow(einum - 1);
    updating_ = false;
}

void InstrumentQuickList::onRowChanged(int row) {
    if (updating_ || row < 0) return;
    einum = row + 1;
    emit instrumentChosen();
}
