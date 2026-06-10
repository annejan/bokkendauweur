#include "StatusStrip.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QEvent>

class ClickableLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
signals:
    void clicked();
protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) emit clicked();
        QLabel::mousePressEvent(e);
    }
};

#include "StatusStrip.moc"

extern "C" {
#include "gcommon.h"
#include "gplay.h"
extern int editmode;
extern int eppos;
extern int epchn;
extern int epoctave;
extern int einum;
extern int esnum;
extern int followplay;
extern int epnum[MAX_CHN];
extern int pattlen[MAX_PATT];
extern int espos[MAX_CHN];
extern int songlen[MAX_SONGS][MAX_CHN];
extern unsigned sidmodel;
extern unsigned sid2model;
extern int stereo_mode;
extern unsigned multiplier;
extern unsigned ntsc;
extern unsigned keypreset;
extern int recordmode;
extern CHN chn[MAX_CHN];
extern INSTR instr[MAX_INSTR];
extern unsigned char pattern[MAX_PATT][MAX_PATTROWS*4+4];
int isplaying(void);
}

static QLabel *makeSegment(const QString &init, const QColor &fg, QWidget *parent) {
    auto *l = new QLabel(init, parent);
    l->setFont(Theme::monoFont(10));
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, fg);
    l->setPalette(p);
    l->setContentsMargins(8, 0, 8, 0);
    l->setMinimumHeight(22);
    return l;
}

static ClickableLabel *makeClickable(const QString &init, const QColor &fg, QWidget *parent) {
    auto *l = new ClickableLabel(init, parent);
    l->setFont(Theme::monoFont(10));
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, fg);
    l->setPalette(p);
    l->setContentsMargins(8, 0, 8, 0);
    l->setMinimumHeight(22);
    l->setCursor(Qt::PointingHandCursor);
    return l;
}

StatusStrip::StatusStrip(QWidget *parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::C::bgAlt);
    setPalette(pal);
    setFixedHeight(24);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    transport_ = makeSegment("■ STOPPED", Theme::C::textDim, this);
    position_  = makeSegment("Row --/--", Theme::C::text, this);
    tempo_     = makeClickable("Spd 1x", Theme::C::text, this);
    tempo_->setToolTip("Click to cycle speed multiplier (½x / 1x / 2x / 3x / 4x). "
                       "Multiplier scales pattern-tempo precision; base tick rate "
                       "is set by the PAL/NTSC segment. Also: Shift+F5/F6.");
    connect(tempo_, &ClickableLabel::clicked, this, &StatusStrip::tempoClicked);
    octave_    = makeClickable("Oct 2", Theme::C::text, this);
    octave_->setToolTip("Record-mode octave (0..7). Click cycles up; mouse "
                        "wheel scrolls ±1. Keyboard: * raises, / lowers.");
    connect(octave_, &ClickableLabel::clicked, this, &StatusStrip::octaveClicked);
    octave_->installEventFilter(this);
    record_    = makeClickable("● REC", Theme::C::vuRed, this);
    record_->setToolTip("Pattern-editor record mode. Red = ON: note / hex keys "
                        "write into the pattern at the cursor. Dim = OFF: keys "
                        "audition (jam) without modifying the song. Toggle: Space "
                        "or click here.");
    connect(record_, &ClickableLabel::clicked, this, &StatusStrip::recordClicked);
    instr_     = makeSegment("Ins 01", Theme::C::instr, this);
    sid_       = makeClickable("6581", Theme::C::highlight, this);
    sid_->setToolTip("SID1 chip model. Click to toggle 6581 ↔ 8580. Also: Shift+F8.");
    connect(sid_, &ClickableLabel::clicked, this, &StatusStrip::sidClicked);
    sid2_      = makeClickable("--", Theme::C::textDim, this);
    sid2_->setToolTip("SID2 chip model. Active only when stereo / dual-SID is on. "
                      "Click to toggle 6581 ↔ 8580 independently of SID1.");
    connect(sid2_, &ClickableLabel::clicked, this, &StatusStrip::sid2Clicked);
    ntsc_      = makeClickable("PAL", Theme::C::transpose, this);
    ntsc_->setToolTip("Click to toggle PAL 50Hz ↔ NTSC 60Hz timing.");
    connect(ntsc_, &ClickableLabel::clicked, this, &StatusStrip::ntscClicked);
    follow_    = makeClickable("Follow off", Theme::C::textDim, this);
    follow_->setToolTip("Click to toggle follow-play. Also: Ctrl+F.");
    connect(follow_, &ClickableLabel::clicked, this, &StatusStrip::followClicked);
    message_   = makeSegment("", Theme::C::textDim, this);

    auto addSep = [&]() {
        auto *sep = new QFrame(this);
        sep->setFrameShape(QFrame::VLine);
        sep->setFrameShadow(QFrame::Plain);
        QPalette sp = sep->palette();
        sp.setColor(QPalette::WindowText, Theme::C::sep);
        sep->setPalette(sp);
        layout->addWidget(sep);
    };

    layout->addWidget(transport_);
    addSep();
    layout->addWidget(position_);
    addSep();
    layout->addWidget(tempo_);
    addSep();
    layout->addWidget(octave_);
    addSep();
    layout->addWidget(record_);
    addSep();
    layout->addWidget(instr_);
    addSep();
    layout->addWidget(sid_);
    layout->addWidget(sid2_);
    addSep();
    layout->addWidget(ntsc_);
    addSep();
    layout->addWidget(follow_);
    addSep();
    layout->addWidget(message_, 1);
}

