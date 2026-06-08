#pragma once
#include <QObject>
#include <QSocketNotifier>
#include <QJsonObject>

class MainWindow;
class QTimer;

// Newline-delimited JSON-RPC over stdin / stdout.
// Each input line is one command, each response line is one JSON object.
// Used by automated tests and agent-driven harnesses, so a key dispatch
// finishes in microseconds instead of waiting on a window manager.
class Rpc : public QObject {
    Q_OBJECT
public:
    explicit Rpc(MainWindow *mw, QObject *parent = nullptr);
    ~Rpc() override;

private slots:
    void onStdinReadable(int fd);

private:
    void handleCommand(const QJsonObject &cmd);
    QJsonObject dispatch(const QJsonObject &cmd);
    void reply(const QJsonObject &payload, const QJsonObject &cmd);
    void replyError(const QString &msg, const QJsonObject &cmd);

    QJsonObject cmdKey(const QJsonObject &c);
    QJsonObject cmdText(const QJsonObject &c);
    QJsonObject cmdClick(const QJsonObject &c);
    QJsonObject cmdAction(const QJsonObject &c);
    QJsonObject cmdState(const QJsonObject &c);
    QJsonObject cmdPattern(const QJsonObject &c);
    QJsonObject cmdInstr(const QJsonObject &c);
    QJsonObject cmdTable(const QJsonObject &c);
    QJsonObject cmdOrder(const QJsonObject &c);
    QJsonObject cmdScreenshot(const QJsonObject &c);
    QJsonObject cmdTick(const QJsonObject &c);
    QJsonObject cmdPauseTimer();
    QJsonObject cmdResumeTimer();
    QJsonObject cmdLoad(const QJsonObject &c);
    QJsonObject cmdSave(const QJsonObject &c);
    QJsonObject cmdEval(const QJsonObject &c);
    QJsonObject cmdQuit();

    MainWindow *mw_;
    QSocketNotifier *notifier_;
    QByteArray buf_;
};
