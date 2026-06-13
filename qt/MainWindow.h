#pragma once
#include <QMainWindow>
#include <QByteArray>
#include <QStringList>
#include <QIcon>

class PatternView;
class OrderView;
class InstrumentView;
class TablesView;
class SongNameView;
class OrderMiniMap;
class InstrumentQuickList;
class StatusStrip;
class QTabBar;
class QWidget;
class QLabel;
class QTimer;
class QDockWidget;
class QUndoStack;
class QMenu;
class CoreEvents;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void loadSongFile(const QString &path);
    void loadSidFile(const QString &path, const QStringList &extraOpts = {});
    void showAbout();
    void showCheatSheet();

    // ChiptuneSAK is an OPTIONAL import dependency. If python3 + the
    // chiptunesak module are importable on the host, the Open Song
    // dialog offers SID / MIDI / etc; otherwise it only shows .sng.
    // Result is cached at first call so we don't fork python3 each
    // time the file dialog opens.
    bool chiptunesakAvailable();

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
    void newSong();
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
    // editmode follows keyboard focus across docked + floating editors.
    void onFocusChanged(QWidget *old, QWidget *now);
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
    QAction *stereoAction_ = nullptr;     // Settings ▸ SID config ▸ Dual SID
    QAction *singleSidAction_ = nullptr;  // Settings ▸ SID config ▸ Single SID
    PatternView *pattern_ = nullptr;
    OrderView *order_ = nullptr;
    InstrumentView *instrument_ = nullptr;
    TablesView *tables_ = nullptr;
    SongNameView *songName_ = nullptr;
    // Editor area: a nested QMainWindow whose five editor QDockWidgets can be
    // torn off into their own top-level windows (multi-monitor) and dragged
    // back to re-tab. editorDock_ is indexed by EDIT_* (0..4).
    QMainWindow *editorArea_ = nullptr;
    QDockWidget *editorDock_[5] = {};
    QIcon editorIcon_[5];           // pictograms, also pushed onto the dock tabs
    // Qt's tabified-dock QTabBar ignores QDockWidget::windowIcon(), so we set
    // the pictograms on the internal tab bar(s) directly — and reapply after
    // any float / re-dock rebuilds it.
    void applyDockTabIcons();

    // Browser-style tab tear-off: dragging a dock tab off the strip floats it
    // into its own window. Qt's native tab drag only reorders, so we watch the
    // dock-area QTabBar(s) and call setFloating() once the cursor leaves it.
    QTabBar *tearBar_ = nullptr;    // tab bar the current drag started on
    int      tearEditorIdx_ = -1;   // EDIT_* index of the dragged tab (-1 = none)
    bool     tearArmed_ = false;
    OrderMiniMap *orderMap_ = nullptr;
    InstrumentQuickList *insQuick_ = nullptr;
    QDockWidget *orderMapDock_ = nullptr;
    QDockWidget *insQuickDock_ = nullptr;
    StatusStrip *statusStrip_ = nullptr;
    class QToolButton *playBeginBtn_ = nullptr;
    class QToolButton *playPosBtn_   = nullptr;
    class QToolButton *playPattBtn_  = nullptr;
    class QToolButton *stopBtn_      = nullptr;
    // True when the user toggled the Pos button to pause an in-progress
    // playback (PLAY_POS or PLAY_PATTERN). Cleared by any explicit Begin /
    // Patt / Stop click, or by the next Pos-resume. Used to drive the
    // Pos / Pause button glow while paused.
    bool pausedAtPos_ = false;
    // Tri-state cache for chiptunesakAvailable(): -1 = not probed yet,
    // 0 = absent, 1 = importable.
    int  chiptunesakCached_ = -1;
    // Per-channel engine position captured at the moment the user paused
    // via the Pos toggle. Used to restore play state on the next Pos
    // resume so we don't end up jumping back to the song start.
    int  pausedSongptr_[6] = {0,0,0,0,0,0};   // MAX_CHN
    int  pausedPattRow_ = 0;
    QWidget     *patternBar_ = nullptr;       // toolbar above the stack —
    QLabel      *patternBarOct_ = nullptr;    // only visible on Pattern
    QLabel      *patternBarLen_ = nullptr;    // editor.
    QTimer *timer_ = nullptr;
    CoreEvents *coreEvents_ = nullptr;

    void buildUi();
    void refreshAll();
    void syncStack();
    // Maps an EDIT_* index to its editor view widget (pattern_, order_, …).
    QWidget *editorView(int idx) const;

    // "Open Recent" submenu — last 10 user-opened .sng files, persisted in
    // QSettings("recentFiles") so the list survives restarts.
    QMenu *recentMenu_ = nullptr;
    QStringList recentFiles_;
    void addRecentFile(const QString &path);
    void updateRecentMenu();
    void loadRecentFiles();
    void saveRecentFiles();

    void shrinkPattern();
    void growPattern();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void showEvent(QShowEvent *e) override;
};
