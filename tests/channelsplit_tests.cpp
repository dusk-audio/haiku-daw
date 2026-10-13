// Host tests for the kit-free half of multichannel input (M5):
//
//   - src/model/ChannelSplit.h — the channel ADDRESS math. What a bug looks
//     like here is a take reading another track's channel, a silent channel
//     where the device fanned one out, or a read past the end of the capture
//     block when the device turned out to have fewer channels than the address
//     asked for.
//
// (The batch take naming a capture needs — one file per armed track, picked in
// one pass — is covered in tests/takenames_tests.cpp.)
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/channelsplit_tests.cpp -o /tmp/cs && /tmp/cs
#include "../src/model/ChannelSplit.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    // --- PlanTake: the address a track's take reads ------------------------
    {
        InputSource in;                       // a fresh track's address
        in.kind = InputSource::kAudioDefault;
        const TakeSpec st = PlanTake("/x/take-1.wav", in, TrackChannels::Stereo,
                                     TakeFormat::Int24);
        CHECK(st.path == "/x/take-1.wav");
        CHECK(st.stereo);
        CHECK(st.srcL == 0 && st.srcR == 1);     // the pre-M5 first pair
        CHECK(st.bits == 24 && !st.float32);
        CHECK(TakeChannels(st) == 2);

        const TakeSpec mo = PlanTake("/x/take-2.wav", in, TrackChannels::Mono,
                                     TakeFormat::Int16);
        CHECK(!mo.stereo);
        CHECK(mo.srcL == 0);
        CHECK(mo.srcR == -1);                    // mono does not name a right
        CHECK(mo.bits == 16 && !mo.float32);
        CHECK(TakeChannels(mo) == 1);

        // A track addressed to the fifth channel of an interface: mono reads
        // exactly that channel.
        InputSource ch5;
        ch5.kind = InputSource::kAudioDefault;
        ch5.device = "/dev/audio/hmulti/0-0";
        ch5.channelL = 4;
        ch5.channelR = 5;
        const TakeSpec a = PlanTake("p", ch5, TrackChannels::Mono, TakeFormat::Int24);
        CHECK(a.srcL == 4 && a.srcR == -1);
        const TakeSpec b = PlanTake("p", ch5, TrackChannels::Stereo, TakeFormat::Int24);
        CHECK(b.srcL == 4 && b.srcR == 5);

        // A negative address (a hand-edited file) never survives to the block.
        InputSource neg = ch5;
        neg.channelL = -3;
        neg.channelR = -1;
        const TakeSpec n = PlanTake("p", neg, TrackChannels::Stereo, TakeFormat::Int24);
        CHECK(n.srcL == 0);
        CHECK(n.srcR == 0);   // R falls back to L rather than staying negative
    }

    // --- TakeFormat: the setting -> writer arguments ----------------------
    {
        CHECK(TakeFormatFromInt(0) == TakeFormat::Int16);
        CHECK(TakeFormatFromInt(1) == TakeFormat::Int24);
        CHECK(TakeFormatFromInt(2) == TakeFormat::Float32);
        CHECK(TakeFormatFromInt(99) == TakeFormat::Int24);   // unknown -> default
        CHECK(TakeFormatFromInt(-1) == TakeFormat::Int24);

        int bits = 0; bool fl = false;
        TakeFormatBits(TakeFormat::Int16, &bits, &fl);   CHECK(bits == 16 && !fl);
        TakeFormatBits(TakeFormat::Int24, &bits, &fl);   CHECK(bits == 24 && !fl);
        TakeFormatBits(TakeFormat::Float32, &bits, &fl); CHECK(bits == 32 && fl);
    }

    // --- ClampToDevice: the device shrank since the address was set --------
    {
        TakeSpec s;
        s.stereo = true; s.srcL = 6; s.srcR = 7;
        ClampToDevice(&s, 2);                    // a 2-channel device
        CHECK(s.srcL == 0);
        CHECK(s.srcR == 0);                      // both sides of channel 1
        // In range: untouched.
        TakeSpec in;
        in.stereo = true; in.srcL = 1; in.srcR = 3;
        ClampToDevice(&in, 4);
        CHECK(in.srcL == 1 && in.srcR == 3);
        // Right out of range but left fine: right follows left.
        TakeSpec half;
        half.stereo = true; half.srcL = 1; half.srcR = 9;
        ClampToDevice(&half, 2);
        CHECK(half.srcL == 1 && half.srcR == 1);
        // Unknown channel count (format not negotiated yet): leave it alone.
        TakeSpec unk;
        unk.stereo = true; unk.srcL = 5; unk.srcR = 9;
        ClampToDevice(&unk, 0);
        CHECK(unk.srcL == 5 && unk.srcR == 9);
        ClampToDevice(nullptr, 4);               // must not crash
        CHECK(true);
    }

    // --- Deinterleave: one take's samples out of the shared capture -------
    {
        // 4 device channels, 3 frames: frame f = {10+f, 20+f, 30+f, 40+f}.
        const float src[12] = { 10, 20, 30, 40,
                                11, 21, 31, 41,
                                12, 22, 32, 42 };
        float dst[8];

        // Mono take of device channel 3 (index 2).
        size_t n = Deinterleave(src, 3, 4, 2, -1, false, dst);
        CHECK(n == 3);
        CHECK(dst[0] == 30 && dst[1] == 31 && dst[2] == 32);

        // Stereo take of channels 4 and 2 (index 3 and 1) -- a SWAPPED pair:
        // the left channel is what the address says, not what the block order is.
        n = Deinterleave(src, 3, 4, 3, 1, true, dst);
        CHECK(n == 6);
        CHECK(dst[0] == 40 && dst[1] == 20);
        CHECK(dst[2] == 41 && dst[3] == 21);
        CHECK(dst[4] == 42 && dst[5] == 22);

        // A stereo take whose right channel is not on the device duplicates the
        // left one (a mono source fanned out, never a dead side).
        n = Deinterleave(src, 3, 4, 1, 9, true, dst);
        CHECK(n == 6);
        CHECK(dst[0] == 20 && dst[1] == 20 && dst[4] == 22 && dst[5] == 22);

        // An address that is not on the device at all reads SILENCE rather than
        // past the block: the capture block holds 4 channels, the address says 7.
        n = Deinterleave(src, 3, 4, 7, -1, false, dst);
        CHECK(n == 3);
        CHECK(dst[0] == 0.0f && dst[1] == 0.0f && dst[2] == 0.0f);

        // Degenerate arguments are no-ops, not crashes.
        CHECK(Deinterleave(nullptr, 3, 4, 0, 1, true, dst) == 0);
        CHECK(Deinterleave(src, 3, 4, 0, 1, true, nullptr) == 0);
        CHECK(Deinterleave(src, 3, 0, 0, 1, true, dst) == 0);
        CHECK(Deinterleave(src, 0, 4, 0, 1, true, dst) == 0);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
