#include "InstrumentTrace.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFontMetrics>
#include <algorithm>

extern "C" {
#include "gcommon.h"
extern INSTR instr[MAX_INSTR];
extern unsigned char ltable[MAX_TABLES][MAX_TABLELEN];
extern unsigned char rtable[MAX_TABLES][MAX_TABLELEN];
}

// --- trace colours (fixed + consistent, referenced by paint + legend) -------
namespace {
const QColor kWaveCol  (0x3F, 0xB9, 0x50);  // green
const QColor kPulseCol (0x39, 0xC5, 0xCF);  // cyan
const QColor kCutCol   (0xD9, 0xA4, 0x41);  // orange
const QColor kEnvCol   (0xE5, 0x48, 0x4D);  // red

struct TraceInfo { const char *name; const char *range; QColor col; int max; };

bool tableIdxOk(int p) { return p >= 1 && p <= MAX_TABLELEN; }
}

InstrTrace computeInstrumentTrace(int instrNum, int maxFrames, int sustainCap) {
    InstrTrace t;
    if (instrNum < 1 || instrNum >= MAX_INSTR) return t;
    const INSTR &ins = instr[instrNum];

    // --- table-walk state (mirrors the per-channel CHN fields) ---
    int wptr = ins.ptr[WTBL], pptr = ins.ptr[PTBL], fptr = ins.ptr[FTBL];
    int wavetime = 0, pulsetime = 0, filtertime = 0;
    int wave   = ins.firstwave;
    int pulse  = 0;
    int cutoff = 0;
    int filterctrl = 0;

    // --- envelope timeline (analytical; gate held until the sustain cap) ---
    // SID attack periods (ms) for the full 0->255 rise; decay/release are ~3x.
    static const int atkMs[16] =
        {2,8,16,24,38,56,68,80,100,250,500,800,1000,3000,5000,8000};
    auto framesFor = [](int ms){ return std::max(1, (ms + 10) / 20); }; // ~50 Hz
    const int A = (ins.ad >> 4) & 0xf, D = ins.ad & 0xf;
    const int S = (ins.sr >> 4) & 0xf, R = ins.sr & 0xf;
    const int susAmp   = S * 17;                       // SID sustain level
    const int attackF  = framesFor(atkMs[A]);
    const int decayF   = framesFor(atkMs[D] * 3) * (255 - susAmp) / 255;
    const int relStart = attackF + decayF + std::max(1, sustainCap);
    const int releaseF = framesFor(atkMs[R] * 3) * (susAmp ? susAmp : 1) / 255;
    t.releaseFrame = relStart;

    const int N = std::min(maxFrames, relStart + releaseF + 1);

    auto stepWave = [&]() {
        if (!tableIdxOk(wptr)) { wptr = 0; return; }
        int wv = ltable[WTBL][wptr - 1];
        if (wv > WAVELASTDELAY) {
            if (wv < WAVESILENT) {
                wave = wv;                                   // waveform set
            } else if (wv <= WAVELASTSILENT) {
                wave = wv & 0xf;                             // waveform-less value
            } else if (wv >= WAVECMD && wv <= WAVELASTCMD) { // command
                int param = rtable[WTBL][wptr - 1];
                switch (wv & 0xf) {
                case CMD_DONOTHING: case CMD_SETWAVEPTR: case CMD_FUNKTEMPO:
                    wptr = 0; return;                        // these stop the song
                case CMD_SETWAVE:        wave = param; break;
                case CMD_SETPULSEPTR:    pptr = param; pulsetime = 0; break;
                case CMD_SETFILTERPTR:   fptr = param; filtertime = 0; break;
                case CMD_SETFILTERCTRL:  filterctrl = param; if (!filterctrl) fptr = 0; break;
                case CMD_SETFILTERCUTOFF:cutoff = param; break;
                default: break;          // SETAD/SETSR (envelope precomputed),
                                         // porta / vibrato / toneporta: freq only
                }
            }
        } else {
            // wavetable delay: stay on the row until wavetime catches up
            if (wavetime != wv) { wavetime++; return; }
        }
        wavetime = 0;
        wptr++;
        if (tableIdxOk(wptr) && ltable[WTBL][wptr - 1] == 0xff)
            wptr = rtable[WTBL][wptr - 1];
    };

    auto stepPulse = [&]() {
        if (!tableIdxOk(pptr)) { pptr = 0; return; }
        if (ltable[PTBL][pptr - 1] == 0xff) {
            pptr = rtable[PTBL][pptr - 1];
            if (!tableIdxOk(pptr)) { pptr = 0; return; }
        }
        if (!pulsetime) {
            if (ltable[PTBL][pptr - 1] >= 0x80) {            // set pulse
                pulse = ((ltable[PTBL][pptr - 1] & 0xf) << 8) | rtable[PTBL][pptr - 1];
                pptr++;
            } else {
                pulsetime = ltable[PTBL][pptr - 1];          // sweep duration
            }
        }
        if (pulsetime) {
            if (!tableIdxOk(pptr)) { pptr = 0; return; }
            int speed = rtable[PTBL][pptr - 1];
            pulse = (speed < 0x80) ? (pulse + speed) & 0xfff
                                   : (pulse + speed - 0x100) & 0xfff;
            if (!--pulsetime) pptr++;
        }
    };

    auto stepFilter = [&]() {
        if (!tableIdxOk(fptr)) { fptr = 0; return; }
        if (ltable[FTBL][fptr - 1] == 0xff) {
            fptr = rtable[FTBL][fptr - 1];
            if (!tableIdxOk(fptr)) { fptr = 0; return; }
        }
        if (!filtertime) {
            if (ltable[FTBL][fptr - 1] >= 0x80) {            // set type + ctrl
                filterctrl = rtable[FTBL][fptr - 1];
                fptr++;
                if (tableIdxOk(fptr) && ltable[FTBL][fptr - 1] == 0x00) {
                    cutoff = rtable[FTBL][fptr - 1];         // combined cutoff set
                    fptr++;
                }
            } else if (ltable[FTBL][fptr - 1]) {
                filtertime = ltable[FTBL][fptr - 1];         // sweep duration
            } else {
                cutoff = rtable[FTBL][fptr - 1];             // cutoff set
                fptr++;
            }
        }
        if (filtertime) {
            if (!tableIdxOk(fptr)) { fptr = 0; return; }
            cutoff = (cutoff + rtable[FTBL][fptr - 1]) & 0xff;   // signed-wrap sweep
            if (!--filtertime) fptr++;
        }
    };

    t.wave.reserve(N); t.pulse.reserve(N); t.cutoff.reserve(N); t.env.reserve(N);
    for (int f = 0; f < N; ++f) {
        stepWave();
        stepPulse();
        stepFilter();

        int env;
        if (f < attackF)              env = 255 * f / std::max(1, attackF);
        else if (f < attackF + decayF) env = 255 - (255 - susAmp) * (f - attackF) / std::max(1, decayF);
        else if (f < relStart)         env = susAmp;
        else if (f < relStart + releaseF) env = susAmp - susAmp * (f - relStart) / std::max(1, releaseF);
        else                           env = 0;

        t.wave.push_back(wave & 0xff);
        t.pulse.push_back(pulse & 0xfff);
        t.cutoff.push_back(cutoff & 0xff);
        t.env.push_back(std::clamp(env, 0, 255));
    }
    t.frames = N;
    return t;
}

