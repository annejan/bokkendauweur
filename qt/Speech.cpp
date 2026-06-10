#include "Speech.h"

#ifdef GT2_HAVE_TTS
#include <QTextToSpeech>
#endif

Speech &Speech::instance() {
    static Speech s;
    return s;
}

Speech::Speech() {
#ifdef GT2_HAVE_TTS
    tts_ = new QTextToSpeech();
    if (tts_->state() != QTextToSpeech::Error) {
        // Tracker users arrow quickly — a touch faster than the default
        // reads cells without lagging behind the cursor.
        tts_->setRate(0.3);
    }
#endif
}

Speech::~Speech() {
#ifdef GT2_HAVE_TTS
    delete tts_;
#endif
}

bool Speech::compiledIn() const {
#ifdef GT2_HAVE_TTS
    return true;
#else
    return false;
#endif
}

bool Speech::available() const {
#ifdef GT2_HAVE_TTS
    return tts_ && tts_->state() != QTextToSpeech::Error;
#else
    return false;
#endif
}

void Speech::setEnabled(bool on) {
    enabled_ = on;
}

void Speech::say(const QString &text, Priority p) {
    if (!enabled_ || text.isEmpty()) return;
#ifdef GT2_HAVE_TTS
    if (!tts_) return;
    if (p == Priority::Cursor) tts_->stop();  // interrupt for snappy cursor
    tts_->say(text);
#else
    Q_UNUSED(text);
    Q_UNUSED(p);
#endif
}
