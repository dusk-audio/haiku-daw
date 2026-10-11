// Host-buildable tests for MIDI expression: pitch bend (integrated, not a
// per-note retune), the mod wheel's vibrato, and the CC64 sustain policy in
// both its forms — the live latch and the offline application to a region.
//
// The headline property is the bend: a voice's phase must move by the INTEGRAL
// of the bend ratio, so a bend step never jumps the phase (which would click)
// and a note that begins while a bend is already in force starts at its own
// pitch. The reference below walks the phase sample by sample and compares
// against the real Synth, so an error in any of the terms shows up as a
// mismatch rather than as "the sound changed".

#include "../src/synth/Synth.h"
#include "../src/model/MidiExpression.h"
#include "../src/model/Sustain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const double SR = 48000.0;

static MidiNote Note(int pitch, int vel, Frame start, Frame len) {
    MidiNote n;
    n.pitch = pitch; n.velocity = vel;
    n.startFrame = start; n.lengthFrames = len;
    return n;
}

static MidiClipEvent Cc(Frame at, int cc, int v) {
    MidiClipEvent e;
    e.type = MidiClipEvent::CC; e.startFrame = at; e.data = cc; e.value = v;
    return e;
}

static MidiClipEvent Bend(Frame at, int v14) {
    MidiClipEvent e;
    e.type = MidiClipEvent::PitchBend; e.startFrame = at; e.data = 0; e.value = v14;
    return e;
}

// A steady tone (no decay/release), so the comparison below is about the phase
// and nothing else.
static Instrument Tone() {
    Instrument inst;
    inst.waveform = (int)Waveform::Sine;
    inst.attack = 0.001f; inst.decay = 0.0f;
    inst.sustain = 1.0f;  inst.release = 0.0f;
    return inst;
}

// The ADSR the Synth uses, copied here so the reference below is a reference of
// the *expression*, not of the envelope.
static double Env(double rel, double noteLen, double a, double d, double s, double r) {
    auto ads = [&](double x) -> double {
        if (a > 0.0 && x < a)     return x / a;
        if (d > 0.0 && x < a + d) return 1.0 - (1.0 - s) * (x - a) / d;
        return s;
    };
    if (rel < noteLen) return ads(rel);
    if (r <= 0.0) return 0.0;
    const double off = ads(noteLen);
    const double t = (rel - noteLen) / r;
    return t >= 1.0 ? 0.0 : off * (1.0 - t);
}

static double NoteFreq(int pitch) {
    return 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
}

// Render `notes` in `blockSize` blocks through the real Synth, feeding each
// block the expression the model helpers produce for it — the engine's path.
static std::vector<float> RenderBlocks(const std::vector<MidiNote>& notes,
                                       const Instrument& inst,
                                       const std::vector<MidiClipEvent>& events,
                                       Frame total, size_t blockSize,
                                       const std::vector<BendPoint>& tl) {
    Synth synth(SR);
    std::vector<float> out((size_t)total * 2, 0.0f);
    for (Frame off = 0; off < total; off += (Frame)blockSize) {
        const size_t n = (size_t)std::min<Frame>((Frame)blockSize, total - off);
        synth.Render(notes, inst, out.data() + (size_t)off * 2, n, off,
                     StereoGain{1.0f, 1.0f}, StereoGain{1.0f, 1.0f},
                     ExpressionAt(tl, events, off, off + (Frame)n));
    }
    return out;
}