// ---------------------------------------------------------------------------
// RegisterGraphView
// ---------------------------------------------------------------------------
static constexpr int kLegendH = 26;
static constexpr int kPad     = 8;

RegisterGraphView::RegisterGraphView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(150);
    setMouseTracking(true);
    setToolTip("SID register values driven by this instrument's tables, frame "
               "by frame. Hover to read exact values; sustain is capped so the "
               "trace stays bounded.");
}

void RegisterGraphView::setInstrument(int instrNum) {
    instr_ = instrNum;
    tr_ = computeInstrumentTrace(instrNum);
    hoverFrame_ = -1;
    update();
}

QRect RegisterGraphView::plotRect() const {
    return QRect(kPad, kPad, width() - 2 * kPad,
                 height() - 2 * kPad - kLegendH);
}

int RegisterGraphView::xForFrame(int f) const {
    QRect r = plotRect();
    if (tr_.frames <= 1) return r.left();
    return r.left() + f * (r.width() - 1) / (tr_.frames - 1);
}

int RegisterGraphView::frameAtX(int x) const {
    QRect r = plotRect();
    if (tr_.frames <= 1 || r.width() <= 1) return 0;
    int f = (x - r.left()) * (tr_.frames - 1) / (r.width() - 1);
    return std::clamp(f, 0, tr_.frames - 1);
}

