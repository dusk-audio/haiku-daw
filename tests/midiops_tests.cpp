// MidiOps — the MIDI transform math: quantize (strength / swing / triplets /
// tempo changes), humanize determinism, legato, transpose, velocity, and the
// non-destructive-window rules every time-domain transform must honour.
#include "../src/model/MidiOps.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static MidiNote N(int pitch, int vel, Frame start, Frame len) {
    MidiNote n;
    n.pitch = pitch; n.velocity = vel;
    n.startFrame = start; n.lengthFrames = len;
    return n;
}
static NoteSel Sel(std::initializer_list<int> idx, size_t n) {
    NoteSel s(n, 0);
    for (int i : idx) s[(size_t)i] = 1;
    return s;
}
static bool Near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// The default map is 48000 Hz / 120 BPM: one beat is 24000 frames and the
// 16th-note grid is 6000 frames, so every expectation below is exact.
static void test_grid_steps() {
    std::printf("test_grid_steps\n");
    CHECK(Near(GridStepBeats(QuantGrid::Quarter), 1.0));
    CHECK(Near(GridStepBeats(QuantGrid::Eighth), 0.5));
    CHECK(Near(GridStepBeats(QuantGrid::Sixteenth), 0.25));
    CHECK(Near(GridStepBeats(QuantGrid::ThirtySecond), 0.125));
    CHECK(Near(GridStepBeats(QuantGrid::QuarterTriplet), 2.0 / 3.0));
    CHECK(Near(GridStepBeats(QuantGrid::EighthTriplet), 1.0 / 3.0));
    CHECK(Near(GridStepBeats(QuantGrid::SixteenthTriplet), 1.0 / 6.0));
    CHECK(std::string(GridName(QuantGrid::Sixteenth)) == "1/16");
    CHECK(std::string(GridName(QuantGrid::EighthTriplet)) == "1/8T");
}

static void test_quantize_strength() {
    std::printf("test_quantize_strength\n");
    TempoMap tm;   // 48 kHz, 120 BPM
    QuantizeOpts o;

    // Already on the grid: full-strength quantize is a no-op.
    std::vector<MidiNote> onGrid = { N(60, 100, 0, 1000), N(62, 100, 6000, 1000),
                                     N(64, 100, 12000, 1000) };
    CHECK(NotesEqual(Quantize(onGrid, {}, tm, 0, 1 << 20, o), onGrid));

    // Strength 1 pulls a note the whole way to the grid, strength 0 not at all.
    std::vector<MidiNote> off = { N(60, 100, 1000, 1000) };
    CHECK(NotesEqual(Quantize(off, {}, tm, 0, 1 << 20, o), onGrid) == false);
    o.strength = 0.0f;
    CHECK(NotesEqual(Quantize(off, {}, tm, 0, 1 << 20, o), off));
    o.strength = 1.0f;
    CHECK(Quantize(off, {}, tm, 0, 1 << 20, o)[0].startFrame == 0);

    // Strength 0.5 is the exact midpoint: 1000 frames -> 500 (the note is a
    // 24th of a beat in, and half of that is 500 frames).
    o.strength = 0.5f;
    CHECK(Quantize(off, {}, tm, 0, 1 << 20, o)[0].startFrame == 500);

    // Lengths are untouched unless asked for; a quantize of the start alone
    // must not resize a note.
    CHECK(Quantize(off, {}, tm, 0, 1 << 20, o)[0].lengthFrames == 1000);
    o.strength = 1.0f;
    o.quantizeLengths = true;
    // Note 1000..2000+1000: the end (2000) snaps down to 0 -- but a note can
    // never be shorter than one frame.
    std::vector<MidiNote> q = Quantize(off, {}, tm, 0, 1 << 20, o);
    CHECK(q[0].startFrame == 0);
    CHECK(q[0].lengthFrames == 1);
    // A note whose end is past the half-step point snaps up instead.
    std::vector<MidiNote> up = { N(60, 100, 1000, 6000) };   // end 7000 -> 6000
    CHECK(Quantize(up, {}, tm, 0, 1 << 20, o)[0].lengthFrames == 6000);
}

