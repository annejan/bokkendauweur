#pragma once
#include <QWidget>
extern "C" {
#include "gcommon.h"     // INSTR
}

class QSpinBox;
class QLineEdit;
class QComboBox;
class QLabel;
class AdsrPreview;
class WavetablePreview;

class InstrumentView : public QWidget {
    Q_OBJECT
public:
    explicit InstrumentView(QWidget *parent = nullptr);
    void refresh();

signals:
    void edited();

private slots:
    void onNameChanged(const QString &);
    void onAdChanged(int);
    void onSrChanged(int);
    void onWtChanged(int);
    void onPtChanged(int);
    void onFtChanged(int);
    void onVibParamChanged(int);
    void onVibDelayChanged(int);
    void onGateChanged(int);
    void onFirstWaveChanged(int);
    void onGotoWaveTable();
    void onGotoPulseTable();
    void onGotoFilterTable();
    void onTestNote();
    void onSilenceTestNote();
    void applyPreset(int index);
    void onApplyEdits();   // commit current state as the new revert baseline
    void onResetEdits();   // roll back to the last applied baseline
    void tickPlayback();   // 33 Hz pull of envelope level + wavetable pos

private:
    class QComboBox *presetBox_ = nullptr;
    QLineEdit *name_ = nullptr;
    QSpinBox *attack_, *decay_, *sustain_, *release_;
    QSpinBox *wave_, *pulse_, *filter_, *vibParam_;
    QSpinBox *vibDelay_, *gateTimer_, *firstWave_;
    class QTimer *autoTestTimer_ = nullptr;
    class QPushButton *autoBtn_ = nullptr;
    class QTimer *playbackTimer_ = nullptr; // 33 Hz monitor for live previews
    AdsrPreview *adsr_;
    WavetablePreview *wavePrev_;
    QLabel *summary_;
    bool updating_ = false;

    // Snapshot of instr[einum] at the moment it was last loaded or the
    // user last clicked Apply. Reset copies this back into instr[einum]
    // so the user can experiment freely on the live slot then roll back.
    // Switching to a different instrument auto-applies (saved_ is just
    // re-snapshotted from the new slot — past edits become the new
    // baseline implicitly).
    INSTR saved_{};
    int savedSlot_ = 0;
    int  *applyBtnDirtyPaint_ = nullptr;   // sentinel, unused; future hook
    class QPushButton *applyBtn_ = nullptr;
    class QPushButton *resetBtn_ = nullptr;
    bool dirty_ = false;
    void markDirty();

    void readFromGlobals();
    void updatePointerPreview(int t, int startRow);
    int  pointerTable_ = -1;
    QLabel *pointerPrev_ = nullptr;

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    // Only sample the live envelope / wavetable previews while this editor is
    // the visible page — no point polling sid_getlevels when the user is in
    // the pattern editor. (Stays running while visible so test notes / jam
    // still animate even when the song is stopped.)
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    void writeAd();
    void writeSr();
};
