#include <QApplication>
#include <QTimer>
#include <QStringList>
#include <QStyleFactory>
#include <QPalette>
#include <QSettings>
#include <SDL/SDL.h>
#include <cstring>
#include "MainWindow.h"
#include "RpcServer.h"
#include "PaAudio.h"
#include "Theme.h"
#include "Log.h"

extern "C" {
#include "gcommon.h"
#include "gsound.h"
#include "gplay.h"
void initchannels(void);
int sound_init(unsigned b, unsigned mr, unsigned writer, unsigned hardsid,
               unsigned m, unsigned ntsc, unsigned multiplier,
               unsigned catweasel, unsigned interpolate, unsigned customclockrate);
void sound_uninit(void);

extern unsigned b, mr, writer, hardsid, sidmodel, ntsc, multiplier;
extern unsigned sid2model;
extern int stereo_mode;
extern unsigned catweasel, interpolate, customclockrate;
extern unsigned keypreset;
extern int followplay;
extern char songfilename[];
extern int songinit;
void loadsong(void);
void countpatternlengths(void);
void clearsong(int cs, int cp, int ci, int ct, int cn);
void sid_init(int speed, unsigned m, unsigned ntsc, unsigned interpolate,
              unsigned customclockrate, unsigned usefp);
}
#include "gplay.h" // PLAY_STOPPED

// SDL 1.2 (and sdl12-compat) #define main SDL_main on macOS/Windows so its
// SDLmain shim can wrap our entry point. This app owns its entry point via
// Qt, so undo that and keep a real main().
#ifdef main
#undef main
#endif

int main(int argc, char **argv) {
    QApplication::setOrganizationName("goattrk2-qt");
    QApplication::setApplicationName("goattrk2-qt");
    QApplication app(argc, argv);
    Log::init();
    qInfo("argv: argc=%d", argc);
    for (int i = 0; i < argc; i++) qInfo("  argv[%d]=%s", i, argv[i]);

    // Restore user preferences from QSettings ($XDG_CONFIG_HOME/goattrk2-qt
    // /goattrk2-qt.conf on Linux). Applied BEFORE sid_init so the SID
    // backend starts with the chip model the user last saved. loadSongFile()
    // later overrides sidmodel / sid2model from the .sng header, so songs
    // written against a specific chip still play back authentically.
    {
        QSettings s;
        sidmodel    = s.value("audio/sidmodel",   sidmodel  ).toUInt();
        sid2model   = s.value("audio/sid2model",  sid2model ).toUInt();
        stereo_mode = s.value("audio/stereo",     stereo_mode).toInt();
        ntsc        = s.value("audio/ntsc",       ntsc      ).toUInt();
        multiplier  = s.value("audio/multiplier", multiplier).toUInt();
        keypreset   = s.value("editor/keypreset", keypreset ).toUInt();
        followplay  = s.value("editor/followplay",followplay).toInt();
        qInfo("prefs loaded from %s: sid1=%u sid2=%u stereo=%d ntsc=%u "
              "mult=%u key=%u follow=%d",
              qPrintable(s.fileName()),
              sidmodel, sid2model, stereo_mode, ntsc, multiplier,
              keypreset, followplay);
    }

    // Persist on shutdown — no manual 'Save preferences' menu item; the
    // app simply remembers what the user did last.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        QSettings s;
        s.setValue("audio/sidmodel",   sidmodel);
        s.setValue("audio/sid2model",  sid2model);
        s.setValue("audio/stereo",     stereo_mode);
        s.setValue("audio/ntsc",       ntsc);
        s.setValue("audio/multiplier", multiplier);
        s.setValue("editor/keypreset", keypreset);
        s.setValue("editor/followplay",followplay);
        s.sync();
    });

    // Force Fusion style + an app-wide dark palette so the editor looks the
    // same across Linux / Windows / macOS. Native Windows style was
    // rendering QLineEdit / QSpinBox text in the default light-on-white
    // theme, making instrument fields unreadable.
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window,          Theme::C::bgBase);
    pal.setColor(QPalette::WindowText,      Theme::C::text);
    pal.setColor(QPalette::Base,            Theme::C::bgBase);
    pal.setColor(QPalette::AlternateBase,   Theme::C::bgAlt);
    pal.setColor(QPalette::Text,            Theme::C::text);
    pal.setColor(QPalette::Button,          Theme::C::bgAlt);
    pal.setColor(QPalette::ButtonText,      Theme::C::text);
    pal.setColor(QPalette::Highlight,       Theme::C::editRow);
    pal.setColor(QPalette::HighlightedText, Theme::C::text);
    pal.setColor(QPalette::ToolTipBase,     Theme::C::bgAlt);
    pal.setColor(QPalette::ToolTipText,     Theme::C::text);
    pal.setColor(QPalette::PlaceholderText, Theme::C::textDim);
    pal.setColor(QPalette::Disabled, QPalette::Text,       Theme::C::textDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, Theme::C::textDim);
    app.setPalette(pal);

    // Filter our own flags so Qt's platform parser doesn't choke.
    bool rpcMode = false;
    QString songArg;
    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); i++) {
        if (args[i] == "--rpc") rpcMode = true;
        else if (!args[i].startsWith("-")) songArg = args[i];
    }

    // SDL audio init removed — QAudioSink owns the audio device now. SDL is
    // only kept around for misc helpers in the bundled bme code, so init the
    // timer subsystem (cheap, no device locking).
    if (SDL_Init(SDL_INIT_TIMER) < 0) {
        qWarning("SDL_Init timer failed: %s", SDL_GetError());
    }

    initchannels();
    songinit = PLAY_STOPPED;
    // Initialise libresidfp directly. Stick with DECIMATE (interpolate=0)
    // for the audio thread — RESAMPLE's sinc filter is 5-10× slower and was
    // causing the audio thread to miss its real-time deadline, producing
    // the 'instruments not playing on time' stutter the user reported. The
    // ticks RESAMPLE was supposed to fix turned out to be the underrun
    // memset(0) (now sample-hold) and not aliasing.
    sid_init((int)mr, sidmodel, ntsc, /*interpolate=*/0, customclockrate, 1);

    PaAudio audio;
    if (!audio.start((int)mr)) {
        qWarning("PaAudio failed to start; running silent.");
    }

    // Start with a fresh empty song so the pattern editor is immediately
    // usable (default 64-row patterns, RST endmark on each channel). The SDL
    // build does this implicitly via the main loop init; the Qt build skipped
    // it and left pattlen[] zeroed.
    clearsong(1, 1, 1, 1, 1);
    countpatternlengths();

    MainWindow w;
    w.show();
    if (!songArg.isEmpty()) w.loadSongFile(songArg);

    Rpc *rpc = nullptr;
    if (rpcMode) rpc = new Rpc(&w);

    int rc = app.exec();

    delete rpc;
    audio.stop();
    SDL_Quit();
    return rc;
}
