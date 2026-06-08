#include "RpcServer.h"
#include "MainWindow.h"
#include "PatternView.h"

#include <QCoreApplication>
#include <QApplication>
#include <QWidget>
#include <QAction>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QBuffer>
#include <QByteArray>
#include <QPixmap>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <SDL/SDL_keysym.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
#include "gcommon.h"
#include "gfile.h"  // MAX_FILENAME
#include "gsong.h"
#include "gplay.h"
extern unsigned char pattern[MAX_PATT][MAX_PATTROWS*4+4];
extern int pattlen[MAX_PATT];
extern unsigned char songorder[MAX_SONGS][MAX_CHN][MAX_SONGLEN+2];
extern int songlen[MAX_SONGS][MAX_CHN];
extern INSTR instr[MAX_INSTR];
extern unsigned char ltable[MAX_TABLES][MAX_TABLELEN];
extern unsigned char rtable[MAX_TABLES][MAX_TABLELEN];
extern int eppos, epchn, epcolumn, epoctave;
extern int epnum[MAX_CHN];
extern int eppos;
extern int einum;
extern int esnum;
extern int eschn;
extern int espos[MAX_CHN];
extern int etnum, etpos;
extern int editmode, followplay;
extern unsigned sidmodel, multiplier, ntsc;
extern char songname[MAX_STR];
extern char authorname[MAX_STR];
extern char copyrightname[MAX_STR];
extern char songfilename[];
extern int key;
extern int rawkey;
extern int shiftpressed;
extern CHN chn[MAX_CHN];
int isplaying(void);
void loadsong(void);
int savesong(void);
void countpatternlengths(void);
}

static QString notenameFor(unsigned char v) {
    static const char *names[] = {
        "C-0","C#0","D-0","D#0","E-0","F-0","F#0","G-0","G#0","A-0","A#0","B-0",
        "C-1","C#1","D-1","D#1","E-1","F-1","F#1","G-1","G#1","A-1","A#1","B-1",
        "C-2","C#2","D-2","D#2","E-2","F-2","F#2","G-2","G#2","A-2","A#2","B-2",
        "C-3","C#3","D-3","D#3","E-3","F-3","F#3","G-3","G#3","A-3","A#3","B-3",
        "C-4","C#4","D-4","D#4","E-4","F-4","F#4","G-4","G#4","A-4","A#4","B-4",
        "C-5","C#5","D-5","D#5","E-5","F-5","F#5","G-5","G#5","A-5","A#5","B-5",
        "C-6","C#6","D-6","D#6","E-6","F-6","F#6","G-6","G#6","A-6","A#6","B-6",
        "C-7","C#7","D-7","D#7","E-7","F-7","F#7","G-7","G#7"
    };
    if (v == REST)   return "REST";
    if (v == KEYOFF) return "KEYOFF";
    if (v == KEYON)  return "KEYON";
    if (v >= FIRSTNOTE && v <= LASTNOTE)
        return names[v - FIRSTNOTE];
    return "";
}

static int parseHex(const QString &s, int def = 0) {
    bool ok = false;
    int v = s.toInt(&ok, 16);
    return ok ? v : def;
}

Rpc::Rpc(MainWindow *mw, QObject *parent)
    : QObject(parent), mw_(mw) {
    // Make stdin non-blocking so partial reads return promptly.
    // POSIX-only; on Windows the QSocketNotifier path below is not used.
#ifndef _WIN32
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);
#endif
    notifier_ = new QSocketNotifier(0, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated,
            this, &Rpc::onStdinReadable);
    // Banner
    QJsonObject ready{
        {"event", "ready"},
        {"protocol", "goattrk2-qt-rpc/1"}
    };
    auto bytes = QJsonDocument(ready).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

Rpc::~Rpc() = default;

