#pragma once
#include <QString>

class QTextToSpeech;

// Self-voicing helper for accessibility. Wraps QTextToSpeech when the Qt
// TextToSpeech module is available at build time (GT2_HAVE_TTS). Without it
// every call is a no-op and available() returns false, so the editor builds
// and runs exactly as before — speech is purely additive.
class Speech {
public:
    enum class Priority { Cursor, Status };

    static Speech &instance();

    // Built with TextToSpeech support (the module was found at configure).
    bool compiledIn() const;
    // compiledIn() AND an engine actually initialised.
    bool available() const;

    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    // Priority::Cursor interrupts the current utterance so fast arrowing
    // stays responsive; Priority::Status is spoken after it.
    void say(const QString &text, Priority p = Priority::Cursor);

private:
    Speech();
    ~Speech();
    Speech(const Speech &) = delete;
    Speech &operator=(const Speech &) = delete;

    bool enabled_ = false;
    QTextToSpeech *tts_ = nullptr;  // null unless GT2_HAVE_TTS
};