static void test_quantize_swing() {
    std::printf("test_quantize_swing\n");
    TempoMap tm;
    QuantizeOpts o;
    o.swingPct = 100.0f;
    // Slots 0 and 2 are on-beats; 1 and 3 are off-beats and must move.
    std::vector<MidiNote> notes = { N(60, 100, 0, 500), N(60, 100, 6000, 500),
                                    N(60, 100, 12000, 500), N(60, 100, 18000, 500) };
    std::vector<MidiNote> q = Quantize(notes, {}, tm, 0, 1 << 20, o);
    CHECK(q[0].startFrame == 0);        // even slot: untouched
    CHECK(q[1].startFrame == 8000);     // odd slot: +1/3 of the step (triplet feel)
    CHECK(q[2].startFrame == 12000);    // even slot: untouched
    CHECK(q[3].startFrame == 20000);    // odd slot: 18000 -> 20000
    // Half swing is half the displacement.
    o.swingPct = 50.0f;
    CHECK(Quantize(notes, {}, tm, 0, 1 << 20, o)[1].startFrame == 7000);
    // Straight is straight.
    o.swingPct = 0.0f;
    CHECK(Quantize(notes, {}, tm, 0, 1 << 20, o)[1].startFrame == 6000);
    // Swing displaces only the START: an end is never swung.
    o.swingPct = 100.0f;
    o.quantizeLengths = true;
    std::vector<MidiNote> one = { N(60, 100, 6000, 6000) };   // 6000..12000
    std::vector<MidiNote> sq = Quantize(one, {}, tm, 0, 1 << 20, o);
    CHECK(sq[0].startFrame == 8000);            // swung
    CHECK(sq[0].startFrame + sq[0].lengthFrames == 12000);   // end unswung
}

static void test_quantize_triplets() {
    std::printf("test_quantize_triplets\n");
    TempoMap tm;
    QuantizeOpts o;
    o.grid = QuantGrid::EighthTriplet;   // 1/3 beat = 8000 frames
    std::vector<MidiNote> notes = { N(60, 100, 7900, 100), N(60, 100, 8100, 100),
                                    N(60, 100, 16000, 100) };
    std::vector<MidiNote> q = Quantize(notes, {}, tm, 0, 1 << 20, o);
    CHECK(q[0].startFrame == 8000);
    CHECK(q[1].startFrame == 8000);
    CHECK(q[2].startFrame == 16000);
    o.grid = QuantGrid::SixteenthTriplet;   // 1/6 beat = 4000 frames
    std::vector<MidiNote> n16 = { N(60, 100, 4100, 100), N(60, 100, 3900, 100) };
    std::vector<MidiNote> q16 = Quantize(n16, {}, tm, 0, 1 << 20, o);
    CHECK(q16[0].startFrame == 4000);
    CHECK(q16[1].startFrame == 4000);
}

static void test_quantize_tempo_change() {
    std::printf("test_quantize_tempo_change\n");
    // 120 BPM to a hard 240 at frame 96000 (= 4 beats), so the 16th grid
    // changes from 6000 to 3000 frames exactly at the change.
    TempoMap tm;
    tm.SetTempoAt(96000, 240.0);
    QuantizeOpts o;
    std::vector<MidiNote> notes = { N(60, 100, 97000, 100), N(60, 100, 98000, 100) };
    std::vector<MidiNote> q = Quantize(notes, {}, tm, 0, 1 << 20, o);
    CHECK(q[0].startFrame == 96000);   // nearest post-change grid line
    CHECK(q[1].startFrame == 99000);   // one 3000-frame step later

    // A RAMP: 120 -> 240 across [0, 96000), six beats in total. Snapping must
    // still land on a beat, even though a fixed frame step would not.
    TempoMap ramp;
    ramp.SetTempoAt(0, 120.0, true);
    ramp.SetTempoAt(96000, 240.0);
    QuantizeOpts quarter;
    quarter.grid = QuantGrid::Quarter;
    std::vector<MidiNote> rn = { N(60, 100, 50000, 100) };
    std::vector<MidiNote> rq = Quantize(rn, {}, ramp, 0, 1 << 20, quarter);
    CHECK(rq[0].startFrame != 50000);   // it moved somewhere on the grid
    // ...and the beat it landed on is a whole beat (within a frame's worth).
    CHECK(Near(ramp.BeatAt(rq[0].startFrame),
               std::llround(ramp.BeatAt(50000)), 1e-4));
    // The same note quantized twice is the same note: snapped is snapped.
    CHECK(NotesEqual(Quantize(rq, {}, ramp, 0, 1 << 20, quarter), rq));

    // Mid-clip tempo change with a clip that does not start at 0: the grid is
    // still the song's, not the clip's.
    std::vector<MidiNote> cn = { N(60, 100, 1000, 100) };   // abs 97000
    std::vector<MidiNote> cq = Quantize(cn, {}, tm, 96000, 1 << 20, o);
    CHECK(cq[0].startFrame == 0);       // abs 96000 == the change frame, on grid
}