// The same span, rendered by walking the phase sample by sample: the interval
// [g, g+1) advances by the ratio the block containing g carries — measured here
// from the same Δ the render is handed, but the WALK is the test's own, so this
// agrees with the Synth only if its integration is right. The vibrato is added
// per frame by the trapezoid rule (an independent quadrature of the same
// instantaneous ratio the Synth integrates in closed form).
static std::vector<float> Reference(const std::vector<MidiNote>& notes,
                                    const Instrument& inst,
                                    const std::vector<MidiClipEvent>& events,
                                    Frame total, size_t blockSize,
                                    const std::vector<BendPoint>& tl) {
    std::vector<float> out((size_t)total * 2, 0.0f);
    const double a = inst.attack * SR, d = inst.decay * SR;
    const double s = inst.sustain, r = inst.release * SR;
    const double vibW = 2.0 * M_PI * kModWheelVibratoHz / SR;
    const double vibA = (ModWheelAt(events, 0) / 127.0) *
                        (std::pow(2.0, kModWheelVibratoSemis / 12.0) - 1.0);

    for (const MidiNote& nt : notes) {
        const double base = NoteFreq(nt.pitch) / SR;
        const Frame stop = std::min<Frame>(nt.startFrame + nt.lengthFrames
                                           + (Frame)r, total);
        double phase = 0.0;                 // cycles, walked one frame at a time
        for (Frame g = nt.startFrame; g < stop; ) {
            const Frame b0 = (g / (Frame)blockSize) * (Frame)blockSize;
            const Frame b1 = b0 + (Frame)blockSize;
            const double bend = 1.0 + (BendPhaseAt(tl, b1) - BendPhaseAt(tl, b0))
                                    / (double)blockSize;
            for (; g < b1 && g < stop; g++) {
                const double rel = (double)(g - nt.startFrame);
                const double env = Env(rel, (double)nt.lengthFrames, a, d, s, r);
                const float smp = (float)(std::sin(2.0 * M_PI * phase) * env)
                                * (nt.velocity / 127.0f * 0.2f);
                out[(size_t)g * 2 + 0] += smp;
                out[(size_t)g * 2 + 1] += smp;
                const double v0 = vibA * std::sin(vibW * rel);
                const double v1 = vibA * std::sin(vibW * (rel + 1.0));
                phase += (bend + 0.5 * (v0 + v1)) * base;
            }
        }
    }
    return out;
}

static double MaxDiff(const std::vector<float>& a, const std::vector<float>& b,
                      size_t* atIndex = nullptr) {
    double m = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const double d = std::fabs((double)a[i] - (double)b[i]);
        if (d > m) { m = d; if (atIndex) *atIndex = i / 2; }
    }
    return m;
}

// Drive the live latch over a note list + CC64 events the way the engine does
// (a note-off is offered to the latch; what it defers it releases on the lift,
// or on a re-strike of the same key), and return each note's release frame.
// Used to prove the live and offline forms of the policy agree.
static std::vector<Frame> LatchReleases(const std::vector<MidiNote>& notes,
                                        const std::vector<MidiClipEvent>& events) {
    struct Ev { Frame at; int kind; size_t note; };   // kind 0=on 1=off 2=pedal
    std::vector<Ev> script;
    for (size_t i = 0; i < notes.size(); i++) {
        script.push_back({ notes[i].startFrame, 0, i });
        script.push_back({ notes[i].startFrame + notes[i].lengthFrames, 1, i });
    }
    for (const MidiClipEvent& e : events)
        if (e.type == MidiClipEvent::CC && e.data == 64)
            script.push_back({ e.startFrame, 2, (size_t)e.value });
    std::stable_sort(script.begin(), script.end(),
                     [](const Ev& a, const Ev& b) { return a.at < b.at; });

    SustainLatch latch;
    std::vector<Frame> end(notes.size(), -1);
    std::vector<size_t> held;            // notes the pedal is holding
    for (const Ev& e : script) {
        if (e.kind == 2) {
            if (latch.SetValue((int)e.note)) {
                for (size_t i : held) end[i] = e.at;   // the lift releases them
                held.clear();
            }
            continue;
        }
        const int key = notes[e.note].pitch;           // no channels in the model
        if (e.kind == 0) {                             // note-on
            for (size_t j = 0; j < held.size(); j++)
                if (notes[held[j]].pitch == key) {     // a re-strike cuts the hold
                    end[held[j]] = e.at;
                    held.erase(held.begin() + (long)j);
                    break;
                }
            latch.Retrigger(key);
        } else {                                       // note-off
            if (latch.DeferNoteOff(key)) held.push_back(e.note);
            else                         end[e.note] = e.at;
        }
    }
    for (size_t i = 0; i < notes.size(); i++)
        if (end[i] < 0) end[i] = notes[i].startFrame + notes[i].lengthFrames;
    return end;
}