bool StatusStrip::eventFilter(QObject *o, QEvent *e) {
    if (o == octave_ && e->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent*>(e);
        int dy = we->angleDelta().y();
        if (dy != 0) {
            emit octaveDelta(dy > 0 ? +1 : -1);
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}

// Lightweight validation: scan current instrument + pattern cell for the
// well-known goattracker footguns documented in docs/in-app-help-spec.md.
static QString warningFor(int einumLocal) {
    INSTR &ins = instr[einumLocal];
    int A = (ins.ad >> 4) & 0xf;
    int R = ins.sr & 0xf;
    if (A == 0 && R == 1)
        return "warn: A=0 R=1 can ADSR-bug; try Attack=$F alt write order";
    int S = (ins.sr >> 4) & 0xf;
    if (S == 0 && R == 0 && A == 0 && ins.ad == 0)
        return "warn: all ADSR zero → silent click only";
    int gate = ins.gatetimer & 0x3f;
    if (gate == 0 && (ins.gatetimer & 0xC0) == 0)
        return "warn: gate timer 0 with HR enabled — undefined";
    return QString();
}

void StatusStrip::refresh() {
    bool playing = isplaying() != 0;
    transport_->setText(playing ? "▶ PLAYING" : "■ STOPPED");
    QPalette tp = transport_->palette();
    tp.setColor(QPalette::WindowText, playing ? Theme::C::vuGreen : Theme::C::textDim);
    transport_->setPalette(tp);

    int patnum = epnum[epchn];
    int currentRow = playing ? (chn[epchn].pattptr / 4) : eppos;
    position_->setText(QString("Row %1/%2  Patt %3  Order %4/%5")
        .arg(currentRow, 3, 16, QLatin1Char('0'))
        .arg(pattlen[patnum], 2, 16, QLatin1Char('0'))
        .arg(patnum, 2, 16, QLatin1Char('0'))
        .arg(espos[epchn], 2, 16, QLatin1Char('0'))
        .arg(songlen[esnum][epchn], 2, 16, QLatin1Char('0'))
        .toUpper());

    QString mlabel = (multiplier == 0) ? "Spd ½x" : QString("Spd %1x").arg(multiplier);
    tempo_->setText(mlabel);
    const char *kp = (keypreset == 1) ? "DMC"
                   : (keypreset == 2) ? "Janko"
                                      : "Protracker";
    octave_->setText(QString("Oct %1 · %2").arg(epoctave).arg(kp));
    record_->setText(recordmode ? "● REC" : "○ rec");
    QPalette rp = record_->palette();
    rp.setColor(QPalette::WindowText, recordmode ? Theme::C::vuRed : Theme::C::textDim);
    record_->setPalette(rp);
    instr_->setText(QString("Ins %1").arg(einum, 2, 16, QLatin1Char('0')).toUpper());
    sid_->setText(QString("SID1 %1").arg(sidmodel ? "8580" : "6581"));
    if (stereo_mode) {
        sid2_->setText(QString("SID2 %1").arg(sid2model ? "8580" : "6581"));
        QPalette p2 = sid2_->palette();
        p2.setColor(QPalette::WindowText, Theme::C::highlight);
        sid2_->setPalette(p2);
    } else {
        sid2_->setText("SID2 off");
        QPalette p2 = sid2_->palette();
        p2.setColor(QPalette::WindowText, Theme::C::textDim);
        sid2_->setPalette(p2);
    }
    ntsc_->setText(ntsc ? "NTSC 60Hz" : "PAL 50Hz");
    follow_->setText(followplay ? "Follow ON" : "Follow off");
    QPalette fp = follow_->palette();
    fp.setColor(QPalette::WindowText, followplay ? Theme::C::highlight : Theme::C::textDim);
    follow_->setPalette(fp);

    // Soft validation warning in the message slot (only when it stays put —
    // showMessage() with a timeout will clear and override us anyway).
    QString warn = warningFor(einum);
    if (!warn.isEmpty() && message_->text().isEmpty()) {
        QPalette mp = message_->palette();
        mp.setColor(QPalette::WindowText, Theme::C::vuOrange);
        message_->setPalette(mp);
        message_->setText(warn);
    } else if (warn.isEmpty()) {
        QPalette mp = message_->palette();
        mp.setColor(QPalette::WindowText, Theme::C::textDim);
        message_->setPalette(mp);
        if (message_->text().startsWith("warn:")) message_->setText("");
    }
}

void StatusStrip::showMessage(const QString &t, int ms) {
    message_->setText(t);
    if (ms > 0) {
        QTimer::singleShot(ms, message_, [this]{ message_->setText(""); });
    }
}
