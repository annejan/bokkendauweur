#include "PaAudio.h"

#include <portaudio.h>

#include <QThread>
#include <QDebug>
#include <cstring>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#  include <pthread.h>
#  include <sched.h>
#  define GT2_HAVE_POSIX_SCHED 1
#endif

#include "CoreEvents.h"

extern "C" {
#include "gcommon.h"
#include "gplay.h"
#include "gsid.h"
int  sid_fillbuffer(short *ptr, int samples);
extern unsigned multiplier;
extern unsigned ntsc;
void stopsong(void);
void snd_lock(void);
void snd_unlock(void);
extern int songinit;
extern unsigned char sidreg[];
extern unsigned char sidreg2[];
extern CHN chn[];
}

PaAudio *PaAudio::self_ = nullptr;

// Promote the calling thread (the PortAudio callback thread) to a realtime
// scheduling policy. Any SCHED_FIFO priority preempts every normal
// SCHED_OTHER task, so even a modest value stops a busy desktop from
// starving the audio callback into an underrun. We pick a low-ish RT
// priority so we coexist with — rather than starve — the kernel's and the
// sound server's own RT threads. Best-effort: if the OS denies RT (no
// RLIMIT_RTPRIO / not in the audio group), we log a one-line hint and carry
// on at normal priority instead of failing playback.
static void raiseCallbackThreadPriority() {
#ifdef GT2_HAVE_POSIX_SCHED
    const int policy   = SCHED_FIFO;
    const int loPrio   = sched_get_priority_min(policy);
    const int hiPrio   = sched_get_priority_max(policy);
    if (loPrio < 0 || hiPrio < 0) return;
    // ~a quarter up the range: clearly realtime, but below the high
    // priorities a sound server (PipeWire/JACK) tends to claim.
    sched_param sp{};
    sp.sched_priority = loPrio + (hiPrio - loPrio) / 4;
    const int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc == 0) {
        qInfo("PaAudio: callback thread -> SCHED_FIFO prio %d", sp.sched_priority);
    } else {
        qWarning("PaAudio: could not raise callback thread to realtime "
                 "(%s). Audio may stutter under load. Grant RLIMIT_RTPRIO "
                 "(e.g. add your user to the 'audio' group, or set "
                 "rtprio in /etc/security/limits.conf).", std::strerror(rc));
    }
#endif
}

PaAudio::PaAudio() { self_ = this; }
PaAudio::~PaAudio() { stop(); if (self_ == this) self_ = nullptr; }

int PaAudio::paCallback(const void * /*in*/, void *out, unsigned long frames,
                        const PaStreamCallbackTimeInfo * /*t*/,
                        unsigned long /*flags*/, void *user) {
    auto *self = static_cast<PaAudio*>(user);
    short *o = static_cast<short*>(out);

    // First callback on this thread: promote it to realtime. exchange() makes
    // the one-time setup branch-free on every subsequent (hot) call, and the
    // syscall happens exactly once, off the steady-state path.
    if (!self->priorityRaised_.exchange(true, std::memory_order_relaxed))
        raiseCallbackThreadPriority();

    // Hard-fenced by the UI thread? Silence chunk + return — cheaper than
    // stalling the device. The fence is only set during the few ms a
    // sid_init / loadsong takes on the UI thread.
    if (self->fenced.load(std::memory_order_acquire)) {
        std::memset(o, 0, frames * sizeof(short));
        return 0; // paContinue
    }

    const int frameHz = (ntsc ? 60 : 50) * (multiplier ? multiplier : 1);
    const double samplesPerTickF = (double)self->sampleRate_ / (double)frameHz;

    unsigned long produced = 0;
    while (produced < frames) {
        if (self->sampleAccumF_ <= 0.0) {
            playroutine();
            // Record playback-state edges into lock-free atomic counters. This
            // does NO Qt work / alloc / locking — it must stay realtime-safe so
            // the audio callback never stalls. The GUI thread turns these into
            // signals in CoreEvents::deliver() (called from MainWindow's tick).
            if (auto *ev = CoreEvents::instance()) ev->pump();
            self->sampleAccumF_ += samplesPerTickF;
        }
        long chunk = (long)self->sampleAccumF_;
        if (chunk <= 0) chunk = 1;
        if (chunk > (long)(frames - produced)) chunk = (long)(frames - produced);
        int got = sid_fillbuffer(o + produced, (int)chunk);
        if (got <= 0) {
            short hold = (produced > 0) ? o[produced - 1] : 0;
            for (long i = 0; i < chunk; i++) o[produced + i] = hold;
            produced += chunk;
            self->sampleAccumF_ -= (double)chunk;
            continue;
        }
        produced += (unsigned long)got;
        self->sampleAccumF_ -= (double)got;
    }
    return 0;
}