int main() {
    Instrument inst = Tone();

    // ---- bend: ratio, timeline, integral ---------------------------------
    {
        CHECK(std::fabs(BendRatio(8192) - 1.0f) < 1e-6f);          // centre
        const double up = std::pow(2.0, kBendRangeSemitones / 12.0);
        CHECK(std::fabs(BendRatio(16383) - up) < 1e-4f);           // max
        CHECK(std::fabs(BendRatio(0) - 1.0 / up) < 1e-4f);         // min
        CHECK(BendRatio(99999) == BendRatio(16383));               // clamps

        std::vector<MidiClipEvent> ev = { Bend(100, 16383), Bend(300, 8192) };
        const std::vector<BendPoint> tl = BuildBendTimeline(ev);
        // 16383 is 8191/8192 of the range, so the ratio is a hair under 2^(2/12).
        const double rUp = (double)BendRatio(16383);
        CHECK(rUp < up && rUp > up * 0.9999);
        CHECK(tl.size() == 2);
        CHECK(BendRatioAt(tl, 0) == 1.0f);                         // before any
        CHECK(std::fabs(BendRatioAt(tl, 100) - rUp) < 1e-6f);      // at the event
        CHECK(std::fabs(BendRatioAt(tl, 299) - rUp) < 1e-6f);      // holds
        CHECK(std::fabs(BendRatioAt(tl, 9999) - 1.0f) < 1e-6f);    // back to centre
        // Δ is the area under (ratio - 1).
        CHECK(std::fabs(BendPhaseAt(tl, 50)) < 1e-9);              // nothing yet
        CHECK(std::fabs(BendPhaseAt(tl, 100)) < 1e-9);
        CHECK(std::fabs(BendPhaseAt(tl, 200) - 100.0 * (rUp - 1.0)) < 1e-6);
        CHECK(std::fabs(BendPhaseAt(tl, 500) - 200.0 * (rUp - 1.0)) < 1e-6);
    }

    // ---- the phase is the bend's INTEGRAL, across a bend step -------------
    {
        // A note that starts before the bend, and one that starts after a bend
        // has been in force for a while. Both are rendered in blocks, so the
        // ratio steps inside each note's life.
        const std::vector<MidiClipEvent> ev = { Bend(3000, 16383),
                                                Bend(9000, 8192) };
        const std::vector<BendPoint> tl = BuildBendTimeline(ev);
        Instrument longInst = Tone();
        longInst.attack = 0.005f;
        std::vector<MidiNote> notes = { Note(69, 100, 0, 14000),
                                        Note(72, 100, 6000, 6000) };
        AnnotateBendPhase(notes, tl);
        CHECK(notes[0].bendPhaseFrames == 0.0);                    // starts flat
        const double rUp = (double)BendRatio(16383);
        CHECK(std::fabs(notes[1].bendPhaseFrames - 3000.0 * (rUp - 1.0)) < 1e-6);

        for (size_t bs : { (size_t)512, (size_t)4096, (size_t)111 }) {
            const std::vector<float> got = RenderBlocks(notes, longInst, ev,
                                                        20000, bs, tl);
            const std::vector<float> want = Reference(notes, longInst, ev,
                                                      20000, bs, tl);
            size_t at = 0;
            const double diff = MaxDiff(got, want, &at);
            if (diff >= 1e-4)
                std::printf("  bend diff %g at frame %zu (block %zu)\n",
                            diff, at, bs);
            CHECK(diff < 1e-4);
        }

        // A centre bend is not "a bend": the render is bit-identical to one with
        // no events at all, so expression costs nothing until it is used.
        const std::vector<MidiClipEvent> centre = { Bend(3000, 8192),
                                                    Bend(9000, 8192) };
        const std::vector<MidiNote> plain = { Note(69, 100, 0, 14000) };
        const std::vector<float> a = RenderBlocks(plain, longInst, centre, 8000,
                                                  512, BuildBendTimeline(centre));
        const std::vector<float> b = RenderBlocks(plain, longInst, {}, 8000, 512,
                                                  {});
        CHECK(a == b);
    }

    // ---- mod wheel: depth, and no walk off pitch --------------------------
    {
        const double w = 2.0 * M_PI * kModWheelVibratoHz / SR;
        const double a = std::pow(2.0, kModWheelVibratoSemis / 12.0) - 1.0;
        CHECK(VibratoPhaseFrames(1234.0, SR, 0.0) == 0.0);         // wheel down
        CHECK(std::fabs(VibratoPhaseFrames(0.0, SR, 1.0)) < 1e-12);
        // A whole LFO period adds nothing: the note wobbles around its own pitch.
        const double period = SR / kModWheelVibratoHz;
        CHECK(std::fabs(VibratoPhaseFrames(period, SR, 1.0)) < 1e-6);
        CHECK(std::fabs(VibratoPhaseFrames(3.0 * period, SR, 1.0)) < 1e-6);
        // Half a period is the deepest excursion: 2A/ω, reached either side.
        // (The phase deviation is one-signed because the ratio only ever leads
        // the note; it comes back to nothing at every whole period, which is
        // the property that matters — the wheel never walks the pitch away.)
        const double half = period / 2.0;
        CHECK(std::fabs(VibratoPhaseFrames(half, SR, 1.0) - 2.0 * a / w) < 1e-4);
        CHECK(std::fabs(VibratoPhaseFrames(1.5 * period, SR, 1.0) - 2.0 * a / w) < 1e-4);

        const std::vector<MidiClipEvent> ev = { Cc(0, 1, 127) };
        const std::vector<MidiClipEvent> off = { Cc(0, 1, 0) };
        const std::vector<MidiNote> notes = { Note(69, 100, 0, 16000) };
        Instrument inst2 = Tone();
        inst2.attack = 0.005f;

        const std::vector<float> wheel = RenderBlocks(notes, inst2, ev, 16000,
                                                      512, {});
        const std::vector<float> flat = RenderBlocks(notes, inst2, off, 16000,
                                                     512, {});
        CHECK(MaxDiff(wheel, flat) > 0.05);        // the wheel actually moves it
        const std::vector<float> want = Reference(notes, inst2, ev, 16000, 512, {});
        const double diff = MaxDiff(wheel, want);
        if (diff >= 1e-4) std::printf("  vibrato diff %g\n", diff);
        CHECK(diff < 1e-4);
    }

    // ---- sustain: the offline form ----------------------------------------
    {
        // No CC64 at all: nothing moves, so existing projects are untouched.
        std::vector<MidiNote> none = { Note(60, 100, 0, 1000) };
        ApplySustain(none, {}, 100000);
        CHECK(none[0].lengthFrames == 1000);

        // Pedal down before the first note ("pedal down at load"), lifted while
        // two overlapping notes are still ringing.
        std::vector<MidiClipEvent> ev = { Cc(0, 64, 127), Cc(3000, 64, 0) };
        std::vector<MidiNote> notes = { Note(60, 100, 0, 1000),      // off @1000
                                        Note(64, 100, 500, 1000) };   // off @1500
        ApplySustain(notes, ev, 100000);
        CHECK(notes[0].lengthFrames == 3000);      // held to the lift
        CHECK(notes[1].lengthFrames == 2500);      // ditto, its own length kept

        // Pedal up BEFORE the note-off: no extension.
        std::vector<MidiClipEvent> early = { Cc(0, 64, 127), Cc(300, 64, 0) };
        std::vector<MidiNote> n2 = { Note(60, 100, 0, 1000) };
        ApplySustain(n2, early, 100000);
        CHECK(n2[0].lengthFrames == 1000);

        // A pedal that never lifts rings to the region's end and no further.
        std::vector<MidiClipEvent> held = { Cc(0, 64, 127) };
        std::vector<MidiNote> n3 = { Note(60, 100, 0, 1000) };
        ApplySustain(n3, held, 4000);
        CHECK(n3[0].lengthFrames == 4000);

        // A re-strike of the same key while the pedal holds cuts it there.
        std::vector<MidiClipEvent> ev4 = { Cc(0, 64, 127), Cc(3000, 64, 0) };
        std::vector<MidiNote> n4 = { Note(60, 100, 0, 1000),
                                     Note(60, 100, 2000, 500) };
        ApplySustain(n4, ev4, 100000);
        CHECK(n4[0].lengthFrames == 2000);         // ends where it was re-struck
        CHECK(n4[1].lengthFrames == 1000);         // off @2500 -> the lift @3000

        // The half-pedal threshold: 63 is up, 64 is down.
        std::vector<MidiClipEvent> ev5 = { Cc(0, 64, 63) };
        std::vector<MidiNote> n5 = { Note(60, 100, 0, 1000) };
        ApplySustain(n5, ev5, 100000);
        CHECK(n5[0].lengthFrames == 1000);
        ev5[0].value = 64;
        ApplySustain(n5, ev5, 100000);
        CHECK(n5[0].lengthFrames == 100000);       // held to the region end
    }

    // ---- sustain: the live latch, and the two forms agreeing -------------
    {
        SustainLatch latch;
        CHECK(!latch.Down());
        CHECK(!latch.DeferNoteOff(60));            // pedal up: release now
        latch.SetValue(127);
        CHECK(latch.Down());
        CHECK(latch.DeferNoteOff(60));             // pedal down: defer
        CHECK(latch.Holding(60));
        CHECK(latch.Retrigger(60));                // a re-strike takes it back
        CHECK(!latch.Retrigger(60));               // ... once
        CHECK(!latch.Holding(60));
        CHECK(latch.DeferNoteOff(60));
        CHECK(latch.HeldCount() == 1);
        CHECK(latch.SetValue(0));                  // the lift releases the held
        CHECK(latch.HeldCount() == 0);
        CHECK(!latch.SetValue(0));                 // already up: nothing to release
        latch.Reset();

        // The same content through both forms. The latch is driven the way the
        // engine drives it; ApplySustain is the recorded path. They must agree
        // note for note, or a recorded pedal would not sound like a played one.
        struct Case { const char* name; std::vector<MidiClipEvent> ev;
                      std::vector<MidiNote> notes; };
        std::vector<Case> cases = {
            { "hold-and-lift",
              { Cc(0, 64, 127), Cc(3000, 64, 0) },
              { Note(60, 100, 0, 1000), Note(64, 100, 500, 1000) } },
            { "pedal-down-at-load",
              { Cc(0, 64, 127), Cc(9000, 64, 0) },
              { Note(60, 100, 100, 900), Note(67, 100, 2000, 100) } },
            { "restrike-under-pedal",
              { Cc(0, 64, 127), Cc(8000, 64, 0) },
              { Note(60, 100, 0, 1000), Note(60, 100, 2000, 500) } },
            { "pedal-before-off",
              { Cc(0, 64, 127), Cc(500, 64, 0) },
              { Note(60, 100, 0, 1000), Note(62, 100, 7000, 500) } },
            { "two-holds-one-lift",
              { Cc(0, 64, 127), Cc(2000, 64, 0), Cc(2500, 64, 127),
                Cc(9000, 64, 0) },
              { Note(60, 100, 0, 1000), Note(64, 100, 3000, 500),
                Note(67, 100, 10000, 500) } },
        };
        for (const Case& c : cases) {
            std::vector<MidiNote> applied = c.notes;
            ApplySustain(applied, c.ev, 1000000);
            const std::vector<Frame> latchEnd = LatchReleases(c.notes, c.ev);
            for (size_t i = 0; i < applied.size(); i++) {
                const Frame offline = applied[i].startFrame + applied[i].lengthFrames;
                if (offline != latchEnd[i])
                    std::printf("  %s note %zu: offline %lld latch %lld\n",
                                c.name, i, (long long)offline,
                                (long long)latchEnd[i]);
                CHECK(offline == latchEnd[i]);
            }
        }
    }

    std::printf("midi_expression_tests: %d checks, %d failures\n",
                g_checks, g_fails);
    return g_fails ? 1 : 0;
}