void RegisterGraphView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::C::bgAlt);
    const QRect r = plotRect();
    p.fillRect(r, Theme::C::bgBase);

    if (tr_.frames <= 0) {
        p.setPen(Theme::C::textDim);
        p.drawText(r, Qt::AlignCenter, "No instrument data");
        return;
    }

    // Faint horizontal grid (0 / 25 / 50 / 75 / 100 %).
    p.setPen(QPen(Theme::C::sep, 1, Qt::DotLine));
    for (int i = 0; i <= 4; ++i) {
        int y = r.top() + i * r.height() / 4;
        p.drawLine(r.left(), y, r.right(), y);
    }

    // Release marker.
    if (tr_.releaseFrame >= 0 && tr_.releaseFrame < tr_.frames) {
        int rx = xForFrame(tr_.releaseFrame);
        p.setPen(QPen(Theme::C::textDim, 1, Qt::DashLine));
        p.drawLine(rx, r.top(), rx, r.bottom());
    }

    auto drawTrace = [&](const QVector<int> &v, int vmax, QColor col) {
        if (v.isEmpty()) return;
        QPainterPath path;
        for (int f = 0; f < v.size(); ++f) {
            double n = std::clamp(v[f] / double(vmax), 0.0, 1.0);
            int x = xForFrame(f);
            int y = r.bottom() - int(n * (r.height() - 1));
            if (f == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        p.setPen(QPen(col, 1.7));
        p.drawPath(path);
    };
    drawTrace(tr_.cutoff, 255,  kCutCol);
    drawTrace(tr_.pulse,  4095, kPulseCol);
    drawTrace(tr_.wave,   255,  kWaveCol);
    drawTrace(tr_.env,    255,  kEnvCol);

    // Hover crosshair + readout.
    if (hoverFrame_ >= 0 && hoverFrame_ < tr_.frames) {
        int hx = xForFrame(hoverFrame_);
        p.setPen(QPen(Theme::C::text, 1));
        p.drawLine(hx, r.top(), hx, r.bottom());

        const QString lines[5] = {
            QString("Frame %1").arg(hoverFrame_),
            QString("Wave  $%1").arg(tr_.wave[hoverFrame_], 2, 16, QLatin1Char('0')).toUpper(),
            QString("Pulse $%1").arg(tr_.pulse[hoverFrame_], 3, 16, QLatin1Char('0')).toUpper(),
            QString("Cutf  $%1").arg(tr_.cutoff[hoverFrame_], 2, 16, QLatin1Char('0')).toUpper(),
            QString("Env   %1").arg(tr_.env[hoverFrame_]),
        };
        const QColor swatch[5] = { Qt::transparent, kWaveCol, kPulseCol, kCutCol, kEnvCol };
        QFont mf = font(); mf.setFamily("monospace"); mf.setPointSize(8);
        p.setFont(mf);
        QFontMetrics fm(mf);
        int bw = 0; for (auto &l : lines) bw = std::max(bw, fm.horizontalAdvance(l));
        bw += 22; int bh = 5 * (fm.height() + 1) + 6;
        int bx = (hx + bw + 12 < r.right()) ? hx + 10 : hx - bw - 10;
        int by = r.top() + 4;
        QColor box = Theme::C::bgAlt; box.setAlpha(235);
        p.setPen(QPen(Theme::C::sep, 1));
        p.setBrush(box);
        p.drawRect(bx, by, bw, bh);
        for (int i = 0; i < 5; ++i) {
            int ly = by + 4 + i * (fm.height() + 1);
            if (swatch[i] != QColor(Qt::transparent)) {
                p.fillRect(bx + 5, ly + 2, 8, 8, swatch[i]);
            }
            p.setPen(Theme::C::text);
            p.drawText(bx + 16, ly + fm.ascent(), lines[i]);
        }
    }

    // Legend strip below the plot.
    static const TraceInfo legend[4] = {
        { "Waveform", "0–255",  kWaveCol,  255  },
        { "Pulse",    "0–FFF",  kPulseCol, 4095 },
        { "Cutoff",   "0–255",  kCutCol,   255  },
        { "Envelope", "0–255",  kEnvCol,   255  },
    };
    QFont lf = font(); lf.setPointSize(8); p.setFont(lf);
    QFontMetrics lfm(lf);
    int lx = kPad;
    int ly = height() - kLegendH + (kLegendH - lfm.height()) / 2;
    for (const auto &t : legend) {
        p.fillRect(lx, ly + 2, 10, 10, t.col);
        lx += 14;
        QString lbl = QString("%1 (%2)").arg(t.name).arg(t.range);
        p.setPen(Theme::C::textDim);
        p.drawText(lx, ly + lfm.ascent(), lbl);
        lx += lfm.horizontalAdvance(lbl) + 16;
    }
}

void RegisterGraphView::mouseMoveEvent(QMouseEvent *e) {
    if (tr_.frames <= 0) return;
    int f = plotRect().contains(QPoint(e->pos().x(), plotRect().center().y()))
            ? frameAtX(e->pos().x()) : -1;
    if (f != hoverFrame_) { hoverFrame_ = f; update(); }
}

void RegisterGraphView::leaveEvent(QEvent *) {
    if (hoverFrame_ != -1) { hoverFrame_ = -1; update(); }
}
