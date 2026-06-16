#include "MainWindow.h"
#include "CheatSheet.h"
#include <QTextBrowser>
#include <QDialog>
#include <QPointer>
#include <QPushButton>
#include "PatternView.h"
#include "OrderView.h"
#include "InstrumentView.h"
#include "TablesView.h"
#include "SongNameView.h"
#include "OrderMiniMap.h"
#include "InstrumentQuickList.h"
#include "StatusStrip.h"
#include "UndoStack.h"
#include "CoreEvents.h"
#include "Speech.h"
#include <QUndoStack>

#include <QDockWidget>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QHash>
#include <QAbstractButton>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QMenuBar>
#include <QToolBar>
#include <QAction>
#include <QShortcut>
#include <QKeySequence>
#include <QTabBar>
#include <QTabWidget>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QLabel>
#include <QTimer>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QCloseEvent>
#include <QProcess>
#include <QMessageBox>
#include <QCoreApplication>
#include <QFile>
#include <QStatusBar>
#include <QWidget>
#include <QVBoxLayout>
#include <QToolButton>
#include <QFontMetrics>
#include <QFrame>
#include <QMouseEvent>
#include <QShowEvent>
#include <QLabel>
#include <QStyle>
#include <QInputDialog>
#include <QActionGroup>
#include "Theme.h"
#include "PaAudio.h"
#include "InstrColors.h"
#include <cstring>

extern "C" {
#include "goattrk2.h"
#include "gcommon.h"
#include "gfile.h"
#include "gsong.h"
#include "gplay.h"
#include "gorder.h"

extern char songfilename[];
extern char songpath[];
extern int recordmode;
extern int autoadvance;
extern char instrfilename[];
extern char instrpath[];
extern char songname[MAX_STR];
extern char authorname[MAX_STR];
extern char copyrightname[MAX_STR];
extern int eppos, epcolumn, epchn, eschn;
extern int espos[MAX_CHN];
extern int esnum;
extern unsigned char songorder[MAX_SONGS][MAX_CHN][MAX_SONGLEN+2];
extern int songlen[MAX_SONGS][MAX_CHN];
extern int editmode;
extern int followplay;
extern int einum;
extern int epchn;
extern CHN chn[];
extern int songinit;
extern int epoctave;
extern unsigned char pattern[MAX_PATT][MAX_PATTROWS*4+4];
extern int pattlen[MAX_PATT];
extern int epnum[MAX_CHN];
void countpatternlengths(void);
extern unsigned sidmodel;
extern unsigned sid2model;
extern int stereo_mode;
extern int song_channels;
extern unsigned multiplier;
extern unsigned keypreset;
extern unsigned b, mr, writer, hardsid, ntsc, catweasel, interpolate, customclockrate;
// Microtonal globals (mirror src/goattrk2.c definitions).
extern float basepitch;
extern float equaldivisionsperoctave;
extern int tuningcount;
extern double tuning[96];
extern char specialnotenames[186];
extern char scalatuningfilepath[];
extern char tuningname[64];
int sound_init(unsigned b, unsigned mr, unsigned writer, unsigned hardsid,
               unsigned m, unsigned ntsc, unsigned multiplier,
               unsigned catweasel, unsigned interpolate, unsigned customclockrate);
void sid_init(int speed, unsigned m, unsigned ntsc, unsigned interpolate,
              unsigned customclockrate, unsigned usefp);
int savesong(void);
int saveinstrument(void);
void loadinstrument(void);
void prevmultiplier(void);
void nextmultiplier(void);
void calculatefreqtable(void);
void setspecialnotenames(void);
void readscalatuningfile(void);
void resetnotenames(void);
}

namespace {
constexpr int MAX_RECENT   = 10;              // Open Recent list cap
constexpr int EDITOR_COUNT = EDIT_NAMES + 1;  // panes EDIT_PATTERN..EDIT_NAMES
// Single source of truth for the editor tab labels + tooltips. The dock tab
// text is derived from these, and applyDockTabIcons() / the tear-off handler
// key off them, so they must stay indexed by EDIT_*.
const char *const EDITOR_TITLE[EDITOR_COUNT] =
    { "Pattern", "Order", "Instrument", "Tables", "Songname" };
const char *const EDITOR_TIP[EDITOR_COUNT] = {
    "Pattern editor (F5)", "Order / song editor (F6)", "Instrument editor (F7)",
    "Tables editor (F8)", "Songname editor" };
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    undoStack_ = new QUndoStack(this);
    undoStack_->setUndoLimit(64);

    // Notification bridge: the audio thread emits transport / row / order-pos
    // edges; queued connections deliver them on the GUI thread, so the views
    // no longer poll chn[]/isplaying() every frame. Created BEFORE buildUi() so
    // child views (OrderView, InstrumentQuickList, …) can connect to
    // CoreEvents::instance() from their own constructors.
    coreEvents_ = new CoreEvents(this);

    buildUi();
    QSettings s("goattracker2-qt", "goattracker2-qt");
    QByteArray sp = s.value("songpath").toString().toLocal8Bit();
    QByteArray ip = s.value("instrpath").toString().toLocal8Bit();
    if (!sp.isEmpty()) std::strncpy(songpath, sp.constData(), MAX_PATHNAME - 1);
    if (!ip.isEmpty()) std::strncpy(instrpath, ip.constData(), MAX_PATHNAME - 1);
    // Restore the editor dock layout (tab grouping + any torn-off / floated
    // editors) and the main window geometry from the previous session. Uses
    // the default app QSettings (org/app = "goattrk2-qt", set in main()) to
    // sit alongside the editor/* prefs and the recent-files list.
    QSettings cfg;
    QByteArray editorLayout = cfg.value("editorLayout").toByteArray();
    if (editorArea_ && !editorLayout.isEmpty())
        editorArea_->restoreState(editorLayout);
    QByteArray mainGeo = cfg.value("mainGeometry").toByteArray();
    if (!mainGeo.isEmpty())
        restoreGeometry(mainGeo);
    connect(coreEvents_, &CoreEvents::transportChanged,
            this, &MainWindow::onTransportChanged);
    connect(coreEvents_, &CoreEvents::rowChanged,
            this, &MainWindow::onPlayRowChanged);
    connect(coreEvents_, &CoreEvents::orderPosChanged,
            this, &MainWindow::onOrderPosChanged);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::tick);
    // 25 Hz UI tick (40 ms). Halved from the previous 50 Hz to leave more
    // room for the audio thread + the playroutine; the user explicitly
    // accepts visual drop-frames over note timing slips. Pattern follow-play
    // and VU strip still update fast enough to read at a glance.
    timer_->start(40);
}

MainWindow::~MainWindow() {
    QSettings s("goattracker2-qt", "goattracker2-qt");
    s.setValue("songpath",  QString::fromLocal8Bit(songpath));
    s.setValue("instrpath", QString::fromLocal8Bit(instrpath));
    // Persist the editor dock layout + window geometry so torn-off editors
    // and tab arrangement survive a restart. Default store, matching the ctor.
    QSettings cfg;
    if (editorArea_) cfg.setValue("editorLayout", editorArea_->saveState());
    cfg.setValue("mainGeometry", saveGeometry());
}

void MainWindow::pauseTimer()  { if (timer_) timer_->stop(); }
void MainWindow::resumeTimer() { if (timer_) timer_->start(40); }

