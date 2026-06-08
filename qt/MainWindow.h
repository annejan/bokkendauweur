#pragma once
#include <QMainWindow>
#include <QByteArray>

class PatternView;
class OrderView;
class InstrumentView;
class TablesView;
class SongNameView;
class OrderMiniMap;
class InstrumentQuickList;
class StatusStrip;
class QStackedWidget;
class QWidget;
class QLabel;
class QTimer;
class QDockWidget;
class QUndoStack;
class CoreEvents;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void loadSongFile(const QString &path);
    void loadSidFile(const QString &path, const QStringList &extraOpts = {});
    void showAbout();

    // Editors call beginEdit() before mutating the song, then endEdit(label)
    // to push the resulting state onto the undo stack with the given label.
    QByteArray beginEdit();
    void endEdit(QByteArray before, const QString &label);

    // Exposed to the RPC layer so a test harness can drive time forward
    // deterministically instead of waiting on the 50 Hz QTimer.
    void tickOnce() { tick(); }
    // Stop / restart the UI tick explicitly. The RPC harness uses these to
    // freeze auto-ticking for deterministic stepping; addressing timer_
    // directly avoids findChild<QTimer*>() picking up a child view's timer.
    void pauseTimer();
    void resumeTimer();
    QWidget *activeEditorWidget() const;

private slots:
    void openSong();
    void mergeSong();
    void saveSong();
    void saveSongAs();
    void packAndRelocate();
    void loadInstrument();
    void saveInstrument();
    void playFromBeginning();
    void playFromPos();   // toggles: stop if playing, resume from position if stopped
    void playPattern();
    void stopSong();
    void muteCurrentChannel();
    void prevMultiplierSlot();
    void nextMultiplierSlot();
    void toggleSidModel();
    void toggleNtsc();
    void cycleMultiplier();
    void toggleFollowPlay();
    void cycleEditMode(bool backwards = false);
    void tick();

    // CoreEvents (audio-thread) notification handlers — replace per-frame
    // polling. Run on the GUI thread via queued connections.
    void onTransportChanged(bool playing); // relabel Pos/Pause + sync views
    void onPlayRowChanged();               // follow-play cursor / row highlight
    void onOrderPosChanged();              // order map follows the song pointer

private slots:
    void undo();
    void redo();

    // Microtonal / tuning settings (backport from v2.75)
    void setTuning12Tet();
    void setTuning19Tet();
    void setTuning24Tet();
    void setTuningCustomNTet();
    void loadScalaFile();
    void resetTuning();
    void setNoteNames12();
    void setNoteNamesSolfege();
    void setNoteNamesCustom();
    void setNoteNamesReset();
    void setKeyPresetTracker();
    void setKeyPresetDmc();
    void setKeyPresetJanko();
    void toggleStereoMode(bool on);
    void toggleSid2Model();
    void cycleSid2();   // status-strip click: off -> 6581 -> 8580 -> off

private:
    QUndoStack *undoStack_ = nullptr;
    QAction *playPosAction_ = nullptr;
    QAction *stereoAction_ = nullptr;   // Settings ▸ Audio ▸ Dual-SID toggle
    PatternView *pattern_ = nullptr;
    OrderView *order_ = nullptr;
    InstrumentView *instrument_ = nullptr;
    TablesView *tables_ = nullptr;
    SongNameView *songName_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    OrderMiniMap *orderMap_ = nullptr;
    InstrumentQuickList *insQuick_ = nullptr;
    QDockWidget *orderMapDock_ = nullptr;
    QDockWidget *insQuickDock_ = nullptr;
    StatusStrip *statusStrip_ = nullptr;
    class QToolButton *playBeginBtn_ = nullptr;
    class QToolButton *playPosBtn_   = nullptr;
    class QToolButton *playPattBtn_  = nullptr;
    class QToolButton *stopBtn_      = nullptr;
    QWidget     *patternBar_ = nullptr;       // toolbar above the stack —
    QLabel      *patternBarOct_ = nullptr;    // only visible on Pattern
    QLabel      *patternBarLen_ = nullptr;    // editor.
    QTimer *timer_ = nullptr;
    CoreEvents *coreEvents_ = nullptr;

    void buildUi();
    void refreshAll();
    void syncStack();

    void shrinkPattern();
    void growPattern();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
};