static void test_quantize_window() {
    std::printf("test_quantize_window\n");
    TempoMap tm;
    QuantizeOpts o;
    const Frame kLen = 10000;

    // A note outside the window is kept-but-silent content: a transform must
    // not pull it in (it would suddenly sound) or push it further out.
    std::vector<MidiNote> out = { N(60, 100, -500, 100), N(60, 100, 12000, 100) };
    CHECK(NotesEqual(Quantize(out, {}, tm, 0, kLen, o), out));

    // A note inside can never be pushed out by the snap.
    std::vector<MidiNote> edge = { N(60, 100, 9900, 100) };
    std::vector<MidiNote> eq = Quantize(edge, {}, tm, 0, kLen, o);
    CHECK(eq[0].startFrame >= 0 && eq[0].startFrame < kLen);

    // ...and a note that was fully inside cannot grow the region: the end is
    // capped at the window, which is also what the engine plays anyway.
    std::vector<MidiNote> tail = { N(60, 100, 4100, 5000) };   // start -> 6000
    std::vector<MidiNote> tq = Quantize(tail, {}, tm, 0, kLen, o);
    CHECK(tq[0].startFrame == 6000);
    CHECK(tq[0].startFrame + tq[0].lengthFrames <= kLen);

    // Selection: an unselected note is byte-identical, even when off-window.
    std::vector<MidiNote> many = { N(60, 100, 1000, 100), N(62, 100, 2000, 100) };
    std::vector<MidiNote> sel = Quantize(many, Sel({1}, many.size()), tm, 0, 1 << 20, o);
    CHECK(sel[0].startFrame == 1000);
    CHECK(sel[1].startFrame == 0);
}

static void test_humanize() {
    std::printf("test_humanize\n");
    const Frame kLen = 1 << 20;
    std::vector<MidiNote> notes = { N(60, 100, 10000, 500), N(64, 60, 20000, 500),
                                    N(67, 127, 30000, 500) };

    // Same seed, same notes -> same result, always.
    std::vector<MidiNote> a = Humanize(notes, {}, kLen, 500, 20, 0xC0FFEE);
    std::vector<MidiNote> b = Humanize(notes, {}, kLen, 500, 20, 0xC0FFEE);
    CHECK(NotesEqual(a, b));
    CHECK(!NotesEqual(a, notes));                 // it did something
    // A different seed is a different take.
    std::vector<MidiNote> c = Humanize(notes, {}, kLen, 500, 20, 0xBADF00D);
    CHECK(!NotesEqual(a, c));

    // Zero jitter is a no-op.
    CHECK(NotesEqual(Humanize(notes, {}, kLen, 0, 0, 7), notes));

    // Every note moves by at most the jitter asked for.
    for (size_t i = 0; i < notes.size(); i++) {
        CHECK(std::llabs(a[i].startFrame - notes[i].startFrame) <= 500);
        CHECK(std::abs(a[i].velocity - notes[i].velocity) <= 20);
    }

    // Velocity clamps at both ends, whatever the draw.
    std::vector<MidiNote> quiet = { N(60, 1, 1000, 100) };
    std::vector<MidiNote> loud   = { N(60, 127, 1000, 100) };
    for (uint64_t seed = 1; seed <= 8; seed++) {
        CHECK(Humanize(quiet, {}, kLen, 0, 60, seed)[0].velocity >= 1);
        CHECK(Humanize(loud,  {}, kLen, 0, 60, seed)[0].velocity <= 127);
    }

    // A start can never be pushed before the region.
    std::vector<MidiNote> near0 = { N(60, 100, 10, 100) };
    for (uint64_t seed = 1; seed <= 8; seed++) {
        const Frame s = Humanize(near0, {}, kLen, 500, 0, seed)[0].startFrame;
        CHECK(s >= 0 && s < kLen);
    }

    // Two notes at the same frame and pitch still move independently (the draw
    // hashes the index too) — pinned for this seed, and deterministic.
    std::vector<MidiNote> twins = { N(60, 100, 5000, 100), N(60, 100, 5000, 100) };
    std::vector<MidiNote> t = Humanize(twins, {}, kLen, 500, 20, 0xC0FFEE);
    CHECK(t[0].startFrame != t[1].startFrame || t[0].velocity != t[1].velocity);

    // Selection only.
    std::vector<MidiNote> h = Humanize(notes, Sel({0}, notes.size()), kLen, 500, 20, 5);
    CHECK(h[1].startFrame == notes[1].startFrame && h[2].pitch == notes[2].pitch);
    CHECK(h[1].velocity == notes[1].velocity);

    // Out-of-window content is not touched by a time transform.
    std::vector<MidiNote> outw = { N(60, 100, 20000, 100) };
    CHECK(NotesEqual(Humanize(outw, {}, 10000, 500, 20, 3), outw));
}