bool PaAudio::start(int sampleRate) {
    sampleRate_ = sampleRate;

    PaError e = Pa_Initialize();
    if (e != paNoError) {
        qWarning("PaAudio: Pa_Initialize failed: %s", Pa_GetErrorText(e));
        return false;
    }
    paInited_ = true;

    // paFramesPerBufferUnspecified -> PortAudio picks the host-optimal frame
    // count, which on ALSA/Pulse is typically ~256-512 samples (~5-10 ms at
    // 44.1 kHz). That puts the editor playhead inside one C64 frame of the
    // audio, no compensation ring needed.
    e = Pa_OpenDefaultStream(&stream_,
                             0,            // no input
                             1,            // mono out
                             paInt16,
                             (double)sampleRate,
                             paFramesPerBufferUnspecified,
                             &PaAudio::paCallback,
                             this);
    if (e != paNoError) {
        qWarning("PaAudio: Pa_OpenDefaultStream failed: %s", Pa_GetErrorText(e));
        Pa_Terminate();
        paInited_ = false;
        return false;
    }

    e = Pa_StartStream(stream_);
    if (e != paNoError) {
        qWarning("PaAudio: Pa_StartStream failed: %s", Pa_GetErrorText(e));
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
        paInited_ = false;
        return false;
    }

    const PaStreamInfo *info = Pa_GetStreamInfo(stream_);
    if (info) {
        qInfo("PaAudio: started — rate=%g Hz, output latency=%.1f ms",
              info->sampleRate, info->outputLatency * 1000.0);
    }
    return true;
}

void PaAudio::stop() {
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    if (paInited_) {
        Pa_Terminate();
        paInited_ = false;
    }
}

AudioFence::AudioFence() {
    auto *a = PaAudio::instance();
    if (a) {
        a->fenced.store(true, std::memory_order_release);
        // Wait long enough for any in-flight PortAudio callback to drain.
        // Callback chunks are typically <10 ms at the default buffer size;
        // 5 ms covers the common case and is cheap if the audio thread is
        // already past the callback boundary.
        QThread::msleep(5);
    }
    // The BME/SDL audio backend ALSO clocks libresidfp on its own callback
    // thread (SDLAudioP1). Lock it too — fencing only PaAudio left the SDL
    // mixer racing the SID rebuild, crashing in reSIDfp::Filter::clock.
    snd_lock();
    stopsong();
    songinit = PLAY_STOPPED;

    // Force every voice off in the SID register shadow + clear gate bits on
    // chn[]. stopsong() asks playroutine() to do this transition the next
    // tick, but we set PLAY_STOPPED above before that tick can run — so
    // without this manual wipe the previous note's gate=1 + frequency stays
    // in sidreg[] and you can hear the note hang when the user switches SID
    // chip type mid-play.
    for (int v = 0; v < 3; v++) {
        int base = v * 7;
        sidreg [base + 0] = 0;        // freq lo
        sidreg [base + 1] = 0;        // freq hi
        sidreg [base + 2] = 0;        // pulse lo
        sidreg [base + 3] = 0;        // pulse hi
        sidreg [base + 4] = 0;        // ctrl  (gate=0 + waveform=0)
        sidreg [base + 5] = 0;        // AD
        sidreg [base + 6] = 0;        // SR
        sidreg2[base + 0] = 0;
        sidreg2[base + 1] = 0;
        sidreg2[base + 2] = 0;
        sidreg2[base + 3] = 0;
        sidreg2[base + 4] = 0;
        sidreg2[base + 5] = 0;
        sidreg2[base + 6] = 0;
    }
    sidreg [0x18] = 0; sidreg2[0x18] = 0;  // master volume = 0 so any
    sidreg [0x17] = 0; sidreg2[0x17] = 0;  // residual envelope is silent
    for (int c = 0; c < MAX_CHN; c++) chn[c].gate = 0xfe;
}
AudioFence::~AudioFence() {
    snd_unlock();
    auto *a = PaAudio::instance();
    if (a) a->fenced.store(false, std::memory_order_release);
}