void MainWindow::buildUi() {
    setWindowTitle("GoatTracker Qt");
    resize(1280, 800);

    // Central stacked editor with custom bottom status strip
    auto *centralWrap = new QWidget(this);
    auto *centralLay = new QVBoxLayout(centralWrap);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);

    // ---- Pattern editor toolbar ------------------------------------------
    // Sits above the stack, only visible when the pattern editor is the
    // active tab. Holds the recording-octave + pattern-length controls so
    // the user has proper Qt buttons (with hover, focus, keyboard nav)
    // instead of a painted pill on the grid canvas.
    patternBar_ = new QWidget(centralWrap);
    auto *pbLay = new QHBoxLayout(patternBar_);
    pbLay->setContentsMargins(10, 4, 10, 4);
    pbLay->setSpacing(6);

    auto makeStep = [&](const QString &label, const QString &tip) {
        auto *b = new QToolButton(patternBar_);
        b->setText(label);
        b->setToolTip(tip);
        b->setAutoRaise(false);
        b->setMinimumWidth(28);
        return b;
    };

    pbLay->addWidget(new QLabel("Octave", patternBar_));
    auto *octDown = makeStep("−", "Lower recording octave by 1 (key: /)");
    octDown->setAccessibleName("Lower octave");
    auto *octShow = new QLabel("0", patternBar_);
    octShow->setMinimumWidth(22);
    octShow->setAlignment(Qt::AlignCenter);
    QFont obf = octShow->font(); obf.setBold(true);
    octShow->setFont(obf);
    auto *octUp   = makeStep("+", "Raise recording octave by 1 (key: *). "
                                  "Right-click = lower by 1.");
    octUp->setAccessibleName("Raise octave");
    octUp->setContextMenuPolicy(Qt::PreventContextMenu);
    pbLay->addWidget(octDown);
    pbLay->addWidget(octShow);
    pbLay->addWidget(octUp);
    patternBarOct_ = octShow;

    connect(octDown, &QToolButton::clicked, this, [this]() {
        if (epoctave > 0) { epoctave--; refreshAll(); }
    });
    connect(octUp, &QToolButton::clicked, this, [this]() {
        if (epoctave < 7) { epoctave++; refreshAll(); }
    });
    // Right-click on + lowers — matches the user's "left=up, right=down"
    // request without losing the explicit '−' button.
    octUp->installEventFilter(this);

    auto *sep1 = new QFrame(patternBar_);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFrameShadow(QFrame::Sunken);
    pbLay->addSpacing(8);
    pbLay->addWidget(sep1);
    pbLay->addSpacing(8);

    pbLay->addWidget(new QLabel("Pattern length", patternBar_));
    auto *lenDown = makeStep("−", "Shrink active pattern by 1 row "
                                  "(pulls ENDPATT back one row).");
    lenDown->setAccessibleName("Shrink pattern");
    auto *lenShow = new QLabel("00", patternBar_);
    lenShow->setMinimumWidth(28);
    lenShow->setAlignment(Qt::AlignCenter);
    QFont lbf = lenShow->font(); lbf.setBold(true);
    lenShow->setFont(lbf);
    auto *lenUp   = makeStep("+", "Grow active pattern by 1 row "
                                  "(REST + ENDPATT).");
    lenUp->setAccessibleName("Grow pattern");
    pbLay->addWidget(lenDown);
    pbLay->addWidget(lenShow);
    pbLay->addWidget(lenUp);
    patternBarLen_ = lenShow;

    connect(lenDown, &QToolButton::clicked, this, &MainWindow::shrinkPattern);
    connect(lenUp,   &QToolButton::clicked, this, &MainWindow::growPattern);

    auto *sep2 = new QFrame(patternBar_);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFrameShadow(QFrame::Sunken);
    pbLay->addSpacing(8);
    pbLay->addWidget(sep2);
    pbLay->addSpacing(8);

    // Read-only tempo indicator: current ticks-per-row of the active channel
    // (set by the F command). No +/- — tempo is driven by pattern F commands,
    // not edited here.
    pbLay->addWidget(new QLabel("Tempo", patternBar_));
    auto *tempoShow = new QLabel("6", patternBar_);
    tempoShow->setMinimumWidth(32);
    tempoShow->setAlignment(Qt::AlignCenter);
    QFont tbf = tempoShow->font(); tbf.setBold(true);
    tempoShow->setFont(tbf);
    tempoShow->setToolTip("Active channel's tempo in ticks per row (set by the "
                          "F command). 'funk' = funktempo (alternating values).");
    tempoShow->setAccessibleName("Tempo");
    pbLay->addWidget(tempoShow);
    patternBarTempo_ = tempoShow;

    pbLay->addStretch(1);
    // ---- Editor area: detachable dock tabs -------------------------------
    // The five editors live inside a nested QMainWindow as tabified
    // QDockWidgets. Qt gives browser-style tear-off for free: drag a tab out
    // and it floats as its own top-level window (great for a second monitor);
    // drag it back onto the tab strip to re-tab. editmode is no longer tied
    // to one visible page — it FOLLOWS KEYBOARD FOCUS (onFocusChanged), so
    // whichever editor you click / focus becomes the one the engine edits.
    // The Mode menu / F5-F8 / Tab cycle still drive editmode via syncStack(),
    // which now raises (and, if floating, activates) the target dock.
    editorArea_ = new QMainWindow;
    editorArea_->setWindowFlags(Qt::Widget);   // behave as a plain child widget
    editorArea_->setDockNestingEnabled(true);
    editorArea_->setDockOptions(QMainWindow::AnimatedDocks
                                | QMainWindow::AllowTabbedDocks
                                | QMainWindow::AllowNestedDocks);

    pattern_    = new PatternView(editorArea_);
    order_      = new OrderView(editorArea_);
    instrument_ = new InstrumentView(editorArea_);
    tables_     = new TablesView(editorArea_);
    songName_   = new SongNameView(editorArea_);

    // Pattern pane bundles its octave / length toolbar so the bar travels
    // with the pattern editor when the dock is torn off into its own window.
    auto *patternPane = new QWidget(editorArea_);
    auto *ppLay = new QVBoxLayout(patternPane);
    ppLay->setContentsMargins(0, 0, 0, 0);
    ppLay->setSpacing(0);
    ppLay->addWidget(patternBar_);
    ppLay->addWidget(pattern_, 1);

    // Hand-painted pictograms (same set as before) — shown on each dock tab
    // via the dock's window icon. Painted at 32px, scaled down for hi-dpi.
    auto makeIcon = [](auto draw) -> QIcon {
        const int S = 32;
        QPixmap pm(S, S);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        draw(p, S);
        p.end();
        return QIcon(pm);
    };
    const QColor ink = Theme::C::text;
    QIcon (&icons)[5] = editorIcon_;
    // Pattern / track editor — three vertical lines (note columns).
    icons[EDIT_PATTERN] = makeIcon([&](QPainter &p, int S) {
        p.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap));
        for (int x : {8, 16, 24}) p.drawLine(x, 6, x, S - 6);
    });
    // Order / song editor — three horizontal lines (sequence rows).
    icons[EDIT_ORDERLIST] = makeIcon([&](QPainter &p, int S) {
        p.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap));
        for (int y : {8, 16, 24}) p.drawLine(6, y, S - 6, y);
    });
    // Instrument editor — a few black-and-white piano keys.
    icons[EDIT_INSTRUMENT] = makeIcon([&](QPainter &p, int S) {
        const int x0 = 5, y0 = 7, w = S - 10, h = S - 14;
        p.setPen(QPen(ink, 1));
        p.setBrush(Qt::white);
        p.drawRect(x0, y0, w, h);
        for (int i = 1; i < 4; ++i) {
            int x = x0 + i * w / 4;
            p.drawLine(x, y0, x, y0 + h);
        }
        p.setBrush(Qt::black);
        p.setPen(Qt::NoPen);
        const int bw = w / 8, bh = h * 3 / 5;
        for (int i : {1, 2, 3}) {
            int x = x0 + i * w / 4 - bw / 2;
            p.drawRect(x, y0, bw, bh);
        }
    });
    // Tables editor — a simple grid/table (2x3 cells).
    icons[EDIT_TABLES] = makeIcon([&](QPainter &p, int S) {
        const int x0 = 5, y0 = 6, w = S - 10, h = S - 12;
        p.setPen(QPen(ink, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(x0, y0, w, h);
        p.setPen(QPen(ink, 1.5));
        p.drawLine(x0 + w / 3,     y0, x0 + w / 3,     y0 + h);
        p.drawLine(x0 + 2 * w / 3, y0, x0 + 2 * w / 3, y0 + h);
        p.drawLine(x0, y0 + h / 2, x0 + w, y0 + h / 2);
    });
    // Songname editor — a label / luggage tag with a punch hole.
    icons[EDIT_NAMES] = makeIcon([&](QPainter &p, int S) {
        p.setPen(QPen(ink, 2, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPolygon tag;
        tag << QPoint(13, 6) << QPoint(S - 6, 6) << QPoint(S - 6, S - 6)
            << QPoint(13, S - 6) << QPoint(5, S / 2);
        p.drawPolygon(tag);
        p.setBrush(ink);
        p.drawEllipse(QPoint(12, S / 2), 2, 2);
    });

    QWidget *panes[EDITOR_COUNT] =
        { patternPane, order_, instrument_, tables_, songName_ };
    for (int i = 0; i < EDITOR_COUNT; ++i) {
        auto *dock = new QDockWidget(EDITOR_TITLE[i], editorArea_);
        // Stable objectName so QMainWindow::saveState / restoreState can
        // round-trip the dock layout (incl. which editors were floated) on
        // the next launch.
        dock->setObjectName(QStringLiteral("editorDock%1").arg(i));
        dock->setWidget(panes[i]);
        dock->setWindowIcon(icons[i]);
        dock->setToolTip(EDITOR_TIP[i]);
        // No close box — an editor must always exist — but free to float /
        // move so the user can pull it onto a second monitor.
        dock->setFeatures(QDockWidget::DockWidgetMovable
                          | QDockWidget::DockWidgetFloatable);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        editorArea_->addDockWidget(Qt::TopDockWidgetArea, dock);
        if (i > 0) editorArea_->tabifyDockWidget(editorDock_[0], dock);
        editorDock_[i] = dock;
        dock->installEventFilter(this);   // intercept the float close button
        // When torn off, promote the dock to a real top-level window so it
        // gets native decorations (title bar, min/max, resize border). The
        // WM then owns the title bar — so drag-back can't redock; the native
        // close button does instead (see eventFilter). Re-docking also
        // rebuilds the tab bar, dropping our icons, so reapply afterwards.
        connect(dock, &QDockWidget::topLevelChanged, this, [this, dock](bool floating) {
            if (floating) {
                // Defer the flag change: topLevelChanged can fire mid-
                // restoreState() (re-entrant dock machinery), and mutating
                // window flags + show() inline there can crash. Run it on the
                // next event-loop pass, once the dock manager has settled.
                QTimer::singleShot(0, dock, [dock]{
                    if (!dock->isFloating()) return;   // redocked meanwhile
                    dock->setWindowFlags(Qt::Window
                                         | Qt::WindowTitleHint
                                         | Qt::WindowSystemMenuHint
                                         | Qt::WindowMinMaxButtonsHint
                                         | Qt::WindowCloseButtonHint);
                    dock->show();
                });
            }
            QTimer::singleShot(0, this, [this]{ applyDockTabIcons(); });
        });
        // restoreState() and any manual re-docking rebuild the tab bar too —
        // reapply icons + the tear-off filter once the new bar settles.
        connect(dock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) {
            QTimer::singleShot(0, this, [this]{ applyDockTabIcons(); });
        });
    }
    editorArea_->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    editorDock_[EDIT_PATTERN]->raise();   // pattern editor up front initially
    // The tab bar isn't materialised until the dock area lays out — set the
    // pictograms on the next event-loop pass once it exists.
    QTimer::singleShot(0, this, [this]{ applyDockTabIcons(); });

    centralLay->addWidget(editorArea_, 1);

    // editmode follows focus across docked + floating editors.
    connect(qApp, &QApplication::focusChanged,
            this, &MainWindow::onFocusChanged);

    statusStrip_ = new StatusStrip(centralWrap);
    centralLay->addWidget(statusStrip_);
    connect(statusStrip_, &StatusStrip::sidClicked, this, &MainWindow::toggleSidModel);
    connect(statusStrip_, &StatusStrip::sid2Clicked, this, &MainWindow::cycleSid2);
    connect(statusStrip_, &StatusStrip::followClicked, this, &MainWindow::toggleFollowPlay);
    connect(statusStrip_, &StatusStrip::ntscClicked, this, &MainWindow::toggleNtsc);
    connect(statusStrip_, &StatusStrip::tempoClicked, this, &MainWindow::cycleMultiplier);
    connect(statusStrip_, &StatusStrip::octaveClicked, this, [this]() {
        epoctave = (epoctave + 1) & 7;
        statusStrip_->showMessage(QString("Octave %1").arg(epoctave));
        refreshAll();
    });
    connect(statusStrip_, &StatusStrip::octaveDelta, this, [this](int d) {
        int n = epoctave + d;
        if (n < 0) n = 0;
        if (n > 7) n = 7;
        epoctave = n;
        statusStrip_->showMessage(QString("Octave %1").arg(epoctave));
        refreshAll();
    });
    connect(statusStrip_, &StatusStrip::recordClicked, this, [this]() {
        recordmode ^= 1;
        statusStrip_->showMessage(recordmode
                                  ? "Record mode ON"
                                  : "Record mode OFF (audition)");
        refreshAll();
    });
    connect(statusStrip_, &StatusStrip::skipClicked, this, [this]() {
        autoadvance = (autoadvance + 1) % 3;
        const char *m = (autoadvance == 0) ? "EDIT SKIP 0 (no advance)"
                      : (autoadvance == 1) ? "EDIT SKIP 1 (advance after note)"
                                           : "EDIT SKIP 2 (advance after every column)";
        statusStrip_->showMessage(m);
        refreshAll();
    });

    setCentralWidget(centralWrap);

    connect(pattern_, &PatternView::patternEdited, this, &MainWindow::refreshAll);
    connect(order_, &OrderView::edited, this, &MainWindow::refreshAll);
    connect(instrument_, &InstrumentView::edited, this, [this]() {
        // The InstrumentView '→ table' jump buttons set editmode = 3
        // (EDIT_TABLES) and emit edited(); without syncStack the editmode
        // change never raises the Tables dock and the user just sees a
        // refresh of the instrument editor.
        //
        // syncStack() force-focuses the target editor, which would steal
        // focus off the active instrument-name QLineEdit on every keystroke.
        // Only resync when the target editor (editmode) isn't already the
        // visible one — i.e. a '→ table' jump that actually switched editors.
        if (QWidget *v = editorView(editmode); v && !v->isVisible()) {
            syncStack();
        }
        refreshAll();
    });
    connect(tables_, &TablesView::edited, this, &MainWindow::refreshAll);
    connect(songName_, &SongNameView::edited, this, &MainWindow::refreshAll);

    // Dock widgets
    orderMapDock_ = new QDockWidget("Order map", this);
    // Wrap: [toggle] + [mini-map]. Toggle picks whether a plain click on
    // a row moves all channels (legacy 'song' navigation) or just the
    // clicked channel (per-track). Ctrl-click always inverts the current
    // mode so the other one stays reachable from the keyboard.
    auto *omWrap = new QWidget(orderMapDock_);
    auto *omLay = new QVBoxLayout(omWrap);
    omLay->setContentsMargins(4, 4, 4, 4);
    omLay->setSpacing(2);
    auto *omToggle = new QToolButton(omWrap);
    omToggle->setCheckable(true);
    omToggle->setChecked(true);
    omToggle->setText("All channels");
    omToggle->setToolTip("Click switches between 'move all channels' (default) "
                         "and 'move only the clicked channel'. Ctrl-click on a "
                         "row inverts the mode for that one click.");
    omToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    omToggle->setAccessibleDescription(
        "Order map navigation mode: move all channels together, or only the clicked channel.");
    omLay->addWidget(omToggle);
    orderMap_ = new OrderMiniMap(omWrap);
    orderMap_->setAccessibleName("Order map");
    orderMap_->setAccessibleDescription(
        "Vertical overview of all orderlist entries with the playback marker. "
        "Click to move the cursor; Ctrl+click moves only the clicked channel.");
    omLay->addWidget(orderMap_, 1);
    orderMapDock_->setWidget(omWrap);
    // Detachable into its own window: drag the dock title bar out to float it
    // (Qt keeps the title bar, so it resizes and can be dragged back onto an
    // edge to re-dock); the [X] still hides it.
    orderMapDock_->setObjectName(QStringLiteral("orderMapDock"));
    orderMapDock_->setFeatures(QDockWidget::DockWidgetClosable
                              | QDockWidget::DockWidgetMovable
                              | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, orderMapDock_);
    connect(omToggle, &QToolButton::toggled, this, [this, omToggle](bool on) {
        orderMap_->setSelectAllChannels(on);
        omToggle->setText(on ? "All channels" : "One channel");
    });
    connect(orderMap_, &OrderMiniMap::positionChanged, this, &MainWindow::refreshAll);
    // Restore the historical 160 px sizing the user expects on first launch —
    // setMinimumWidth was dropped so the dock could collapse, but Qt picks
    // a tiny initial width without a hint, leaving the map unreadable.
    resizeDocks({orderMapDock_}, {160}, Qt::Horizontal);

    insQuickDock_ = new QDockWidget("Instruments", this);
    // Wrap: [colour master button] + [instrument list]. Double-clicking a
    // row in the list toggles that one instrument's colour; the button is
    // an 'all on' / 'all off' master so the user can flood-fill or wipe in
    // one click.
    {
        auto *iqWrap = new QWidget(insQuickDock_);
        auto *iqLay = new QVBoxLayout(iqWrap);
        iqLay->setContentsMargins(4, 4, 4, 4);
        iqLay->setSpacing(2);
        auto *colAllBtn = new QToolButton(iqWrap);
        colAllBtn->setText("Colours: all on");
        colAllBtn->setToolTip(
            "Toggle the colour bit on every instrument. Double-click a "
            "single row in the list below to flip just that one.");
        colAllBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        colAllBtn->setAccessibleDescription(
            "Toggle the colour bit on every instrument. Double-click a row below to flip just one.");
        iqLay->addWidget(colAllBtn);
        insQuick_ = new InstrumentQuickList(iqWrap);
        iqLay->addWidget(insQuick_, 1);
        insQuickDock_->setWidget(iqWrap);

        // The button label tracks the next action: 'all on' when the mask
        // is empty, 'all off' otherwise (so a click clears any non-empty
        // mask first).
        auto refreshColBtn = [colAllBtn]() {
            // Probe bit 1 + bit 2 — if any non-zero slot is coloured the
            // button offers 'all off'; otherwise 'all on'.
            bool any = false;
            for (int i = 1; i < 64 && !any; i++)
                if (isInstrColored((unsigned char)i)) any = true;
            colAllBtn->setText(any ? "Colours: all off" : "Colours: all on");
        };
        loadInstrColorMask();
        refreshColBtn();
        connect(colAllBtn, &QToolButton::clicked, this, [this, refreshColBtn]() {
            bool any = false;
            for (int i = 1; i < 64 && !any; i++)
                if (isInstrColored((unsigned char)i)) any = true;
            insQuick_->setAllColored(!any);
            refreshColBtn();
            pattern_->refresh();
        });
        connect(insQuick_, &InstrumentQuickList::colorMaskChanged, this,
                [this, refreshColBtn]() {
                    refreshColBtn();
                    pattern_->refresh();
                });
    }
    // Detachable into its own window — same as the Order map dock above.
    insQuickDock_->setObjectName(QStringLiteral("instrumentsDock"));
    insQuickDock_->setFeatures(QDockWidget::DockWidgetClosable
                              | QDockWidget::DockWidgetMovable
                              | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, insQuickDock_);
    connect(insQuick_, &InstrumentQuickList::instrumentChosen, this, &MainWindow::refreshAll);

    // ---- Menus -----------------------------------------------------------
    auto *fileMenu = menuBar()->addMenu("&File");
    auto *newA = fileMenu->addAction("&New");
    newA->setShortcut(Qt::CTRL | Qt::Key_N);
    newA->setToolTip("Discard the current song and start a fresh empty project "
                     "(64-row patterns, REST endmarks, default tempo).");
    connect(newA, &QAction::triggered, this, &MainWindow::newSong);
    auto *openA = fileMenu->addAction("&Open .sng…");
    openA->setShortcut(Qt::CTRL | Qt::Key_O);
    connect(openA, &QAction::triggered, this, &MainWindow::openSong);
    recentMenu_ = fileMenu->addMenu("Open &Recent");
    loadRecentFiles();
    updateRecentMenu();
    auto *mergeA = fileMenu->addAction("&Merge .sng…");
    mergeA->setShortcut(Qt::CTRL | Qt::Key_M);
    connect(mergeA, &QAction::triggered, this, &MainWindow::mergeSong);
    auto *saveA = fileMenu->addAction("&Save");
    saveA->setShortcut(Qt::CTRL | Qt::Key_S);
    connect(saveA, &QAction::triggered, this, &MainWindow::saveSong);
    auto *saveAsA = fileMenu->addAction("Save &As…");
    saveAsA->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_S);
    connect(saveAsA, &QAction::triggered, this, &MainWindow::saveSongAs);
    auto *packA = fileMenu->addAction("&Pack to PRG / SID / BIN…");
    packA->setShortcut(Qt::Key_F9);
    packA->setToolTip("Run gt2reloc to produce a C64-loadable PRG, PSID file, or raw BIN");
    connect(packA, &QAction::triggered, this, &MainWindow::packAndRelocate);
    fileMenu->addSeparator();
    auto *loadInsA = fileMenu->addAction("Load &Instrument…");
    connect(loadInsA, &QAction::triggered, this, &MainWindow::loadInstrument);
    auto *saveInsA = fileMenu->addAction("Save I&nstrument…");
    connect(saveInsA, &QAction::triggered, this, &MainWindow::saveInstrument);
    fileMenu->addSeparator();
    auto *quitA = fileMenu->addAction("&Quit");
    quitA->setShortcut(Qt::CTRL | Qt::Key_Q);
    connect(quitA, &QAction::triggered, this, &QMainWindow::close);

    // Edit menu — undo / redo
    auto *editMenu = menuBar()->addMenu("&Edit");
    auto *undoA = editMenu->addAction("&Undo");
    undoA->setShortcut(QKeySequence::Undo);
    connect(undoA, &QAction::triggered, this, &MainWindow::undo);
    auto *redoA = editMenu->addAction("&Redo");
    redoA->setShortcut(QKeySequence::Redo);
    connect(redoA, &QAction::triggered, this, &MainWindow::redo);

    auto *modeMenu = menuBar()->addMenu("&Mode");
    auto addMode = [&](const QString &label, int mode, Qt::Key shortcut) {
        auto *a = modeMenu->addAction(label);
        a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, [this, mode]{
            editmode = mode; syncStack(); refreshAll();
        });
    };
    addMode("&Pattern editor",   EDIT_PATTERN,    Qt::Key_F5);
    addMode("&Order/song editor", EDIT_ORDERLIST, Qt::Key_F6);
    addMode("&Instrument editor", EDIT_INSTRUMENT, Qt::Key_F7);
    addMode("&Tables editor",     EDIT_TABLES,    Qt::Key_F8);
    auto *namesA = modeMenu->addAction("Song&name editor");
    connect(namesA, &QAction::triggered, this, [this]{
        editmode = EDIT_NAMES; syncStack(); refreshAll();
    });

    auto *tabA = new QAction(this);
    tabA->setShortcut(Qt::Key_Tab);
    tabA->setShortcutContext(Qt::ApplicationShortcut);
    connect(tabA, &QAction::triggered, this, [this]{ cycleEditMode(false); });
    addAction(tabA);
    auto *backTabA = new QAction(this);
    backTabA->setShortcut(Qt::SHIFT | Qt::Key_Tab);
    backTabA->setShortcutContext(Qt::ApplicationShortcut);
    connect(backTabA, &QAction::triggered, this, [this]{ cycleEditMode(true); });
    addAction(backTabA);

    auto *viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction(orderMapDock_->toggleViewAction());
    viewMenu->addAction(insQuickDock_->toggleViewAction());
    auto *followA = viewMenu->addAction("Toggle &follow-play");
    followA->setShortcut(Qt::CTRL | Qt::Key_F);
    followA->setCheckable(true);
    connect(followA, &QAction::triggered, this, &MainWindow::toggleFollowPlay);

    // Accessibility: self-voicing of the pattern-editor cursor. Speaks the
    // note / instrument / command under the cursor as you move and edit.
    auto *speakA = viewMenu->addAction("&Speak cursor (accessibility)");
    speakA->setCheckable(true);
    speakA->setToolTip(
        "Self-voicing: speak the note / instrument / command under the "
        "pattern-editor cursor as you move and edit, via the system "
        "text-to-speech engine.");
    if (!Speech::instance().compiledIn()) {
        speakA->setEnabled(false);
        speakA->setToolTip(speakA->toolTip() +
            "  (this build was compiled without the Qt TextToSpeech module)");
    } else {
        QSettings s;
        bool on = s.value("editor/speakCursor", false).toBool();
        speakA->setChecked(on);
        Speech::instance().setEnabled(on);
    }
    connect(speakA, &QAction::toggled, this, [](bool on) {
        Speech::instance().setEnabled(on);
        QSettings s; s.setValue("editor/speakCursor", on);
        if (on) Speech::instance().say("Speech enabled", Speech::Priority::Status);
    });

    auto *blinkA = viewMenu->addAction("&Blink active instruments");
    blinkA->setCheckable(true);
    blinkA->setChecked(false);
    connect(blinkA, &QAction::toggled, this,
            [this](bool on){ insQuick_->setBlinkEnabled(on); });

    // Per-instrument colours are opt-in via double-click in the right-side
    // dock (and the 'Colours: all on/off' master button in the dock
    // header). No View-menu toggle — opt-in keeps the editor from turning
    // into a wall of colour by default.
    pattern_->setInstrColorsEnabled(true);

    auto *boomA = viewMenu->addAction("&Boomwhacker note colours");
    boomA->setCheckable(true);
    boomA->setToolTip(
        "Paint each note cell in the pattern grid with the Boomwhacker / "
        "handbell pitch palette: C=red, D=orange, E=yellow, F=green, "
        "G=light blue, A=dark blue, B=purple. Sharps stay slate. Text "
        "switches between black and white per cell for contrast. Useful "
        "for teaching / arranging when reading note names is faster than "
        "reading note letters.");
    {
        QSettings s;
        bool on = s.value("editor/noteColors", false).toBool();
        boomA->setChecked(on);
        pattern_->setNoteColorsEnabled(on);
    }
    connect(boomA, &QAction::toggled, this, [this](bool on) {
        pattern_->setNoteColorsEnabled(on);
        QSettings s; s.setValue("editor/noteColors", on);
    });

    auto *sidIndA = viewMenu->addAction("&SID waveform indicators");
    sidIndA->setCheckable(true);
    sidIndA->setToolTip(
        "Show the per-voice lit-box block above each channel: T S P (triangle "
        "/ sawtooth / pulse) in the first column, N y r F (noise / sync / ring "
        "/ filter route) in the second. Boxes light up in real time from the "
        "live SID control register. Off reclaims the space for the VU bar + "
        "scope curve.");
    {
        QSettings s;
        bool on = s.value("editor/sidIndicators", true).toBool();
        sidIndA->setChecked(on);
        pattern_->setSidIndicatorsEnabled(on);
    }
    connect(sidIndA, &QAction::toggled, this, [this](bool on) {
        pattern_->setSidIndicatorsEnabled(on);
        QSettings s; s.setValue("editor/sidIndicators", on);
    });

    auto *cmdHoverA = viewMenu->addAction("Decode &commands on hover");
    cmdHoverA->setCheckable(true);
    cmdHoverA->setToolTip(
        "Hover a pattern command cell (the command nibble + databyte) to see "
        "a tooltip explaining the GoatTracker command and its parameters.");
    {
        QSettings s;
        bool on = s.value("editor/cmdHover", true).toBool();
        cmdHoverA->setChecked(on);
        pattern_->setCmdHoverEnabled(on);
    }
    connect(cmdHoverA, &QAction::toggled, this, [this](bool on) {
        pattern_->setCmdHoverEnabled(on);
        QSettings s; s.setValue("editor/cmdHover", on);
    });

    // ---- VU meter colour scheme ---------------------------------------
    // Drives the vertical inter-track VU bars. Order must match PatternView's
    // kVuSchemes table.
    auto *vuMenu = viewMenu->addMenu("VU &meter colours");
    auto *vuGroup = new QActionGroup(this);
    const char *vuNames[] = { "&Classic", "&Rainbow", "&Spectrum", "&Heat", "&Mono" };
    int curScheme = QSettings().value("editor/vuScheme", 1).toInt();
    if (curScheme < 0 || curScheme > 4) curScheme = 1;
    pattern_->setVuScheme(curScheme);
    for (int i = 0; i < 5; ++i) {
        auto *a = vuMenu->addAction(vuNames[i]);
        a->setCheckable(true);
        a->setActionGroup(vuGroup);
        a->setChecked(i == curScheme);
        connect(a, &QAction::triggered, this, [this, i]() {
            pattern_->setVuScheme(i);
            QSettings s; s.setValue("editor/vuScheme", i);
        });
    }

    // ---- Insert row mode submenu --------------------------------------
    // Pattern editor Insert / Ctrl+Backspace can either grow / shrink the
    // pattern length by one, or push rows off / pull rows in while keeping
    // pattlen fixed. The user picks which feels right from a radio group
    // here; the choice persists via editor/insertGrows.
    auto *insertMenu = viewMenu->addMenu("&Insert row mode");
    auto *insertGroup = new QActionGroup(this);
    auto *insGrow = insertMenu->addAction("Grow / shrink pattern length");
    insGrow->setCheckable(true);
    insGrow->setActionGroup(insertGroup);
    auto *insPush = insertMenu->addAction("Push last row off / pull empty in (fixed length)");
    insPush->setCheckable(true);
    insPush->setActionGroup(insertGroup);
    {
        QSettings s;
        bool grows = s.value("editor/insertGrows", false).toBool();
        pattern_->setInsertGrowsPattern(grows);
        if (grows) insGrow->setChecked(true);
        else       insPush->setChecked(true);
    }
    connect(insGrow, &QAction::triggered, this, [this]() {
        pattern_->setInsertGrowsPattern(true);
        QSettings s; s.setValue("editor/insertGrows", true);
    });
    connect(insPush, &QAction::triggered, this, [this]() {
        pattern_->setInsertGrowsPattern(false);
        QSettings s; s.setValue("editor/insertGrows", false);
    });

    // ---- Beat tinting submenu ------------------------------------------
    // 'Every 4th row' beat band + 'every 16th row' downbeat band are nice
    // for 4/4 in a 16-th-note grid, but punk-up a waltz (3/4) or a 6/8
    // shuffle and the tint lands on the wrong row. Let the user pick the
    // rows-per-beat + beats-per-bar combination from a single submenu.
    auto *beatMenu = viewMenu->addMenu("&Beat tinting");
    auto applyBeatGrid = [this](int rpb, int bpb) {
        pattern_->setBeatGrid(rpb, bpb);
        QSettings s;
        s.setValue("editor/beatRows", rpb);
        s.setValue("editor/barBeats", bpb);
    };
    {
        QSettings s;
        int rpb = s.value("editor/beatRows", 4).toInt();
        int bpb = s.value("editor/barBeats", 4).toInt();
        pattern_->setBeatGrid(rpb, bpb);
    }
    struct BeatPreset { const char *label; int rpb; int bpb; };
    static const BeatPreset presets[] = {
        { "4/4 — 4 rows / beat, 4 beats / bar (default)", 4, 4 },
        { "4/4 — 8 rows / beat, 4 beats / bar (32-row bar)", 8, 4 },
        { "3/4 — 4 rows / beat, 3 beats / bar (waltz)", 4, 3 },
        { "6/8 — 3 rows / beat, 6 beats / bar (shuffle)", 3, 6 },
        { "2/4 — 4 rows / beat, 2 beats / bar (polka)", 4, 2 },
        { "5/4 — 4 rows / beat, 5 beats / bar", 4, 5 },
        { "7/8 — 2 rows / beat, 7 beats / bar", 2, 7 },
    };
    auto *beatGroup = new QActionGroup(this);
    for (const auto &p : presets) {
        auto *a = beatMenu->addAction(p.label);
        a->setCheckable(true);
        a->setActionGroup(beatGroup);
        int rpb = p.rpb, bpb = p.bpb;
        if (rpb == pattern_->rowsPerBeat() && bpb == pattern_->beatsPerBar())
            a->setChecked(true);
        connect(a, &QAction::triggered, this, [applyBeatGrid, rpb, bpb]() {
            applyBeatGrid(rpb, bpb);
        });
    }

    // ---- Settings menu (microtonal / tuning / keypreset) ---------------
    auto *settingsMenu = menuBar()->addMenu("&Settings");

    auto *tuningMenu = settingsMenu->addMenu("&Tuning");
    auto *tuningGroup = new QActionGroup(this);
    tuningGroup->setExclusive(true);
    auto addTuning = [&](const QString &label, void (MainWindow::*slot)()) {
        auto *a = tuningMenu->addAction(label);
        a->setCheckable(true);
        tuningGroup->addAction(a);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    auto *t12A = addTuning("12-TET (default)", &MainWindow::setTuning12Tet);
    t12A->setChecked(true);
    addTuning("19-TET",            &MainWindow::setTuning19Tet);
    addTuning("24-TET",            &MainWindow::setTuning24Tet);
    addTuning("Custom N-TET…",     &MainWindow::setTuningCustomNTet);
    tuningMenu->addSeparator();
    auto *scalaA = tuningMenu->addAction("Load Scala .scl file…");
    connect(scalaA, &QAction::triggered, this, &MainWindow::loadScalaFile);
    tuningMenu->addSeparator();
    auto *resetTuningA = tuningMenu->addAction("&Reset to built-in table");
    connect(resetTuningA, &QAction::triggered, this, &MainWindow::resetTuning);

    auto *nameMenu = settingsMenu->addMenu("&Note names");
    auto *nameGroup = new QActionGroup(this);
    nameGroup->setExclusive(true);
    auto addNoteNames = [&](const QString &label, void (MainWindow::*slot)()) {
        auto *a = nameMenu->addAction(label);
        a->setCheckable(true);
        nameGroup->addAction(a);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    auto *n12A = addNoteNames("Standard 12 (C, C#, D…)", &MainWindow::setNoteNames12);
    n12A->setChecked(true);
    addNoteNames("Solfège (do, re, mi…)", &MainWindow::setNoteNamesSolfege);
    addNoteNames("Custom…",               &MainWindow::setNoteNamesCustom);
    nameMenu->addSeparator();
    auto *nResetA = nameMenu->addAction("Reset to defaults");
    connect(nResetA, &QAction::triggered, this, &MainWindow::setNoteNamesReset);

    auto *keyMenu = settingsMenu->addMenu("Note &entry layout");
    auto *keyGroup = new QActionGroup(this);
    keyGroup->setExclusive(true);
    auto addKey = [&](const QString &label, void (MainWindow::*slot)(), bool checked) {
        auto *a = keyMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(checked);
        keyGroup->addAction(a);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    addKey("Protracker (default)", &MainWindow::setKeyPresetTracker, keypreset == KEY_TRACKER);
    addKey("DMC",                  &MainWindow::setKeyPresetDmc,     keypreset == KEY_DMC);
    addKey("Janko / isomorphic",   &MainWindow::setKeyPresetJanko,   keypreset == KEY_JANKO);

    settingsMenu->addSeparator();
    auto *physA = settingsMenu->addAction("Use &physical (scancode) note layout");
    physA->setCheckable(true);
    physA->setToolTip(
        "Map the QWERTY note positions (bottom row Z..M + S D G H J sharps; "
        "top row Q..U + 2 3 5 6 7 sharps) by physical scancode instead of "
        "logical key. Lets users on Dvorak / AZERTY / Colemak play notes "
        "from the same physical keys as a QWERTY user. Hex digits and "
        "navigation keys still use the logical layout so typing hex stays "
        "natural. Currently Linux-only.");
    {
        QSettings s;
        bool on = s.value("editor/physicalKeyLayout", true).toBool();
        physA->setChecked(on);
        pattern_->setPhysicalKeyLayout(on);
    }
    connect(physA, &QAction::toggled, this, [this](bool on) {
        pattern_->setPhysicalKeyLayout(on);
        QSettings s; s.setValue("editor/physicalKeyLayout", on);
    });

    settingsMenu->addSeparator();
    auto *sidMenu = settingsMenu->addMenu("&SID config");
    auto *sidGroup = new QActionGroup(this);   // exclusive single/dual
    auto *singleA = sidMenu->addAction("&Single SID (3 channels)");
    singleA->setCheckable(true);
    singleA->setActionGroup(sidGroup);
    singleA->setToolTip("One SID — 3 channels (default mono).");
    auto *dualA = sidMenu->addAction("&Dual SID (6 channels)");
    dualA->setCheckable(true);
    dualA->setActionGroup(sidGroup);
    dualA->setToolTip("Two SIDs — 6 channels (stereo).");
    (stereo_mode != 0 ? dualA : singleA)->setChecked(true);
    singleSidAction_ = singleA;
    stereoAction_    = dualA;   // loadSongFile syncs this pair to the .sng
    // The group unchecks the sibling automatically; only the action that
    // turned ON drives the stereo switch.
    connect(dualA,   &QAction::toggled, this, [this](bool on) { if (on) toggleStereoMode(true);  });
    connect(singleA, &QAction::toggled, this, [this](bool on) { if (on) toggleStereoMode(false); });

    auto *playMenu = menuBar()->addMenu("&Play");
    auto *playA = playMenu->addAction(QString::fromUtf8("⏮  Play from &beginning"));
    playA->setShortcut(Qt::Key_F1);
    playA->setToolTip("Play song from beginning (F1)");
    connect(playA, &QAction::triggered, this, &MainWindow::playFromBeginning);
    auto *playPosA = playMenu->addAction(QString::fromUtf8("▶  Play from &position / pause"));
    playPosA->setShortcut(Qt::Key_F2);
    playPosA->setToolTip("Toggle play/pause from current order position (F2). "
                         "Resumes near where you stopped.");
    connect(playPosA, &QAction::triggered, this, &MainWindow::playFromPos);
    playPosAction_ = playPosA;
    auto *playPatA = playMenu->addAction(QString::fromUtf8("⧈  Play one pa&ttern"));
    playPatA->setShortcut(Qt::Key_F3);
    playPatA->setToolTip("Loop the current pattern (F3)");
    connect(playPatA, &QAction::triggered, this, &MainWindow::playPattern);
    auto *stopA = playMenu->addAction(QString::fromUtf8("⏹  &Stop"));
    stopA->setShortcut(Qt::Key_F4);
    stopA->setToolTip("Stop playback (F4)");
    connect(stopA, &QAction::triggered, this, &MainWindow::stopSong);
    playMenu->addSeparator();
    auto *muteA = playMenu->addAction("&Mute current channel");
    muteA->setShortcut(Qt::SHIFT | Qt::Key_F4);
    connect(muteA, &QAction::triggered, this, &MainWindow::muteCurrentChannel);
    auto *decSpA = playMenu->addAction("&Decrease speed multiplier");
    decSpA->setShortcut(Qt::SHIFT | Qt::Key_F5);
    connect(decSpA, &QAction::triggered, this, &MainWindow::prevMultiplierSlot);
    auto *incSpA = playMenu->addAction("&Increase speed multiplier");
    incSpA->setShortcut(Qt::SHIFT | Qt::Key_F6);
    connect(incSpA, &QAction::triggered, this, &MainWindow::nextMultiplierSlot);
    auto *sidA = playMenu->addAction("Toggle SID &model (6581/8580)");
    sidA->setShortcut(Qt::SHIFT | Qt::Key_F8);
    connect(sidA, &QAction::triggered, this, &MainWindow::toggleSidModel);

    auto *helpMenu = menuBar()->addMenu("&Help");
    auto *cheatA = helpMenu->addAction("&Command chart…");
    cheatA->setShortcut(Qt::Key_F12);
    cheatA->setToolTip("Reference card for track effects, wavetable / pulse / "
                       "filter commands, chord spellings + the Qt frontend's "
                       "keyboard shortcuts.");
    connect(cheatA, &QAction::triggered, this, &MainWindow::showCheatSheet);
    helpMenu->addSeparator();
    auto *aboutA = helpMenu->addAction("&About GoatTracker Qt…");
    connect(aboutA, &QAction::triggered, this, &MainWindow::showAbout);
    auto *aboutQtA = helpMenu->addAction("About &Qt…");
    connect(aboutQtA, &QAction::triggered, [this]() { QMessageBox::aboutQt(this); });

    auto *tb = addToolBar("Transport");
    tb->setMovable(false);
    tb->setStyleSheet(QString(
        "QToolBar { background:%1; spacing:4px; padding:6px; border:0; }"
        "QToolButton { color:%2; background:%3; padding:5px 10px; border:1px solid %4; border-radius:4px; min-width:0; }"
        "QToolButton:hover { background:%5; }"
        // Play family — distinct shades + icon-sized
        "QToolButton#playBegin  { background:#2F8C3A; color:#FFFFFF; border-color:#3FB950; font-weight:bold; }"
        "QToolButton#playBegin:hover  { background:#3FB950; }"
        "QToolButton#playPos    { background:#1F5E7A; color:#E0E6EE; border-color:#3892B5; }"
        "QToolButton#playPos:hover    { background:#3892B5; }"
        "QToolButton#playPatt   { background:#7A5A1F; color:#E0E6EE; border-color:#D9A441; }"
        "QToolButton#playPatt:hover   { background:#B58E38; }"
        "QToolButton#stopBtn    { background:#8C2F2F; color:#FFFFFF; border-color:#E5484D; font-weight:bold; }"
        "QToolButton#stopBtn:hover    { background:#E5484D; }"
        // Active-state glow. Driven by the dynamic 'active' property set
        // in tick() once per UI refresh. Bright saturated fill + a thick
        // bright border so the running transport stands out at a glance.
        "QToolButton#playBegin[active=\"true\"] { background:#3FB950; color:#FFFFFF; border:2px solid #9CFFB7; }"
        "QToolButton#playPos[active=\"true\"]   { background:#3892B5; color:#FFFFFF; border:2px solid #9CDDF5; }"
        "QToolButton#playPatt[active=\"true\"]  { background:#D9A441; color:#1A1A1A; border:2px solid #FFE5A1; }"
        "QToolButton#stopBtn[active=\"true\"]   { background:#E5484D; color:#FFFFFF; border:2px solid #FFB3B5; }"
    )
        .arg(Theme::C::bgAlt.name())
        .arg(Theme::C::text.name())
        .arg(Theme::C::bgBase.name())
        .arg(Theme::C::sep.name())
        .arg(Theme::C::editRow.name()));

    // Transport group: three buttons — Begin, Pos (which doubles as
    // Pause while playing), Patt. Stop dropped from the toolbar because
    // it's a functional duplicate of Pos-toggle-while-playing: both call
    // stopsong(), and the next Pos always restarts from the current
    // cursor anyway. Stop still lives on F4 + the Play menu for users
    // who want the explicit keyboard shortcut.
    tb->addAction(playA);
    tb->addAction(playPosA);
    tb->addAction(playPatA);
    if (auto *btn = qobject_cast<QToolButton*>(tb->widgetForAction(playA)))
        { btn->setObjectName("playBegin"); btn->setText("⏮ Begin");
          btn->setAccessibleName("Play from beginning");
          btn->setAccessibleDescription("Start playback from the first pattern of the song.");
          playBeginBtn_ = btn; }
    if (auto *btn = qobject_cast<QToolButton*>(tb->widgetForAction(playPosA))) {
        btn->setObjectName("playPos");
        btn->setText("▶ Pos");
        btn->setAccessibleName("Play or pause from position");
        btn->setAccessibleDescription("Toggle playback from the current order position.");
        playPosBtn_ = btn;
        // Lock width to the wider of '▶ Pos' / '⏸ Pause' so the rest of
        // the toolbar doesn't shift left/right every time the label
        // flips on play / pause.
        QFontMetrics fm(btn->font());
        int wPos   = fm.horizontalAdvance("▶ Pos");
        int wPause = fm.horizontalAdvance("⏸ Pause");
        btn->setMinimumWidth(qMax(wPos, wPause) + 28); // +padding
    }
    // onTransportChanged re-labels playPos between ▶ Pos / ⏸ Pause.
    if (auto *btn = qobject_cast<QToolButton*>(tb->widgetForAction(playPatA)))
        { btn->setObjectName("playPatt");  btn->setText("⟳ Patt");
          btn->setAccessibleName("Play pattern");
          btn->setAccessibleDescription("Loop the current pattern continuously.");
          playPattBtn_ = btn; }

    auto addSpacer = [&](int w = 28) {
        auto *spacer = new QWidget(tb);
        spacer->setFixedWidth(w);
        tb->addWidget(spacer);
    };

    addSpacer();

    // Editor-mode switching lives on the QTabBar above the editor stack
    // (see buildUi) — the old "Pattern editor / Order/song editor / …"
    // toolbar buttons were a duplicate of those tabs and have been removed.
    // F5-F8, Tab/Shift-Tab and the Mode menu still drive editmode.

    // Follow-play stays on the toolbar (frequently toggled); the two dock
    // toggles ("Order map" / "Instruments") live in the View menu only, to
    // avoid visual collision with the mode switches above.
    tb->addAction(followA);

    tb->style()->unpolish(tb);
    tb->style()->polish(tb);

    statusStrip_->showMessage("Ready. Ctrl+O to open a song.");
    syncStack();
}

QWidget *MainWindow::editorView(int idx) const {
    switch (idx) {
        case EDIT_PATTERN:    return pattern_;
        case EDIT_ORDERLIST:  return order_;
        case EDIT_INSTRUMENT: return instrument_;
        case EDIT_TABLES:     return tables_;
        case EDIT_NAMES:      return songName_;
    }
    return nullptr;
}

void MainWindow::showEvent(QShowEvent *e) {
    QMainWindow::showEvent(e);
    // The dock tab bar is built lazily — after first show and after the
    // ctor's restoreState(). That rebuild drops our pictograms, tab indices
    // and the tear-off filter, so (re)apply them here once the bar exists.
    // Deferred so the bar is fully materialised before we touch it.
    QTimer::singleShot(0, this, [this]{ applyDockTabIcons(); });
}

void MainWindow::applyDockTabIcons() {
    if (!editorArea_) return;
    // findChildren is recursive, so it also turns up tab bars *inside* the
    // editors (e.g. TablesView's Wave/Pulse/Filter/Speed). Only the dock-group
    // bars carry our editor titles, so we icon + filter those, and leave the
    // inner ones untouched.
    for (QTabBar *bar : editorArea_->findChildren<QTabBar*>()) {
        bool touched = false;
        for (int t = 0; t < bar->count(); ++t) {
            for (int i = 0; i < EDITOR_COUNT; ++i) {
                if (bar->tabText(t) == QLatin1String(EDITOR_TITLE[i])) {
                    bar->setTabIcon(t, editorIcon_[i]);
                    // NB: do NOT setTabData here — QMainWindow reserves the
                    // dock tab bar's tabData to hold the QDockWidget pointer.
                    // Overwriting it corrupts dock bookkeeping (crashes on
                    // re-dock / restoreState). Tear-off matches by title.
                    touched = true;
                    break;
                }
            }
        }
        if (!touched) continue;
        bar->setIconSize(QSize(18, 18));
        // Watch this dock-group bar for tab tear-off (once — guard with a
        // dynamic property since the bar persists across reapplies).
        if (!bar->property("tearFilter").toBool()) {
            bar->installEventFilter(this);
            bar->setProperty("tearFilter", true);
        }
    }
}

void MainWindow::syncStack() {
    if (editmode < 0 || editmode > EDIT_NAMES) editmode = EDIT_PATTERN;
    // Raise the target editor's dock to the front of its tab group (and, if
    // the user tore it off onto another monitor, pop that window forward).
    if (QDockWidget *d = editorDock_[editmode]) {
        d->show();
        d->raise();
        if (d->isFloating()) d->activateWindow();
    }
    QWidget *w = editorView(editmode);
    if (w) {
        w->setFocus();
        // Accessibility: announce the editor now active. Single funnel point
        // for F5-F8, Tab/Shift-Tab and the Mode menu, so a blind user always
        // knows which editor they landed on. Reuses the view's accessibleName
        // ("Pattern editor", "Order and song editor", …).
        Speech::instance().say(w->accessibleName(), Speech::Priority::Status);
    }
}

// editmode follows keyboard focus: clicking into (or Tab-ing through) any
// editor — docked or floated onto a second monitor — makes it the one the
// engine edits. Walks up from the freshly-focused widget to find which of
// the five editor views (if any) owns it.
void MainWindow::onFocusChanged(QWidget * /*old*/, QWidget *now) {
    if (!now) return;
    for (int i = 0; i < EDITOR_COUNT; ++i) {
        QWidget *v = editorView(i);
        if (v && (v == now || v->isAncestorOf(now))) {
            if (editmode != i) { editmode = i; refreshAll(); }
            return;
        }
    }
}

void MainWindow::cycleEditMode(bool backwards) {
    if (backwards) editmode--;
    else editmode++;
    if (editmode > EDIT_NAMES) editmode = EDIT_PATTERN;
    if (editmode < EDIT_PATTERN) editmode = EDIT_NAMES;
    syncStack();
    refreshAll();
}

static void rememberDir(const QString &filePath, char *pathSlot, int slotSize) {
    QFileInfo fi(filePath);
    QString dir = fi.absolutePath();
    if (!dir.endsWith('/')) dir += '/';
    QByteArray ba = dir.toLocal8Bit();
    std::strncpy(pathSlot, ba.constData(), slotSize - 1);
    pathSlot[slotSize - 1] = 0;
}

static QString titleForSong(const QString &path) {
    if (path.isEmpty()) return "GoatTracker Qt";
    return QString("GoatTracker Qt — %1").arg(QFileInfo(path).fileName());
}

void MainWindow::loadSongFile(const QString &path) {
    qInfo("load: path=%s", qPrintable(path));
    // AudioFence:
    //   1. QAudioSink::suspend() (cooperative hint)
    //   2. lock the audio mutex — waits for the in-flight PullDevice::readData
    //      to return, so the audio thread can't be inside playroutine() /
    //      sid_fillbuffer() while we rewrite chn[] / sidreg[] / songorder[]
    //   3. stopsong + songinit=PLAY_STOPPED so the next fill after resume()
    //      doesn't reanimate the half-loaded state
    // Released at end of this scope -> sink resumes against the new song.
    AudioFence fence;

    QByteArray ba = path.toLocal8Bit();
    std::strncpy(songfilename, ba.constData(), MAX_FILENAME - 1);
    songfilename[MAX_FILENAME - 1] = 0;
    rememberDir(path, songpath, MAX_PATHNAME);
    setWindowTitle(titleForSong(path));
    // Reset editor cursors so the views point at row 0 of pattern 0 — any
    // stale eppos from a previously edited song would land past the end of
    // the new song's patterns and the grid would look empty.
    // clearsong() (called by loadsong) also zeroes eseditpos / esnum and
    // sets einum=1, so the orderlist + instrument cursors snap to the
    // start of the new song automatically.
    eppos = 0;
    epcolumn = 0;
    epchn = 0;
    eschn = 0;
    for (int c = 0; c < MAX_CHN; c++) espos[c] = 0;
    epoctave = 4;          // default play octave — middle of the C64 range
    // Clear the Pos-resume bookmark too. Without this, loading a new song
    // while a previous one was paused (or just played) mid-song left
    // pausedSongptr_/pausedPattRow_ pointing into the OLD song, so the next
    // Play-from-position resumed the NEW song from that stale offset instead
    // of the top. Homing it here makes a fresh Pos start from the (now home)
    // editor cursor.
    pausedAtPos_ = false;
    pausedPattRow_ = 0;
    for (int c = 0; c < MAX_CHN; c++) pausedSongptr_[c] = 0;
    // PatternView::refresh() will yank the vertical scrollbar back to 0
    // on the next refreshAll() call because (eppos < rowOffset) → setValue(eppos).
    // Wipe chn[] so stale pattptr / songptr / pattnum / gate / instr from
    // the previous song don't bleed into the new one's first play. loadsong
    // already calls clearsong() internally which resets songorder /
    // pattern[] / instr[] / tables, but chn[] is playroutine state, not
    // song state, so loadsong leaves it alone. Sequential imports of
    // .sid / .mod (each tmp-staged through this path) showed odd
    // first-play artifacts otherwise.
    initchannels();
    loadsong();
    // loadsong() set song_channels from the file (3 = mono, 6 = stereo/dual
    // SID). Mirror that into the runtime stereo state and (re)build SID2 to
    // match — still inside the AudioFence above, so the audio thread can't be
    // mid-render while sid_init tears the SID down + rebuilds.
    stereo_mode = (song_channels >= MAX_CHN) ? 1 : 0;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    // Keep the Settings ▸ SID config radio pair in sync with the loaded song.
    // Block both signals so this doesn't re-enter toggleStereoMode (which
    // would reseed channels + re-init the SID we just set up).
    if (stereoAction_ && singleSidAction_) {
        QSignalBlocker b1(stereoAction_);
        QSignalBlocker b2(singleSidAction_);
        (stereo_mode != 0 ? stereoAction_ : singleSidAction_)->setChecked(true);
    }
    qInfo("load: done patterns=%d instr=%d song_channels=%d stereo=%d",
          highestusedpattern, highestusedinstr, song_channels, stereo_mode);
    countpatternlengths();
    undoStack_->clear();   // loaded state starts a fresh history
    refreshAll();
    if (auto *w = activeEditorWidget()) w->update();
    statusStrip_->showMessage(QString("Loaded: %1").arg(path));
    addRecentFile(path);
}

// --- Open Recent ----------------------------------------------------------
// Persist the last 10 user-opened .sng files. Only native .sng paths land
// here — .sid / .mid / .mod imports stage through a temp .sng (see
// loadSidFile) whose path we don't want to remember, so temp-dir paths and
// non-.sng extensions are filtered out.
void MainWindow::addRecentFile(const QString &path) {
    if (!path.endsWith(".sng", Qt::CaseInsensitive)) return;
    QFileInfo fi(path);
    QString abs = fi.absoluteFilePath();
    if (abs.startsWith(QDir::tempPath(), Qt::CaseInsensitive)) return;
    // De-dupe (case-insensitive on the absolute path), newest first, cap 10.
    recentFiles_.removeIf([&](const QString &p) {
        return p.compare(abs, Qt::CaseInsensitive) == 0;
    });
    recentFiles_.prepend(abs);
    while (recentFiles_.size() > MAX_RECENT) recentFiles_.removeLast();
    saveRecentFiles();
    updateRecentMenu();
}

void MainWindow::updateRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    if (recentFiles_.isEmpty()) {
        QAction *none = recentMenu_->addAction("(no recent files)");
        none->setEnabled(false);
        return;
    }
    int i = 1;
    for (const QString &path : recentFiles_) {
        // &1..&9 mnemonics for the first nine; show the file name, full path
        // in the tooltip. The 10th item just shows its name (no mnemonic).
        const QString name = QFileInfo(path).fileName();
        const QString label = (i <= 9) ? QStringLiteral("&%1  %2").arg(i).arg(name)
                                       : name;
        QAction *a = recentMenu_->addAction(label);
        a->setToolTip(path);
        connect(a, &QAction::triggered, this, [this, path]() {
            if (!QFileInfo::exists(path)) {
                statusStrip_->showMessage(QString("Missing: %1").arg(path));
                recentFiles_.removeAll(path);
                saveRecentFiles();
                updateRecentMenu();
                return;
            }
            loadSongFile(path);
        });
        ++i;
    }
    recentMenu_->addSeparator();
    QAction *clear = recentMenu_->addAction("&Clear Recent");
    connect(clear, &QAction::triggered, this, [this]() {
        recentFiles_.clear();
        saveRecentFiles();
        updateRecentMenu();
    });
}

void MainWindow::loadRecentFiles() {
    QSettings s;
    recentFiles_ = s.value("recentFiles").toStringList();
    while (recentFiles_.size() > MAX_RECENT) recentFiles_.removeLast();
}

void MainWindow::saveRecentFiles() {
    QSettings s;
    s.setValue("recentFiles", recentFiles_);
}

void MainWindow::newSong() {
    // Treat 'undo history present' as 'might have unsaved edits' so the
    // user doesn't lose work by accident. The undo stack also tracks
    // post-load edits (loadSongFile clears it), so this is a tight
    // enough heuristic without bolting a separate dirty flag onto the
    // C core.
    if (undoStack_->canUndo()) {
        QMessageBox::StandardButton choice = QMessageBox::question(this,
            "Start a new song?",
            "The current song has unsaved edits. Discard them and start "
            "a fresh empty project?",
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (choice != QMessageBox::Discard) return;
    }

    {
        AudioFence fence;
        clearsong(1, 1, 1, 1, 1);
        initchannels();
        countpatternlengths();
        eppos = 0;
        epcolumn = 0;
        epchn = 0;
        eschn = 0;
        for (int c = 0; c < MAX_CHN; c++) espos[c] = 0;
        epoctave = 4;
        // Forget the previous file: Save behaves as Save-As next time.
        songfilename[0] = 0;
        setWindowTitle(titleForSong(""));
    }
    undoStack_->clear();
    refreshAll();
    statusStrip_->showMessage("New song");
}

void MainWindow::openSong() {
    QString start = songpath[0] ? QString::fromLocal8Bit(songpath)
                                : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString filter;
    if (chiptunesakAvailable()) {
        filter = "GoatTracker / SID / MIDI / MOD (*.sng *.sid *.mid *.midi *.mod);;"
                 "SNG (*.sng);;SID (*.sid);;MIDI (*.mid *.midi);;MOD (*.mod);;"
                 "All (*.*)";
    } else {
        // ChiptuneSAK not on the host -> only the native .sng path is
        // available. Mention how to enable the extras in the dialog
        // title so the user knows it's optional, not broken.
        filter = "SNG (*.sng);;All (*.*)";
    }
    QString fn = QFileDialog::getOpenFileName(this, "Open Song", start, filter);
    if (fn.isEmpty()) return;
    if (fn.endsWith(".sid", Qt::CaseInsensitive)) {
        if (!chiptunesakAvailable()) {
            QMessageBox::warning(this, "SID import requires ChiptuneSAK",
                "Importing .sid files needs the ChiptuneSAK Python module.\n\n"
                "Install with uv (recommended):\n"
                "  uv venv ext/chiptunesak/venv\n"
                "  uv pip install --python ext/chiptunesak/venv/bin/python \\\n"
                "      mido matplotlib numpy more-itertools parameterized\n"
                "  git clone https://github.com/c64cryptoboy/ChiptuneSAK\n"
                "  export GT2_CHIPTUNESAK_PATH=/path/to/ChiptuneSAK\n\n"
                "Or pip install chiptunesak into your system Python. "
                "Restart the editor after installing — the SID / MIDI "
                "filters will appear in the Open Song dialog. See the "
                "README for full instructions.");
            return;
        }
        loadSidFile(fn);
        return;
    }
    // .mid / .midi / .mod handled by loadSidFile too via the same
    // wrapper — sid_to_sng.py dispatches by extension to the right
    // importer (ChiptuneSAK MIDI for .mid, in-process ProTracker
    // parser for .mod). Anything else funnels through loadSongFile.
    if (fn.endsWith(".mid",  Qt::CaseInsensitive) ||
        fn.endsWith(".midi", Qt::CaseInsensitive) ||
        fn.endsWith(".mod",  Qt::CaseInsensitive)) {
        if (!chiptunesakAvailable()) {
            const QString kind = fn.endsWith(".mod", Qt::CaseInsensitive)
                ? QStringLiteral("MOD") : QStringLiteral("MIDI");
            QMessageBox::warning(this,
                QString("%1 import requires ChiptuneSAK").arg(kind),
                QString("Importing %1 files needs the ChiptuneSAK "
                        "Python module. See Help > About for "
                        "installation instructions.").arg(kind));
            return;
        }
        loadSidFile(fn);  // wrapper dispatches by extension
        return;
    }
    loadSongFile(fn);
}

bool MainWindow::chiptunesakAvailable() {
    if (chiptunesakCached_ >= 0) return chiptunesakCached_ != 0;

    QString py = QStandardPaths::findExecutable("python3");
    if (py.isEmpty()) py = QStandardPaths::findExecutable("python");
    if (py.isEmpty()) {
        qInfo("chiptunesak probe: no python3 / python on PATH");
        chiptunesakCached_ = 0;
        return false;
    }

    // Build PYTHONPATH the same way loadSidFile does so the probe
    // matches what the importer will see. GT2_CHIPTUNESAK_PATH wins;
    // otherwise we rely on whatever's on the system Python's
    // site-packages (e.g. a pip install).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString chipPath = qEnvironmentVariable("GT2_CHIPTUNESAK_PATH");
    if (!chipPath.isEmpty()) {
        QString existing = env.value("PYTHONPATH");
        env.insert("PYTHONPATH",
                   existing.isEmpty() ? chipPath
                                      : chipPath + ":" + existing);
    }

    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start(py, QStringList{} << "-c"
               << "import chiptunesak.sid;import chiptunesak.goat_tracker");
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        proc.waitForFinished(500);
        qInfo("chiptunesak probe: python3 -c 'import chiptunesak' timed out");
        chiptunesakCached_ = 0;
        return false;
    }
    chiptunesakCached_ = (proc.exitCode() == 0) ? 1 : 0;
    qInfo("chiptunesak probe: %s (exit=%d)",
          chiptunesakCached_ ? "AVAILABLE" : "absent",
          proc.exitCode());
    return chiptunesakCached_ != 0;
}

void MainWindow::showCheatSheet() {
    // Reuse one dialog across F12 presses — opening / closing doesn't
    // tear down the QTextBrowser content. Parented to MainWindow so it
    // closes when the editor quits.
    static QPointer<QDialog> dlg;
    if (dlg) {
        dlg->raise();
        dlg->activateWindow();
        return;
    }
    dlg = new QDialog(this);
    dlg->setWindowTitle("GoatTracker Qt — command chart");
    dlg->resize(1100, 800);
    auto *lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(8, 8, 8, 8);
    auto *tb = new QTextBrowser(dlg);
    tb->setOpenExternalLinks(true);
    tb->setHtml(cheatSheetHtml());
    lay->addWidget(tb);
    auto *closeBtn = new QPushButton("Close", dlg);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    auto *bottom = new QHBoxLayout();
    bottom->addStretch();
    bottom->addWidget(closeBtn);
    lay->addLayout(bottom);
    dlg->show();
}

void MainWindow::showAbout() {
    QMessageBox box(this);
    box.setWindowTitle("About GoatTracker Qt");
    box.setTextFormat(Qt::RichText);
    // GoatTracker Qt logo, scaled to a tidy dialog size.
    box.setIconPixmap(QPixmap(":/icons/goat.png")
                          .scaled(96, 96, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation));
    box.setText(
        "<h2>GoatTracker 2 — Qt edition</h2>"
        "<p>Native Qt6 frontend for the Commodore 64 SID chip tracker.</p>");
    box.setInformativeText(
        "<p><b>Original GoatTracker authors</b></p>"
        "<ul>"
        "<li>Lasse Öörni — original editor + playroutine "
        "(<a href='http://covertbitops.c64.org'>covertbitops.c64.org</a>)</li>"
        "<li>Táli Sándor — HardSID 4U support</li>"
        "<li>Antonio Vera — GoatTracker icon</li>"
        "<li>Simon Bennett — command quick reference</li>"
        "<li>Patches by Stefan A. Haubenthal, Valerio Cannone, "
        "Raine M. Ekman, Groepaz, drfiemost, Tero Lindeman, "
        "Henrik Paulini</li>"
        "<li>Microtonal support by Birgit Jauernig</li>"
        "</ul>"

        "<p><b>SID emulation</b></p>"
        "<ul>"
        "<li>Dag Lem — original reSID engine</li>"
        "<li>Antti Lankila — reSID-fp nonlinear filter</li>"
        "<li>Leandro Nini et al. — libresidfp (vendored under src/residfp/)</li>"
        "</ul>"

        "<p><b>Tools</b></p>"
        "<ul>"
        "<li>Magnus Lind — 6510 crossassembler from Exomizer 2</li>"
        "<li>David Knapp &amp; David Youd — ChiptuneSAK, driven by "
        "<code>ext/chiptunesak/sid_to_sng.py</code> to load arbitrary "
        ".sid files back into the editor "
        "(<a href='https://github.com/c64cryptoboy/ChiptuneSAK'>github.com/c64cryptoboy/ChiptuneSAK</a>)</li>"
        "<li>sasq64 — sid2sng (still vendored under ext/sid2sng/ as a "
        "fallback CLI for GoatTracker-generated .sids; no longer used "
        "by the Qt loader)</li>"
        "</ul>"

        "<p><b>Audio / GUI stack</b></p>"
        "<ul>"
        "<li>PortAudio — cross-platform low-latency audio</li>"
        "<li>Qt 6 — GUI toolkit</li>"
        "<li>SDL 1.2 — kept for the BME helpers used by the engine</li>"
        "</ul>"

        "<p><b>Qt frontend + integrations</b></p>"
        "<ul>"
        "<li><b>Paul Honig</b> — original idea for the Qt frontend, "
        "ongoing maintenance, design direction, and prompting.</li>"
        "<li>Qt6 port, libresidfp adaptation, dual-SID runtime toggle, "
        "microtonal backport, Janko / DMC / Protracker keypreset, JSON-RPC, "
        "instrument-colour palette, ADSR drag handles, pointer preview, "
        "ChiptuneSAK-based .sid loader, status-strip widgets, Order map dock — by "
        "Claude (Anthropic) Opus 4.7 in collaboration with Paul.</li>"
        "</ul>"

        "<p><b>License</b> — GNU General Public License v2 or later. "
        "See <code>COPYING</code>.</p>"
        "<p>Covert BitOps homepage: "
        "<a href='http://covertbitops.c64.org'>covertbitops.c64.org</a></p>");
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

// Convert a .sid into a .sng via the ChiptuneSAK Python library and
// hand the result off to loadSongFile so the rest of the load path
// (AudioFence, cursor reset, undoStack clear, refreshAll) stays the
// same.
//
// We drive ChiptuneSAK through ext/chiptunesak/sid_to_sng.py rather
// than the old vendored sid2sng CLI: ChiptuneSAK runs an actual 6502
// emulator over the SID's player code, so it imports PSID + many RSID
// files (sid2sng only handled GoatTracker-generated .sids and even
// then required hand-picked flag combinations).
//
// ChiptuneSAK is pure Python and must be importable. If it isn't on
// the system Python's site-packages, point GT2_CHIPTUNESAK_PATH at a
// source checkout (or set PYTHONPATH yourself before launching the
// editor). See the README 'Importing .sid / MIDI' section for the
// uv-based install recipe.
// extraOpts is kept on the signature for ABI compatibility with the
// previous loader but is no longer consulted.
void MainWindow::loadSidFile(const QString &path, const QStringList &extraOpts) {
    Q_UNUSED(extraOpts);

    // The wrapper script ships alongside the source tree, not the
    // binary. Search common locations relative to the running exe so a
    // single-tree dev build and an installed build both work.
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList scriptCandidates = {
        appDir + "/ext/chiptunesak/sid_to_sng.py",
        appDir + "/../ext/chiptunesak/sid_to_sng.py",
        appDir + "/../../ext/chiptunesak/sid_to_sng.py",
        // Worktree / out-of-source CMake build: src tree is at
        // <repo>/qt while build is at <repo>/build/qt.
        appDir + "/../../qt/../ext/chiptunesak/sid_to_sng.py",
    };
    QString script;
    for (const QString &c : scriptCandidates) {
        if (QFile::exists(c)) { script = QFileInfo(c).canonicalFilePath(); break; }
    }
    if (script.isEmpty()) {
        QMessageBox::warning(this, "Cannot open .sid",
            "sid_to_sng.py not found. Tried:\n  " + scriptCandidates.join("\n  "));
        return;
    }

    const QString tmp = QDir::tempPath() + "/" +
        QFileInfo(path).completeBaseName() + ".sng";
    QFile::remove(tmp);  // stale output from a previous failed attempt

    QStringList args;
    args << script
         << QFileInfo(path).absoluteFilePath()
         << tmp;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Allow the user to point at a checkout of ChiptuneSAK without
    // installing it. GT2_CHIPTUNESAK_PATH controls this; otherwise
    // we rely on the system Python's site-packages (pip install).
    QString chipPath = qEnvironmentVariable("GT2_CHIPTUNESAK_PATH");
    if (!chipPath.isEmpty()) {
        const QString existing = env.value("PYTHONPATH");
        env.insert("PYTHONPATH",
                   existing.isEmpty() ? chipPath
                                      : chipPath + ":" + existing);
    }

    statusStrip_->showMessage(
        QString("Converting %1 via ChiptuneSAK…").arg(QFileInfo(path).fileName()),
        0);
    QApplication::processEvents();

    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    // python3 is the canonical Linux/macOS entry point; on Windows
    // python.exe is what's on PATH. Prefer python3 if it resolves.
    QString py = QStandardPaths::findExecutable("python3");
    if (py.isEmpty()) py = QStandardPaths::findExecutable("python");
    if (py.isEmpty()) py = "python3";
    proc.start(py, args);

    // 15s budget covers the 60s default capture for everything we've
    // tried; ChiptuneSAK's emulator is roughly 5-10x faster than real
    // time on a modern CPU.
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        proc.waitForFinished(1000);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle("ChiptuneSAK conversion timed out");
        box.setText(QString("ChiptuneSAK did not finish converting %1 "
                            "within 15 seconds.")
                    .arg(QFileInfo(path).fileName()));
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
        statusStrip_->showMessage("SID load timed out.");
        return;
    }

    const int exitCode = proc.exitCode();
    const QString out = QString::fromLocal8Bit(proc.readAll());

    if (exitCode == 0 && QFile::exists(tmp) && QFileInfo(tmp).size() > 0) {
        loadSongFile(tmp);
        // loadSongFile stored 'path' as songfilename / songpath, which
        // points at the /tmp staging file the wrapper wrote. Replace
        // both with the ORIGINAL source path so the next Open Song
        // dialog opens in the imported file's directory and Save acts
        // like a fresh session ('Save As' the user picks the location
        // — silently overwriting the tmp file would be surprising).
        const QString absSrc = QFileInfo(path).absoluteFilePath();
        std::strncpy(songfilename, absSrc.toLocal8Bit().constData(), MAX_FILENAME - 1);
        songfilename[MAX_FILENAME - 1] = 0;
        rememberDir(absSrc, songpath, MAX_PATHNAME);
        setWindowTitle(titleForSong(absSrc + " (imported)"));
        statusStrip_->showMessage(
            QString("Loaded from .sid (ChiptuneSAK): %1")
                .arg(QFileInfo(path).fileName()));
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("ChiptuneSAK conversion failed");
    if (exitCode == 2) {
        // Wrapper signals 'module missing' with exit code 2.
        box.setText("ChiptuneSAK is not importable.\n\n"
                    "Install it (e.g. `pip install --editable .` inside a "
                    "venv on PATH) or set GT2_CHIPTUNESAK_PATH / PYTHONPATH "
                    "to point at a ChiptuneSAK source checkout, then try "
                    "opening the .sid again.");
    } else {
        box.setText(QString("ChiptuneSAK could not convert %1 (exit %2).")
                    .arg(QFileInfo(path).fileName())
                    .arg(exitCode));
    }
    QString tail = out;
    if (tail.size() > 600) tail = tail.right(600);
    box.setDetailedText(QString("python: %1\nargs:   %2\nexit:   %3\n\n--- output ---\n%4")
                        .arg(py).arg(args.join(" ")).arg(exitCode).arg(tail));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
    statusStrip_->showMessage("SID load failed.");
}

// Append a second song's patterns / orderlists / instruments / tables onto
// the currently-loaded song. Uses the v2.73 mergesong() C helper which
// reads from the global `songfilename` slot.
void MainWindow::mergeSong() {
    QString start = songpath[0] ? QString::fromLocal8Bit(songpath)
                                : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString fn = QFileDialog::getOpenFileName(this, "Merge Song", start, "SNG (*.sng);;All (*.*)");
    if (fn.isEmpty()) return;
    QByteArray before = beginEdit();
    QByteArray ba = fn.toLocal8Bit();
    std::strncpy(songfilename, ba.constData(), MAX_FILENAME - 1);
    songfilename[MAX_FILENAME - 1] = 0;
    rememberDir(fn, songpath, MAX_PATHNAME);
    mergesong();
    countpatternlengths();
    refreshAll();
    endEdit(before, "Merge song");
    statusStrip_->showMessage(QString("Merged: %1").arg(fn));
}

void MainWindow::saveSong() {
    if (!songfilename[0]) { saveSongAs(); return; }
    if (savesong()) {
        statusStrip_->showMessage(QString("Saved: %1").arg(songfilename));
        setWindowTitle(titleForSong(QString::fromLocal8Bit(songfilename)));
    } else statusStrip_->showMessage("Save failed");
}

void MainWindow::packAndRelocate() {
    qInfo("pack: songfilename=%s", songfilename[0] ? songfilename : "(empty)");
    if (!songfilename[0]) {
        statusStrip_->showMessage("Save the .sng first, then pack");
        qWarning("pack: no songfilename — aborting");
        return;
    }
    // Suggest output next to the current .sng with a sane extension.
    QString songDir = QFileInfo(QString::fromLocal8Bit(songfilename)).absolutePath();
    QString stem = QFileInfo(QString::fromLocal8Bit(songfilename)).baseName();
    QString outPath = QFileDialog::getSaveFileName(this,
        "Pack && Relocate", songDir + "/" + stem + ".sid",
        "C64 PRG (*.prg);;PSID (*.sid);;Raw BIN (*.bin)");
    qInfo("pack: outPath=%s", qPrintable(outPath));
    if (outPath.isEmpty()) { qInfo("pack: user cancelled"); return; }

    // Save the song first to make sure the gt2reloc subprocess sees the
    // current state, including any unsaved edits.
    int r = savesong();
    qInfo("pack: savesong()=%d", r);
    if (!r) {
        statusStrip_->showMessage("Save before pack failed");
        qWarning("pack: savesong failed — aborting");
        return;
    }

    // gt2reloc may live next to our binary (single-tree build), one directory
    // up under qt/ (top-level cmake adds qt/ as a subdir, gt2reloc lands at
    // build/qt/gt2reloc while the wrapper goattrk2-qt lands at build/qt/),
    // or in PATH (install).
    QStringList candidates = {
        QCoreApplication::applicationDirPath() + "/gt2reloc",
        QCoreApplication::applicationDirPath() + "/qt/gt2reloc",
        QCoreApplication::applicationDirPath() + "/../qt/gt2reloc",
        "gt2reloc"
    };
    QString tool;
    for (const QString &c : candidates) {
        qInfo("pack: candidate tool=%s exists=%d", qPrintable(c), QFile::exists(c));
        if (QFile::exists(c)) { tool = c; break; }
    }
    if (tool.isEmpty()) {
        statusStrip_->showMessage("gt2reloc not found (tried: "
            + candidates.join(", ") + ")");
        qWarning("pack: gt2reloc not found in any candidate path");
        return;
    }
    qInfo("pack: tool=%s", qPrintable(tool));

    // gt2reloc uses bme/io_open which falls back to fopen() against the
    // current working directory to find player.s / altplayer.s. Run it
    // from the directory that holds those files — try common candidates.
    QStringList srcDirs = {
        QCoreApplication::applicationDirPath() + "/../../src",  // build/qt → ../../src
        QCoreApplication::applicationDirPath() + "/../src",     // build → ../src
        QCoreApplication::applicationDirPath() + "/src",
        "src", "."
    };
    QString workDir;
    for (const QString &d : srcDirs) {
        bool ok = QFile::exists(d + "/player.s");
        qInfo("pack: srcDir candidate=%s player.s=%d", qPrintable(d), ok);
        if (ok) { workDir = d; break; }
    }
    qInfo("pack: workDir=%s", qPrintable(workDir));
    QProcess proc;
    if (!workDir.isEmpty()) proc.setWorkingDirectory(workDir);
    QStringList args;
    // Pass absolute paths so the song / output don't get resolved relative
    // to the workingDirectory we just changed into.
    args << QFileInfo(QString::fromLocal8Bit(songfilename)).absoluteFilePath()
         << QFileInfo(outPath).absoluteFilePath();
    qInfo("pack: args=[%s]", qPrintable(args.join(" | ")));
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(tool, args);
    if (!proc.waitForStarted(5000)) {
        QMessageBox::warning(this, "Pack failed",
            QString("gt2reloc could not start\ntool: %1\ncwd: %2\nerror: %3")
                .arg(tool).arg(workDir).arg(proc.errorString()));
        return;
    }
    if (!proc.waitForFinished(15000)) {
        QMessageBox::warning(this, "Pack failed",
            QString("gt2reloc timed out\ntool: %1\ncwd: %2\nargs: %3")
                .arg(tool).arg(workDir).arg(args.join(" ")));
        proc.kill();
        return;
    }
    QString out = QString::fromLocal8Bit(proc.readAll());
    QString absOut = QFileInfo(outPath).absoluteFilePath();
    QFileInfo fi(absOut);
    qInfo("pack: exit=%d absOut=%s exists=%d size=%lld",
          proc.exitCode(), qPrintable(absOut), fi.exists(), (long long)fi.size());
    qInfo("pack: gt2reloc output:\n%s", qPrintable(out));
    if (proc.exitCode() != 0 || !fi.exists()) {
        QMessageBox::warning(this, "Pack failed",
            QString("gt2reloc exit %1\ntool: %2\ncwd: %3\nargs: %4\noutput exists: %5\n\n--- gt2reloc output ---\n%6")
                .arg(proc.exitCode()).arg(tool).arg(workDir)
                .arg(args.join(" ")).arg(fi.exists() ? "yes" : "no").arg(out));
        return;
    }
    statusStrip_->showMessage(QString("Packed: %1 (%2 bytes)")
                              .arg(absOut).arg(fi.size()));
}

void MainWindow::saveSongAs() {
    QString start = songpath[0] ? QString::fromLocal8Bit(songpath)
                  : songfilename[0] ? QString::fromLocal8Bit(songfilename)
                  : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString fn = QFileDialog::getSaveFileName(this, "Save Song", start, "SNG (*.sng)");
    if (fn.isEmpty()) return;
    QByteArray ba = fn.toLocal8Bit();
    std::strncpy(songfilename, ba.constData(), MAX_FILENAME - 1);
    songfilename[MAX_FILENAME - 1] = 0;
    rememberDir(fn, songpath, MAX_PATHNAME);
    saveSong();
}

void MainWindow::loadInstrument() {
    QString start = instrpath[0] ? QString::fromLocal8Bit(instrpath)
                                 : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString fn = QFileDialog::getOpenFileName(this, "Load Instrument", start, "INS (*.ins);;All (*.*)");
    if (fn.isEmpty()) return;
    QByteArray ba = fn.toLocal8Bit();
    std::strncpy(instrfilename, ba.constData(), MAX_FILENAME - 1);
    instrfilename[MAX_FILENAME - 1] = 0;
    rememberDir(fn, instrpath, MAX_PATHNAME);
    loadinstrument();
    refreshAll();
    statusStrip_->showMessage(QString("Loaded ins: %1").arg(fn));
}

void MainWindow::saveInstrument() {
    QString start = instrpath[0] ? QString::fromLocal8Bit(instrpath)
                  : instrfilename[0] ? QString::fromLocal8Bit(instrfilename)
                  : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString fn = QFileDialog::getSaveFileName(this, "Save Instrument", start, "INS (*.ins)");
    if (fn.isEmpty()) return;
    QByteArray ba = fn.toLocal8Bit();
    std::strncpy(instrfilename, ba.constData(), MAX_FILENAME - 1);
    instrfilename[MAX_FILENAME - 1] = 0;
    rememberDir(fn, instrpath, MAX_PATHNAME);
    if (saveinstrument()) statusStrip_->showMessage(QString("Saved ins: %1").arg(fn));
    else statusStrip_->showMessage("Save instrument failed");
}

void MainWindow::refreshAll() {
    pattern_->refresh();
    order_->refresh();
    instrument_->refresh();
    tables_->refresh();
    songName_->refresh();
    orderMap_->refresh();
    insQuick_->refresh();
    statusStrip_->refresh();
    if (patternBarOct_)
        patternBarOct_->setText(QString::number(epoctave));
    if (patternBarLen_) {
        int p = epnum[epchn];
        patternBarLen_->setText(QString("$%1")
            .arg(pattlen[p], 2, 16, QLatin1Char('0')).toUpper());
    }
    if (patternBarTempo_ && epchn >= 0 && epchn < MAX_CHN) {
        // chn[].tempo is the reload value (ticks-1); >=2 is a normal tempo,
        // <2 means funktempo is active on that channel.
        int t = chn[epchn].tempo;
        patternBarTempo_->setText(t >= 2 ? QString::number(t + 1)
                                         : QStringLiteral("funk"));
    }

    // Transport glow — light up whichever Play / Stop button reflects the
    // engine's current state. lastsonginit holds the most recent mode the
    // playroutine was started in (PLAY_BEGINNING / PLAY_POS / PLAY_PATTERN);
    // isplaying() flips between running and stopped. We set a dynamic
    // 'active' property + repolish the style so the per-id selectors in
    // the toolbar stylesheet kick in.
    auto setActive = [](QWidget *w, bool on) {
        if (!w) return;
        if (w->property("active").toBool() == on) return;
        w->setProperty("active", on);
        w->style()->unpolish(w);
        w->style()->polish(w);
    };
    bool playing = isplaying() != 0;
    // Glow rule (option B from the user):
    //   Begin    pure trigger — never holds an active state. Click =
    //            'start over from the top'.
    //   Pos      owns 'is playing'. Glows whenever the engine is
    //            running OR while paused (label flips between
    //            ⏸ Pause / ▶ Pos via onTransportChanged).
    //   Patt     glows on PLAY_PATTERN alongside Pos (pattern is
    //            looping = is playing, just constrained).
    bool playPatt = playing && lastsonginit == PLAY_PATTERN;
    setActive(playBeginBtn_, false);
    setActive(playPosBtn_,   playing || pausedAtPos_);
    setActive(playPattBtn_,  playPatt);
}

// Toolbar shrink/grow operate on the channel the cursor is on. Same byte-
// level invariant the L## header click dialog uses: a single ENDPATT byte
// (0xff) in the note column at row = pattlen, REST padding before it.
void MainWindow::shrinkPattern() {
    int p = epnum[epchn];
    int cur = pattlen[p];
    if (cur <= 1) return;
    int newLen = cur - 1;
    pattern[p][newLen*4 + 0] = ENDPATT;
    pattern[p][newLen*4 + 1] = 0;
    pattern[p][newLen*4 + 2] = 0;
    pattern[p][newLen*4 + 3] = 0;
    countpatternlengths();
    if (eppos >= pattlen[p]) eppos = pattlen[p] - 1;
    refreshAll();
}
void MainWindow::growPattern() {
    int p = epnum[epchn];
    int cur = pattlen[p];
    if (cur >= MAX_PATTROWS) return;
    int newLen = cur + 1;
    // Old ENDPATT slot becomes a REST row.
    pattern[p][cur*4 + 0] = REST;
    pattern[p][cur*4 + 1] = 0;
    pattern[p][cur*4 + 2] = 0;
    pattern[p][cur*4 + 3] = 0;
    pattern[p][newLen*4 + 0] = ENDPATT;
    pattern[p][newLen*4 + 1] = 0;
    pattern[p][newLen*4 + 2] = 0;
    pattern[p][newLen*4 + 3] = 0;
    countpatternlengths();
    refreshAll();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e) {
    // A torn-off editor floats as a native window with a close button. Since
    // its WM-owned title bar can't redock by dragging, the close button snaps
    // it back into the tab group instead of hiding it (editors always exist).
    if (auto *d = qobject_cast<QDockWidget*>(o)) {
        if (e->type() == QEvent::Close && d->isFloating()) {
            e->ignore();
            d->setFloating(false);
            return true;
        }
        return QMainWindow::eventFilter(o, e);
    }

    // Tab tear-off: drag a dock tab off its strip -> float that editor into
    // its own window (browser-style). Native QTabBar drag only reorders, so
    // we bridge to QDockWidget::setFloating() once the cursor leaves the bar.
    if (auto *bar = qobject_cast<QTabBar*>(o)) {
        switch (e->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
                // Identify the dragged editor by tab title (tabData is off
                // limits — Qt stores the dock pointer there).
                const int tab = bar->tabAt(me->pos());
                tearBar_ = bar;
                tearEditorIdx_ = -1;
                if (tab >= 0) {
                    const QString text = bar->tabText(tab);
                    for (int i = 0; i < EDITOR_COUNT; ++i)
                        if (text == QLatin1String(EDITOR_TITLE[i])) { tearEditorIdx_ = i; break; }
                }
                tearArmed_ = tearEditorIdx_ >= 0;
            }
            return false;   // let the bar do its normal select / reorder
        }
        case QEvent::MouseMove: {
            if (!tearArmed_ || bar != tearBar_) return false;
            auto *me = static_cast<QMouseEvent*>(e);
            if (!(me->buttons() & Qt::LeftButton)) return false;
            const int m = 26;   // px the cursor must leave the strip by
            const QPoint p = me->pos();
            const bool out = p.y() < -m || p.y() > bar->height() + m
                          || p.x() < -m || p.x() > bar->width() + m;
            if (!out) return false;
            // Tear-off detected. setFloating() reparents the dock tree, and
            // doing that here — inside the tab bar's own mouse-move, while it
            // holds a mouse grab + internal drag state — crashes. So capture
            // the target + drop point, end the bar's drag, and perform the
            // float on the next event-loop pass.
            const int idx = tearEditorIdx_;
            const QPoint g = bar->mapToGlobal(me->pos());
            tearArmed_ = false;
            bar->releaseMouse();   // end the bar's internal drag cleanly
            QTimer::singleShot(0, this, [this, idx, g]{
                if (idx < 0 || idx >= EDITOR_COUNT) return;
                QDockWidget *d = editorDock_[idx];
                if (d && !d->isFloating()) {
                    d->setFloating(true);
                    d->move(g - QPoint(60, 12));
                    d->raise();
                    d->activateWindow();
                    if (d->widget()) d->widget()->setFocus();
                }
            });
            return true;           // consume so it doesn't also reorder
        }
        case QEvent::MouseButtonRelease:
            tearArmed_ = false;
            tearBar_ = nullptr;
            return false;
        default:
            break;
        }
        return QMainWindow::eventFilter(o, e);
    }

    // Right-click on the Octave [+] button lowers — gives the user the
    // 'left = up, right = down' affordance they asked for on the same
    // step button, without losing the explicit [−] / [+] pair.
    if (e->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::RightButton) {
            if (epoctave > 0) { epoctave--; refreshAll(); }
            return true;
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::playFromBeginning() {
    followplay = 1;
    pausedAtPos_ = false;
    initsong(esnum, PLAY_BEGINNING);
    refreshAll();
}
void MainWindow::playFromPos() {
    if (isplaying()) {
        // Snapshot the engine's per-channel order position + the active
        // channel's in-pattern row so the next Pos toggle actually
        // RESUMES from here instead of jumping to song start. Previously
        // we always re-seeded chn[c].songptr = espos[c] (the editor's
        // order-cursor, which the user typically left at 0), so pausing
        // mid-song followed by resume snapped back to order 0.
        for (int c = 0; c < MAX_CHN; c++) pausedSongptr_[c] = chn[c].songptr;
        pausedPattRow_ = chn[epchn].pattptr / 4;
        stopsong();
        pausedAtPos_ = true;
        statusStrip_->showMessage("Paused");
    } else {
        followplay = 1;
        if (pausedAtPos_) {
            // Resume from the snapshot we took on pause.
            for (int c = 0; c < MAX_CHN; c++)
                chn[c].songptr = pausedSongptr_[c];
            int start = pausedPattRow_;
            if (start < 0) start = 0;
            initsongpos(esnum, PLAY_POS, start);
        } else {
            // Fresh Pos start from the editor cursor.
            for (int c = 0; c < MAX_CHN; c++) chn[c].songptr = espos[c];
            int start = eppos;
            if (start < 0) start = 0;
            initsongpos(esnum, PLAY_POS, start);
        }
        pausedAtPos_ = false;
    }
    refreshAll();
}
void MainWindow::playPattern() {
    followplay = 1;
    for (int c = 0; c < MAX_CHN; c++) {
        chn[c].pattnum = epnum[c];
        chn[c].songptr = espos[c];
    }
    int start = eppos;
    if (start < 0) start = 0;
    pausedAtPos_ = false;
    initsongpos(esnum, PLAY_PATTERN, start);
    refreshAll();
}
void MainWindow::stopSong() {
    stopsong();
    pausedAtPos_ = false;
    refreshAll();
}

void MainWindow::muteCurrentChannel() {
    mutechannel(epchn);
    // Visible + spoken feedback (showMessage also self-voices).
    statusStrip_->showMessage(QString("Channel %1 %2")
        .arg(epchn + 1).arg(chn[epchn].mute ? "muted" : "unmuted"));
}

// Adjust the speed multiplier with sid_init (fenced), like cycleMultiplier —
// NOT via qt_stubs' prevmultiplier/nextmultiplier, which call the full
// sound_init() and would spawn the second (SDL) audio backend + corrupt the
// heap. sid_init re-inits the SID for the new rate without touching the
// audio device.
void MainWindow::prevMultiplierSlot() {
    AudioFence fence;
    if (multiplier > 0) multiplier--;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    refreshAll();
}
void MainWindow::nextMultiplierSlot() {
    AudioFence fence;
    if (multiplier < 16) multiplier++;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    refreshAll();
}
void MainWindow::toggleStereoMode(bool on) {
    AudioFence fence;
    stereo_mode = on ? 1 : 0;
    // When promoting an existing mono song to stereo, channels 4-6 normally
    // have songlen=0 (nothing loaded for them) — the order map would render
    // them black. Seed each empty extra channel with a single pattern slot
    // pointing at a fresh pattern + RST endmark so the user has something
    // to edit on without dropping into Insert key spam first.
    if (on) {
        for (int c = 3; c < MAX_CHN; c++) {
            if (songlen[esnum][c] == 0) {
                songorder[esnum][c][0] = c;       // pattern N
                songorder[esnum][c][1] = LOOPSONG;
                songorder[esnum][c][2] = 0;        // restart at 0
                songlen[esnum][c] = 1;
            }
        }
    }
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    statusStrip_->showMessage(on
        ? "Stereo ON — 6 channels, dual SID"
        : "Stereo OFF — 3 channels, single SID");
    // Keep the Settings ▸ SID config radio in sync no matter who flipped
    // stereo (menu, the status-strip SID2 click, etc.). Signal-blocked so
    // this can't re-enter via the actions' toggled() handlers.
    if (stereoAction_ && singleSidAction_) {
        QSignalBlocker b1(stereoAction_);
        QSignalBlocker b2(singleSidAction_);
        (on ? stereoAction_ : singleSidAction_)->setChecked(true);
    }
    refreshAll();
}

void MainWindow::toggleSid2Model() {
    // Rebuild ONLY the SID (sid_init), fenced, exactly like toggleSidModel /
    // toggleStereoMode. The previous sound_init() ran the full BME snd_init —
    // which opens a SECOND audio backend (the SDL mixer thread, SDLAudioP1)
    // and reallocs channel/mixer buffers — racing PaAudio and corrupting the
    // heap ("malloc(): corrupted top size") / crashing in the SID. sid_init
    // already applies sid2model to SID2, so the full re-init was never needed.
    AudioFence fence;
    sid2model ^= 1;
    if (stereo_mode)
        sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    statusStrip_->showMessage(sid2model ? "SID2 → 8580" : "SID2 → 6581");
    refreshAll();
}

// Status-strip SID2 click cycles 3-state: off -> 6581 -> 8580 -> off.
// The 'enable dual SID' menu checkbox remains the explicit on/off control;
// the status-bar segment is a quick 'glance + click' affordance.
void MainWindow::cycleSid2() {
    if (!stereo_mode) {
        sid2model = 0;             // entering stereo defaults to 6581
        toggleStereoMode(true);    // already audio-fenced
        statusStrip_->showMessage("SID2 enabled — 6581");
        return;
    }
    if (sid2model == 0) {
        toggleSid2Model();         // 6581 -> 8580
        statusStrip_->showMessage("SID2 → 8580");
        return;
    }
    // sid2model == 1 (8580) -> off (stereo_mode off)
    toggleStereoMode(false);
    statusStrip_->showMessage("SID2 disabled");
}

void MainWindow::toggleSidModel() {
    // sid_init tears down + rebuilds the libresidfp instance the audio
    // thread is mid-clock on. AudioFence locks the mutex + hard-stops the
    // playroutine for the rebuild window.
    AudioFence fence;
    sidmodel ^= 1;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    statusStrip_->showMessage(sidmodel ? "Switched to 8580 SID"
                                       : "Switched to 6581 SID");
    refreshAll();
}

void MainWindow::toggleNtsc() {
    AudioFence fence;
    ntsc ^= 1;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    statusStrip_->showMessage(ntsc ? "Switched to NTSC 60Hz"
                                   : "Switched to PAL 50Hz");
    refreshAll();
}

void MainWindow::cycleMultiplier() {
    AudioFence fence;
    if (multiplier == 0)      multiplier = 1;
    else if (multiplier < 4)  multiplier++;
    else                       multiplier = 0;
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);
    statusStrip_->showMessage(QString("Speed multiplier: %1")
        .arg(multiplier == 0 ? "½x" : QString("%1x").arg(multiplier)));
    refreshAll();
}
void MainWindow::toggleFollowPlay() {
    followplay = !followplay;
    statusStrip_->showMessage(followplay ? "Follow-play ON" : "Follow-play OFF");
    refreshAll();
}

QWidget *MainWindow::activeEditorWidget() const {
    return editorView(editmode);
}

void MainWindow::tick() {
    // Turn any playback-state edges the audio thread recorded into Qt signals,
    // emitted here on the GUI thread (the audio thread only bumps lock-free
    // counters — it never emits, to stay realtime-safe). Cheap: 3 atomic loads.
    if (coreEvents_) coreEvents_->deliver();

    // VU / scope meter is a continuous signal — keep sampling it on the timer.
    // tickScope() short-circuits when the level hasn't changed, so an idle SID
    // costs nothing. (Must keep running even when stopped so jam / test notes
    // still show on the meter.)
    pattern_->tickScope();

    // Playback-driven repaints — follow-play cursor, order map, the Pos/Pause
    // label — are now event-driven via CoreEvents (onPlayRowChanged /
    // onOrderPosChanged / onTransportChanged). The timer only repaints the
    // pattern grid while STOPPED, so editor edits + cursor moves stay
    // responsive without a playback in progress.
    if (!isplaying() && pattern_->isVisible())
        pattern_->refresh();

    statusStrip_->refresh();
}

// --- CoreEvents notification handlers (GUI thread, queued from audio) -------

void MainWindow::onTransportChanged(bool playing) {
    // Relabel the Pos toolbar button so it always shows the action it performs.
    if (playPosAction_) {
        const QString desired = playing ? "⏸ Pause" : "▶ Pos";
        const QList<QObject*> objs = playPosAction_->associatedObjects();
        for (QObject *o : objs) {
            if (auto *btn = qobject_cast<QToolButton*>(o)) {
                if (btn->text() != desired) btn->setText(desired);
            }
        }
    }
    // Repaint once on the edge so the starting / final position and the
    // play-row highlight (or its clearing on stop) show immediately.
    if (pattern_->isVisible()) pattern_->refresh();
    if (orderMap_) orderMap_->refresh();
    // Accessibility: announce playback start. The stop / pause side already
    // goes through statusStrip_->showMessage() (which now also speaks), so we
    // only voice the start here to avoid a double "Paused / Stopped".
    if (playing) Speech::instance().say("Playing", Speech::Priority::Status);
}

void MainWindow::onPlayRowChanged() {
    if (pattern_->isVisible()) pattern_->refresh();
}

void MainWindow::onOrderPosChanged() {
    if (orderMap_) orderMap_->refresh();
}

QByteArray MainWindow::beginEdit() {
    return captureSongSnapshot();
}

void MainWindow::endEdit(QByteArray before, const QString &label) {
    auto *cmd = new SongSnapshotCommand(std::move(before), label);
    undoStack_->push(cmd);
}

void MainWindow::undo() {
    if (!undoStack_->canUndo()) {
        statusStrip_->showMessage("Nothing to undo");
        return;
    }
    QString label = undoStack_->undoText();
    undoStack_->undo();
    statusStrip_->showMessage(QString("Undo: %1").arg(label));
    refreshAll();
}

void MainWindow::redo() {
    if (!undoStack_->canRedo()) {
        statusStrip_->showMessage("Nothing to redo");
        return;
    }
    QString label = undoStack_->redoText();
    undoStack_->redo();
    statusStrip_->showMessage(QString("Redo: %1").arg(label));
    refreshAll();
}

// ----- Microtonal / Scala / keyboard layout settings ------------------------
// Backport of v2.75's -Q / -J / -Y / Janko features. The shared C
// functions (calculatefreqtable / setspecialnotenames / readscalatuningfile)
// live in qt_globals.c. Any slot that changes the freq table also kicks
// notename[] back to defaults first so we don't reuse stale microtonal
// labels from a previous Scala load.

static void applyNTet(float n, const char *label, StatusStrip *strip) {
    equaldivisionsperoctave = n;
    tuningcount = 0;            // disables the ratio path in calculatefreqtable()
    if (basepitch <= 0.0f) basepitch = 440.0f;
    calculatefreqtable();
    resetnotenames();
    if (strip) strip->showMessage(QString("Tuning: %1").arg(label));
}

void MainWindow::setTuning12Tet() { applyNTet(12.0f, "12-TET", statusStrip_); }
void MainWindow::setTuning19Tet() { applyNTet(19.0f, "19-TET", statusStrip_); }
void MainWindow::setTuning24Tet() { applyNTet(24.0f, "24-TET", statusStrip_); }

void MainWindow::setTuningCustomNTet() {
    bool ok = false;
    double n = QInputDialog::getDouble(this, "Custom N-TET",
        "Equal divisions per octave\n(e.g. 31, or 8.2019143 for Bohlen-Pierce):",
        equaldivisionsperoctave, 1.0, 96.0, 4, &ok);
    if (!ok) return;
    applyNTet((float)n, QString("%1-TET").arg(n).toUtf8().constData(), statusStrip_);
}

void MainWindow::loadScalaFile() {
    QString fn = QFileDialog::getOpenFileName(this, "Load Scala tuning",
        QString(), "Scala tuning (*.scl);;All (*.*)");
    if (fn.isEmpty()) return;
    QByteArray ba = fn.toLocal8Bit();
    std::strncpy(scalatuningfilepath, ba.constData(), MAX_PATHNAME - 1);
    scalatuningfilepath[MAX_PATHNAME - 1] = 0;
    tuningcount = 0;
    specialnotenames[0] = '\0';
    readscalatuningfile();
    if (tuningcount <= 0) {
        statusStrip_->showMessage("Scala load failed (no tuning ratios parsed)");
        return;
    }
    if (basepitch <= 0.0f) basepitch = 440.0f;
    calculatefreqtable();
    resetnotenames();
    if (specialnotenames[0] && specialnotenames[1]) setspecialnotenames();
    statusStrip_->showMessage(QString("Loaded Scala: %1 (%2 ratios)")
        .arg(tuningname[0] ? QString::fromLocal8Bit(tuningname) : QFileInfo(fn).fileName())
        .arg(tuningcount));
    refreshAll();
}

void MainWindow::resetTuning() {
    equaldivisionsperoctave = 12.0f;
    tuningcount = 0;
    specialnotenames[0] = '\0';
    scalatuningfilepath[0] = '\0';
    basepitch = 0.0f;  // signal "use built-in baked freqtable"
    // We can't restore the original baked freqtable without re-reading
    // gplay.c's initialiser, so just recompute 12-TET at 440Hz. Close enough
    // for hearing the reset land; matches what -G440 -Q12 produces.
    basepitch = 440.0f;
    calculatefreqtable();
    basepitch = 0.0f;
    resetnotenames();
    statusStrip_->showMessage("Tuning reset to 12-TET defaults");
    refreshAll();
}

void MainWindow::setNoteNames12() {
    resetnotenames();
    specialnotenames[0] = '\0';
    statusStrip_->showMessage("Note names: standard 12");
    refreshAll();
}

void MainWindow::setNoteNamesSolfege() {
    // Two-char-per-note pack for solfège — sharps keep the C# style label
    // so the cycle still has 12 entries (matches 12-TET).
    static const char *names12[12] = {
        "Do","C#","Re","D#","Mi","Fa","F#","So","G#","La","A#","Ti"
    };
    int i = 0;
    for (int n = 0; n < 12; n++) {
        specialnotenames[i++] = names12[n][0];
        specialnotenames[i++] = names12[n][1];
    }
    specialnotenames[i] = '\0';
    setspecialnotenames();
    statusStrip_->showMessage("Note names: solfège");
    refreshAll();
}

void MainWindow::setNoteNamesCustom() {
    bool ok = false;
    QString cur = QString::fromLocal8Bit(specialnotenames);
    QString s = QInputDialog::getText(this, "Custom note names",
        "Two chars per note within an octave/cycle.\n"
        "E.g. C-DbD-EbE-F-GbG-AbA-BbB-",
        QLineEdit::Normal, cur, &ok);
    if (!ok) return;
    QByteArray ba = s.toLocal8Bit();
    if (ba.size() < 2) {
        statusStrip_->showMessage("Need at least one 2-char name");
        return;
    }
    std::strncpy(specialnotenames, ba.constData(), sizeof(specialnotenames) - 1);
    specialnotenames[sizeof(specialnotenames) - 1] = '\0';
    setspecialnotenames();
    statusStrip_->showMessage("Note names: custom");
    refreshAll();
}

void MainWindow::setNoteNamesReset() {
    specialnotenames[0] = '\0';
    resetnotenames();
    statusStrip_->showMessage("Note names reset");
    refreshAll();
}

void MainWindow::setKeyPresetTracker() {
    keypreset = KEY_TRACKER;
    statusStrip_->showMessage("Note entry: Protracker layout");
}
void MainWindow::setKeyPresetDmc() {
    keypreset = KEY_DMC;
    statusStrip_->showMessage("Note entry: DMC layout");
}
void MainWindow::setKeyPresetJanko() {
    keypreset = KEY_JANKO;
    statusStrip_->showMessage("Note entry: Janko (isomorphic) layout");
}