static void test_legato() {
    std::printf("test_legato\n");
    const Frame kLen = 1 << 20;
    // A chain: each note extends to the next one's start, whatever its pitch.
    std::vector<MidiNote> chain = { N(60, 100, 0, 500), N(64, 100, 1000, 500),
                                    N(67, 100, 2000, 500) };
    std::vector<MidiNote> l = Legato(chain, {}, kLen);
    CHECK(l[0].lengthFrames == 1000);
    CHECK(l[1].lengthFrames == 1000);
    CHECK(l[2].lengthFrames == 500);      // the last note keeps its length
    // Idempotent: extended is extended.
    CHECK(NotesEqual(Legato(l, {}, kLen), l));
    // Extend-only: a note already overlapping the next is left alone.
    std::vector<MidiNote> over = { N(60, 100, 0, 5000), N(64, 100, 1000, 100) };
    CHECK(Legato(over, {}, kLen)[0].lengthFrames == 5000);
    // Two notes on the same frame cannot make a zero-length note.
    std::vector<MidiNote> same = { N(60, 100, 1000, 100), N(64, 100, 1000, 100) };
    CHECK(Legato(same, {}, kLen)[0].lengthFrames == 100);
    // A legato extension never grows the region: it is capped at the window.
    std::vector<MidiNote> past = { N(60, 100, 100, 100), N(64, 100, 1600, 100) };
    std::vector<MidiNote> lp = Legato(past, {}, 1500);
    CHECK(lp[0].startFrame + lp[0].lengthFrames <= 1500);
    // Selection only.
    std::vector<MidiNote> one = Legato(chain, Sel({2}, chain.size()), kLen);
    CHECK(one[0].lengthFrames == 500 && one[1].lengthFrames == 500);
    // Notes the window excludes are kept-but-silent and a time transform must
    // not touch them at all -- not their start, not their length.
    std::vector<MidiNote> outw = { N(60, 100, 3000, 100), N(64, 100, 4000, 100) };
    CHECK(NotesEqual(Legato(outw, {}, 1500), outw));
}

static void test_pitch_and_velocity() {
    std::printf("test_pitch_and_velocity\n");
    std::vector<MidiNote> notes = { N(60, 100, 0, 100), N(120, 1, 1000, 100) };
    std::vector<MidiNote> up = TransposeSemitones(notes, {}, 12);
    CHECK(up[0].pitch == 72);
    CHECK(up[1].pitch == 127);            // clamped, not wrapped
    CHECK(TransposeSemitones(notes, {}, -120)[0].pitch == 0);
    CHECK(TransposeSemitones(notes, Sel({0}, 2), 12)[1].pitch == 120);

    std::vector<MidiNote> v = ScaleVelocity(notes, {}, 1.0f, 10.0f);
    CHECK(v[0].velocity == 110);
    CHECK(v[1].velocity == 11);
    CHECK(ScaleVelocity(notes, {}, 2.0f, 0.0f)[1].velocity == 2);
    CHECK(ScaleVelocity(notes, {}, 0.0f, -100.0f)[0].velocity == 1);   // floor
    CHECK(ScaleVelocity(notes, {}, 10.0f, 0.0f)[0].velocity == 127);   // ceiling
    CHECK(ScaleVelocity(notes, Sel({1}, 2), 1.0f, 10.0f)[0].velocity == 100);
}

int main() {
    test_grid_steps();
    test_quantize_strength();
    test_quantize_swing();
    test_quantize_triplets();
    test_quantize_tempo_change();
    test_quantize_window();
    test_humanize();
    test_legato();
    test_pitch_and_velocity();

    std::printf("\nmidiops_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