void Rpc::onStdinReadable(int) {
    char tmp[4096];
    ssize_t n;
    while ((n = ::read(0, tmp, sizeof tmp)) > 0) {
        buf_.append(tmp, (int)n);
    }
    while (true) {
        int nl = buf_.indexOf('\n');
        if (nl < 0) break;
        QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject e{
                {"error", QString("parse: %1").arg(err.errorString())},
                {"line", QString::fromUtf8(line)}
            };
            auto bytes = QJsonDocument(e).toJson(QJsonDocument::Compact);
            std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
            continue;
        }
        handleCommand(doc.object());
    }
    if (n == 0) {
        // EOF on stdin — exit cleanly.
        QCoreApplication::quit();
    }
}

void Rpc::handleCommand(const QJsonObject &cmd) {
    QJsonObject out = dispatch(cmd);
    reply(out, cmd);
}

void Rpc::reply(const QJsonObject &payload, const QJsonObject &cmd) {
    QJsonObject envelope = payload;
    if (cmd.contains("id")) envelope["id"] = cmd.value("id");
    envelope["ok"] = !envelope.contains("error");
    auto bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void Rpc::replyError(const QString &msg, const QJsonObject &cmd) {
    reply(QJsonObject{{"error", msg}}, cmd);
}

QJsonObject Rpc::dispatch(const QJsonObject &c) {
    QString name = c.value("cmd").toString();
    if (name.isEmpty()) return {{"error", "missing 'cmd'"}};

    if (name == "key")         return cmdKey(c);
    if (name == "text")        return cmdText(c);
    if (name == "click")       return cmdClick(c);
    if (name == "action")      return cmdAction(c);
    if (name == "state")       return cmdState(c);
    if (name == "pattern")     return cmdPattern(c);
    if (name == "instr")       return cmdInstr(c);
    if (name == "table")       return cmdTable(c);
    if (name == "order")       return cmdOrder(c);
    if (name == "screenshot")  return cmdScreenshot(c);
    if (name == "tick")        return cmdTick(c);
    if (name == "pause-timer") return cmdPauseTimer();
    if (name == "resume-timer")return cmdResumeTimer();
    if (name == "load")        return cmdLoad(c);
    if (name == "save")        return cmdSave(c);
    if (name == "eval")        return cmdEval(c);
    if (name == "quit")        return cmdQuit();
    return {{"error", QString("unknown cmd '%1'").arg(name)}};
}

// ---- Input ---------------------------------------------------------------

static int keysymForName(const QString &s) {
    QString u = s.toUpper();
    if (u == "RETURN" || u == "ENTER") return SDLK_RETURN;
    if (u == "ESC" || u == "ESCAPE")   return SDLK_ESCAPE;
    if (u == "SPACE")                  return SDLK_SPACE;
    if (u == "TAB")                    return SDLK_TAB;
    if (u == "BACKSPACE")              return SDLK_BACKSPACE;
    if (u == "INSERT")                 return SDLK_INSERT;
    if (u == "DELETE")                 return SDLK_DELETE;
    if (u == "UP")                     return SDLK_UP;
    if (u == "DOWN")                   return SDLK_DOWN;
    if (u == "LEFT")                   return SDLK_LEFT;
    if (u == "RIGHT")                  return SDLK_RIGHT;
    if (u == "HOME")                   return SDLK_HOME;
    if (u == "END")                    return SDLK_END;
    if (u == "PAGEUP")                 return SDLK_PAGEUP;
    if (u == "PAGEDOWN")               return SDLK_PAGEDOWN;
    if (u.startsWith("F") && u.length() <= 3) {
        int n = u.mid(1).toInt();
        if (n >= 1 && n <= 15) return SDLK_F1 + (n - 1);
    }
    if (s.length() == 1) {
        QChar ch = s.at(0);
        ushort uc = ch.unicode();
        if (uc < 128) return (int)ch.toLower().unicode();
    }
    return 0;
}

static Qt::Key qtKeyFor(int sym) {
    switch (sym) {
        case SDLK_RETURN: return Qt::Key_Return;
        case SDLK_ESCAPE: return Qt::Key_Escape;
        case SDLK_SPACE:  return Qt::Key_Space;
        case SDLK_TAB:    return Qt::Key_Tab;
        case SDLK_BACKSPACE: return Qt::Key_Backspace;
        case SDLK_INSERT: return Qt::Key_Insert;
        case SDLK_DELETE: return Qt::Key_Delete;
        case SDLK_UP:     return Qt::Key_Up;
        case SDLK_DOWN:   return Qt::Key_Down;
        case SDLK_LEFT:   return Qt::Key_Left;
        case SDLK_RIGHT:  return Qt::Key_Right;
        case SDLK_HOME:   return Qt::Key_Home;
        case SDLK_END:    return Qt::Key_End;
        case SDLK_PAGEUP: return Qt::Key_PageUp;
        case SDLK_PAGEDOWN: return Qt::Key_PageDown;
    }
    if (sym >= SDLK_F1 && sym <= SDLK_F15)
        return Qt::Key(Qt::Key_F1 + (sym - SDLK_F1));
    if (sym >= 'a' && sym <= 'z')
        return Qt::Key(Qt::Key_A + (sym - 'a'));
    if (sym >= '0' && sym <= '9')
        return Qt::Key(Qt::Key_0 + (sym - '0'));
    return Qt::Key_unknown;
}

QJsonObject Rpc::cmdKey(const QJsonObject &c) {
    int sym = c.value("sym").toInt(0);
    if (!sym) sym = keysymForName(c.value("keysym").toString());
    if (!sym) return {{"error", "key: provide 'keysym' or 'sym'"}};

    Qt::KeyboardModifiers mods = Qt::NoModifier;
    QJsonValue m = c.value("modifiers");
    bool shiftLike = false;
    if (m.isArray()) {
        for (auto v : m.toArray()) {
            QString s = v.toString().toLower();
            if (s == "shift") { mods |= Qt::ShiftModifier;   shiftLike = true; }
            else if (s == "ctrl" || s == "control") {
                mods |= Qt::ControlModifier; shiftLike = true;
            } else if (s == "alt")  mods |= Qt::AltModifier;
            else if (s == "meta")   mods |= Qt::MetaModifier;
        }
    }
    // In offscreen mode there's no focus widget. Default the target to the
    // currently visible editor widget so the keystroke reaches PatternView /
    // OrderView / etc. The caller can override with widget=<objectName>.
    QString w = c.value("widget").toString();
    QWidget *target = nullptr;
    if (!w.isEmpty()) target = mw_->findChild<QWidget*>(w);
    if (!target) target = QApplication::focusWidget();
    if (!target) target = mw_->activeEditorWidget();
    if (!target) target = mw_;
    target->setFocus(Qt::OtherFocusReason);

    Qt::Key qkey = qtKeyFor(sym);
    QString text;
    if (sym < 128 && sym >= 32 && (mods & Qt::ShiftModifier) == 0)
        text = QChar((char)sym);
    QKeyEvent ev(QEvent::KeyPress, qkey, mods, text);
    QApplication::sendEvent(target, &ev);
    QKeyEvent rev(QEvent::KeyRelease, qkey, mods, text);
    QApplication::sendEvent(target, &rev);
    (void)shiftLike;
    return {{"sent", sym}};
}

QJsonObject Rpc::cmdText(const QJsonObject &c) {
    QString t = c.value("value").toString();
    QWidget *focus = QApplication::focusWidget();
    if (auto *le = qobject_cast<QLineEdit*>(focus)) {
        le->setText(t);
        return {{"set", t}};
    }
    for (QChar ch : t) {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
        QApplication::sendEvent(focus ? focus : mw_, &ev);
    }
    return {{"typed", t}};
}

QJsonObject Rpc::cmdClick(const QJsonObject &c) {
    QString objName = c.value("widget").toString();
    if (objName.isEmpty()) return {{"error", "click: missing 'widget'"}};
    auto *w = mw_->findChild<QWidget*>(objName);
    if (!w) return {{"error", QString("widget '%1' not found").arg(objName)}};
    if (auto *btn = qobject_cast<QAbstractButton*>(w)) {
        btn->click();
        return {{"clicked", objName}};
    }
    return {{"error", QString("widget '%1' is not a button").arg(objName)}};
}

QJsonObject Rpc::cmdAction(const QJsonObject &c) {
    QString name = c.value("name").toString();
    if (name.isEmpty()) return {{"error", "action: missing 'name'"}};
    // Walk all QActions on mw_ and match by text() (with & ampersands stripped).
    const auto actions = mw_->findChildren<QAction*>();
    for (auto *a : actions) {
        QString t = a->text();
        t.remove('&');
        if (t.compare(name, Qt::CaseInsensitive) == 0) {
            a->trigger();
            return {{"triggered", t}};
        }
    }
    return {{"error", QString("action '%1' not found").arg(name)}};
}

// ---- State / inspection --------------------------------------------------

QJsonObject Rpc::cmdState(const QJsonObject &) {
    QJsonObject st;
    st["editmode"]   = editmode;
    st["isplaying"]  = isplaying() != 0;
    st["followplay"] = followplay;
    st["sidmodel"]   = (int)sidmodel;
    st["multiplier"] = (int)multiplier;
    st["ntsc"]       = (int)ntsc;
    st["songname"]   = QString::fromLocal8Bit(songname).trimmed();
    st["authorname"] = QString::fromLocal8Bit(authorname).trimmed();
    st["copyright"]  = QString::fromLocal8Bit(copyrightname).trimmed();
    st["esnum"]      = esnum;
    st["eschn"]      = eschn;
    st["einum"]      = einum;
    st["etnum"]      = etnum;
    st["etpos"]      = etpos;
    st["eppos"]      = eppos;
    st["epchn"]      = epchn;
    st["epcolumn"]   = epcolumn;
    st["epoctave"]   = epoctave;
    QJsonArray pn;
    for (int c = 0; c < MAX_CHN; c++) pn.append(epnum[c]);
    st["epnum"] = pn;
    QJsonArray sp;
    for (int c = 0; c < MAX_CHN; c++) sp.append(espos[c]);
    st["espos"] = sp;
    QJsonArray sl;
    for (int c = 0; c < MAX_CHN; c++) sl.append(songlen[esnum][c]);
    st["songlen_active"] = sl;
    QJsonArray pp;
    for (int c = 0; c < MAX_CHN; c++)
        pp.append(QJsonObject{{"ch", c},
                              {"pattnum", (int)chn[c].pattnum},
                              {"row", (int)(chn[c].pattptr / 4)},
                              {"songptr", (int)chn[c].songptr},
                              {"mute", (bool)chn[c].mute}});
    st["channels"] = pp;
    st["songfile"] = QString::fromLocal8Bit(songfilename);
    return {{"state", st}};
}

QJsonObject Rpc::cmdPattern(const QJsonObject &c) {
    int num = c.value("num").toInt(-1);
    if (num < 0) num = epnum[epchn];
    if (num < 0 || num >= MAX_PATT) return {{"error", "pattern out of range"}};
    int from = 0, to = pattlen[num];
    QJsonArray range = c.value("rows").toArray();
    if (range.size() == 2) { from = range[0].toInt(); to = range[1].toInt(); }
    if (from < 0) from = 0;
    if (to > pattlen[num]) to = pattlen[num];
    QJsonArray rows;
    for (int r = from; r < to; r++) {
        const unsigned char *cell = &pattern[num][r * 4];
        QJsonObject row{
            {"row", r},
            {"note", notenameFor(cell[0])},
            {"note_raw", QString("%1").arg(cell[0], 2, 16, QLatin1Char('0')).toUpper()},
            {"instr", cell[1]},
            {"cmd", QString("%1").arg(cell[2], 1, 16).toUpper()},
            {"param", QString("%1").arg(cell[3], 2, 16, QLatin1Char('0')).toUpper()},
        };
        rows.append(row);
    }
    return {{"pattern", QJsonObject{
        {"num", num},
        {"length", pattlen[num]},
        {"rows", rows}
    }}};
}

QJsonObject Rpc::cmdInstr(const QJsonObject &c) {
    int num = c.value("num").toInt(einum);
    if (num < 0 || num >= MAX_INSTR) return {{"error", "instr out of range"}};
    INSTR &ins = instr[num];
    char nameBuf[MAX_INSTRNAMELEN + 1];
    std::memcpy(nameBuf, ins.name, MAX_INSTRNAMELEN);
    nameBuf[MAX_INSTRNAMELEN] = 0;
    QJsonObject o{
        {"num", num},
        {"name", QString::fromLocal8Bit(nameBuf).trimmed()},
        {"ad", QString("%1").arg(ins.ad, 2, 16, QLatin1Char('0')).toUpper()},
        {"sr", QString("%1").arg(ins.sr, 2, 16, QLatin1Char('0')).toUpper()},
        {"wave_ptr",   ins.ptr[WTBL]},
        {"pulse_ptr",  ins.ptr[PTBL]},
        {"filter_ptr", ins.ptr[FTBL]},
        {"vib_param",  ins.ptr[STBL]},
        {"vib_delay",  ins.vibdelay},
        {"gate_timer", ins.gatetimer},
        {"first_wave", ins.firstwave},
    };
    return {{"instr", o}};
}

QJsonObject Rpc::cmdTable(const QJsonObject &c) {
    QString name = c.value("which").toString("wave").toLower();
    int t = etnum;
    if (name == "wave")   t = WTBL;
    else if (name == "pulse")  t = PTBL;
    else if (name == "filter") t = FTBL;
    else if (name == "speed")  t = STBL;
    QJsonArray rows;
    for (int r = 0; r < MAX_TABLELEN; r++) {
        rows.append(QJsonObject{
            {"row", r + 1},
            {"L", QString("%1").arg(ltable[t][r], 2, 16, QLatin1Char('0')).toUpper()},
            {"R", QString("%1").arg(rtable[t][r], 2, 16, QLatin1Char('0')).toUpper()},
        });
    }
    return {{"table", QJsonObject{{"which", name}, {"rows", rows}}}};
}

QJsonObject Rpc::cmdOrder(const QJsonObject &c) {
    int sub = c.value("subtune").toInt(esnum);
    if (sub < 0 || sub >= MAX_SONGS) return {{"error", "subtune out of range"}};
    QJsonObject result{{"subtune", sub}};
    for (int ch = 0; ch < MAX_CHN; ch++) {
        QJsonArray entries;
        int n = songlen[sub][ch];
        for (int i = 0; i <= n + 1; i++) {
            unsigned char v = songorder[sub][ch][i];
            QString s;
            if (v == LOOPSONG) s = "RST";
            else if (v >= TRANSUP)   s = QString("+%1").arg(v - TRANSUP, 1, 16).toUpper();
            else if (v >= TRANSDOWN) s = QString("-%1").arg(v - TRANSDOWN, 1, 16).toUpper();
            else if (v >= REPEAT)    s = QString("R%1").arg(v - REPEAT, 1, 16).toUpper();
            else s = QString("%1").arg(v, 2, 16, QLatin1Char('0')).toUpper();
            entries.append(s);
        }
        result.insert(QString("ch%1").arg(ch + 1), entries);
    }
    return {{"order", result}};
}

// ---- Visual / time-control ------------------------------------------------

QJsonObject Rpc::cmdScreenshot(const QJsonObject &c) {
    QString objName = c.value("widget").toString();
    QWidget *w = mw_;
    if (!objName.isEmpty()) {
        w = mw_->findChild<QWidget*>(objName);
        if (!w) return {{"error", QString("widget '%1' not found").arg(objName)}};
    }
    QPixmap pm = w->grab();
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    pm.save(&buf, "PNG");
    return {
        {"png_b64", QString::fromLatin1(bytes.toBase64())},
        {"width",   pm.width()},
        {"height",  pm.height()}
    };
}

QJsonObject Rpc::cmdTick(const QJsonObject &c) {
    int n = c.value("frames").toInt(1);
    for (int i = 0; i < n; i++) mw_->tickOnce();
    return {{"ticked", n}};
}

QJsonObject Rpc::cmdPauseTimer() {
    mw_->pauseTimer();
    return {{"paused", true}};
}
QJsonObject Rpc::cmdResumeTimer() {
    mw_->resumeTimer();
    return {{"resumed", true}};
}

QJsonObject Rpc::cmdLoad(const QJsonObject &c) {
    QString path = c.value("path").toString();
    if (path.isEmpty()) return {{"error", "load: missing 'path'"}};
    if (!QFile::exists(path)) return {{"error", QString("no file '%1'").arg(path)}};
    mw_->loadSongFile(path);
    return {{"loaded", path}};
}

QJsonObject Rpc::cmdSave(const QJsonObject &c) {
    QString path = c.value("path").toString();
    if (path.isEmpty()) return {{"error", "save: missing 'path'"}};
    QByteArray ba = path.toLocal8Bit();
    std::strncpy(songfilename, ba.constData(), MAX_FILENAME - 1);
    songfilename[MAX_FILENAME - 1] = 0;
    int rc = savesong();
    if (!rc) return {{"error", "savesong failed"}};
    return {{"saved", path}};
}

QJsonObject Rpc::cmdEval(const QJsonObject &c) {
    QString expr = c.value("expr").toString().trimmed();
    // Tiny domain-specific eval: `pattern[N][R][col]`, `instr[N].field`, etc.
    // For now support: pattern[N], pattern[N][R], pattlen[N], songlen[S][C],
    // songorder[S][C][R].
    if (expr.startsWith("pattlen[")) {
        int n = expr.mid(8, expr.indexOf(']') - 8).toInt();
        if (n < 0 || n >= MAX_PATT) return {{"error", "out of range"}};
        return {{"value", pattlen[n]}};
    }
    if (expr.startsWith("songorder[")) {
        QRegularExpression re("songorder\\[(\\d+)\\]\\[(\\d+)\\]\\[(\\d+)\\]");
        auto m = re.match(expr);
        if (!m.hasMatch()) return {{"error", "parse"}};
        int s = m.captured(1).toInt();
        int c = m.captured(2).toInt();
        int r = m.captured(3).toInt();
        if (s<0||s>=MAX_SONGS||c<0||c>=MAX_CHN||r<0||r>MAX_SONGLEN+1)
            return {{"error", "out of range"}};
        return {{"value", songorder[s][c][r]}};
    }
    if (expr.startsWith("pattern[")) {
        QRegularExpression re("pattern\\[(\\d+)\\]\\[(\\d+)\\]\\[(\\d+)\\]");
        auto m = re.match(expr);
        if (m.hasMatch()) {
            int n = m.captured(1).toInt();
            int r = m.captured(2).toInt();
            int col = m.captured(3).toInt();
            if (n<0||n>=MAX_PATT||r<0||r>=MAX_PATTROWS||col<0||col>=4)
                return {{"error", "out of range"}};
            return {{"value", pattern[n][r * 4 + col]}};
        }
    }
    return {{"error", QString("eval: unsupported expr '%1'").arg(expr)}};
}

QJsonObject Rpc::cmdQuit() {
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    return {{"quitting", true}};
}
