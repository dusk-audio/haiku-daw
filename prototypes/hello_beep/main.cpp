// hello_beep — Milestone 0 for the Haiku DAW.
//
// Proves three things before we build anything real:
//   1. The Haiku toolchain compiles and links against the Media Kit.
//   2. BSoundPlayer can open the default audio output and pull buffers.
//   3. We can observe latency / glitching on this machine (VM vs metal).
//
// It plays a 440 Hz sine for a few seconds, printing the negotiated
// format and the reported output latency, then exits cleanly.
//
// Build: see CMakeLists.txt  (or the one-liner in the README).

#include <SoundPlayer.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>   // snooze()

// State shared with the real-time buffer callback. Kept trivially simple:
// the callback only reads sampleRate/frequency and advances phase. No
// allocation, no locks, no I/O in the callback — the rule that will govern
// the whole engine later.
struct SineState {
    float  frequency;   // Hz
    float  sampleRate;  // frames per second
    double phase;       // radians, carried across buffers to avoid clicks
    float  amplitude;   // 0..1
};

// The BSoundPlayer callback. `buffer` is `size` bytes of interleaved
// float stereo (we request that format below). Fill it and return.
static void FillBuffer(void* cookie, void* buffer, size_t size,
                       const media_raw_audio_format& format)
{
    SineState* s = static_cast<SineState*>(cookie);

    const int channels = format.channel_count;              // expect 2
    const size_t frames = size / (sizeof(float) * channels);
    float* out = static_cast<float*>(buffer);

    const double step = 2.0 * M_PI * s->frequency / s->sampleRate;

    for (size_t f = 0; f < frames; f++) {
        const float sample = s->amplitude * (float)sin(s->phase);
        s->phase += step;
        if (s->phase >= 2.0 * M_PI)
            s->phase -= 2.0 * M_PI;

        for (int c = 0; c < channels; c++)
            *out++ = sample;   // same sample to every channel
    }
}

int main()
{
    // Ask for 48 kHz float stereo. BSoundPlayer negotiates with the
    // media_server; the granted format comes back via Format().
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate   = 48000;
    format.channel_count = 2;
    format.format       = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order   = B_MEDIA_HOST_ENDIAN;
    // buffer_size left as wildcard -> let the server choose a good size.

    SineState state;
    state.frequency  = 440.0f;
    state.sampleRate = format.frame_rate;
    state.phase      = 0.0;
    state.amplitude  = 0.25f;   // -12 dB-ish, don't blast the ears

    BSoundPlayer player(&format, "hello_beep", FillBuffer,
                        /*notifier*/ NULL, /*cookie*/ &state);

    status_t err = player.InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "BSoundPlayer InitCheck failed: %s\n", strerror(err));
        return 1;
    }

    // Report what we actually got — sampleRate may differ from requested.
    const media_raw_audio_format& got = player.Format();
    state.sampleRate = got.frame_rate;   // keep the callback in sync

    printf("Negotiated format:\n");
    printf("  frame_rate   = %.1f Hz\n", got.frame_rate);
    printf("  channels     = %d\n", got.channel_count);
    printf("  buffer_size  = %zu bytes\n", (size_t)got.buffer_size);
    printf("  latency      = %lld us (%.2f ms)\n",
           (long long)player.Latency(), player.Latency() / 1000.0);

    printf("Playing 440 Hz for 3 seconds...\n");
    player.SetHasData(true);
    player.Start();

    snooze(3 * 1000 * 1000);   // microseconds

    player.Stop();
    printf("Done.\n");
    return 0;
}
