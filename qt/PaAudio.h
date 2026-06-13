#pragma once
#include <atomic>

typedef void PaStream;
struct PaStreamCallbackTimeInfo;

// PortAudio-backed audio output for goattrk2-qt. Replaces the previous
// QAudioSink + QIODevice pull-mode plumbing.
//
// Why PortAudio: Qt 6.4 (the LTS we target) only exposes the pull-mode
// QIODevice + QAudioSink::start(), which forced a ~160 ms sink buffer to
// keep PipeWire/Pulse from underrunning. That buffer in turn put the
// editor playhead ~160 ms behind what was being heard, and any UI-thread
// contention on the audio mutex produced audible ticks. PortAudio exposes
// a direct callback running at RT priority with ~10-20 ms latency on the
// same hosts, killing both problems at the source.
class PaAudio {
public:
    PaAudio();
    ~PaAudio();

    bool start(int sampleRate);
    void stop();

    // Atomic fence consulted from inside the PortAudio callback. While set,
    // the callback emits silence — used by AudioFence to bracket loadsong /
    // sid_init / clearsong against the audio thread without taking a mutex
    // on the hot path.
    std::atomic<bool> fenced{false};

    int  sampleRate() const { return sampleRate_; }
    static PaAudio *instance() { return self_; }

private:
    static int paCallback(const void *in, void *out, unsigned long frames,
                          const PaStreamCallbackTimeInfo *t,
                          unsigned long flags, void *user);

    PaStream *stream_     = nullptr;
    int       sampleRate_ = 0;
    double    sampleAccumF_ = 0.0;
    bool      paInited_ = false;
    // Set once, from the callback thread itself, the first time it runs — we
    // promote that thread to a realtime scheduling policy so a busy machine
    // can't preempt it into an underrun. PortAudio exposes no thread handle,
    // so self-promotion on first callback is the portable hook.
    std::atomic<bool> priorityRaised_{false};

    static PaAudio *self_;
};

// RAII silence-fence around any non-thread-safe rewrite of the C globals
// (loadsong, clearsong, sid_init, stereo / NTSC toggle). Sets the atomic
// flag the audio callback short-circuits on, sleeps long enough for an
// in-flight callback to drain, then hard-stops the playroutine.
class AudioFence {
public:
    AudioFence();
    ~AudioFence();
};
