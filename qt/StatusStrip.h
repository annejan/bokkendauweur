#pragma once
#include <QFrame>

class QLabel;
class ClickableLabel;

class StatusStrip : public QFrame {
    Q_OBJECT
public:
    explicit StatusStrip(QWidget *parent = nullptr);
    void refresh();
    void showMessage(const QString &t, int ms = 3000);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

signals:
    void sidClicked();
    void sid2Clicked();
    void followClicked();
    void tempoClicked();
    void ntscClicked();
    void octaveClicked();   // cycle up (wraps 7 -> 0)
    void octaveDelta(int);  // mouse wheel: ±1
    void recordClicked();   // status-strip click on REC -> toggle recordmode
    void skipClicked();     // status-strip click on SKIP -> cycle autoadvance 0..2

private:
    QLabel *transport_;
    QLabel *position_;
    ClickableLabel *tempo_;
    ClickableLabel *octave_;
    ClickableLabel *record_;
    ClickableLabel *skip_;
    QLabel *instr_;
    ClickableLabel *sid_;
    ClickableLabel *sid2_;
    ClickableLabel *ntsc_;
    ClickableLabel *follow_;
    QLabel *message_;
};
