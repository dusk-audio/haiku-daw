// Host tests for the M2.4/M2.5 model work: the fade-shape curve, the
// time-range slice/delete/insert arithmetic and the clip-depth commands.
//
//   g++ -std=c++17 -Isrc tests/range_edit_tests.cpp src/model/*.cpp ...
// (or ctest --test-dir build-host -R range_edit_tests)

#include "../src/model/Commands.h"
#include "../src/model/Crossfade.h"   // ComputeCrossfades (the shape rules)
#include "../src/model/Fade.h"
#include "../src/model/RangeEdit.h"

#include <cmath>
#include <cstdio>
#include <memory>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A track with one audio clip [start, start+len) whose fade-in/out are set.
static void AddAudioClip(Project& p, TrackId tid, ClipId id, Frame start,
                         Frame len, Frame fadeIn = 0, Frame fadeOut = 0) {
    Clip c;
    c.id = id;
    c.startFrame = start;
    c.lengthFrames = len;
    c.sourceOffset = 1000;      // non-zero, so a trim's offset move is visible
    c.fadeInFrames = fadeIn;
    c.fadeOutFrames = fadeOut;
    c.sourcePath = "x.wav";
    p.AddClip(tid, c);
}

// A MIDI track with one region [start, start+len) holding the given notes
// (clip-relative).
static void AddRegion(Project& p, TrackId tid, ClipId id, Frame start,
                      Frame len, std::vector<MidiNote> notes) {
    MidiClip mc;
    mc.id = id;
    mc.startFrame = start;
    mc.lengthFrames = len;
    mc.notes = std::move(notes);
    p.AddMidiClip(tid, mc);
}

static Track MakeTrack(Project& p, TrackType type, const char* name) {
    Track t;
    t.id = p.NextTrackId();
    t.type = type;
    t.name = name;
    p.AddTrack(t);
    return t;
}

static const Clip* ClipAt(const Project& p, TrackId tid, ClipId id) {
    const Track* t = p.FindTrack(tid);
    return t ? t->FindClip(id) : nullptr;
}

int main() {
    // --- FadeGainAt: endpoints exact, monotone, equal power pairs up --------
    for (FadeShape s : { FadeShape::Linear, FadeShape::EqualPower,
                         FadeShape::SCurve }) {
        CHECK(FadeGainAt(s, 0.0f) == 0.0f);
        CHECK(FadeGainAt(s, 1.0f) == 1.0f);
        CHECK(FadeGainAt(s, -0.5f) == 0.0f);   // renderer safety, no epsilon
        CHECK(FadeGainAt(s, 1.5f) == 1.0f);
        float prev = -1.0f;
        bool mono = true;
        for (int i = 0; i <= 20; i++) {
            const float g = FadeGainAt(s, (float)i / 20.0f);
            if (g < prev) mono = false;
            prev = g;
        }
        CHECK(mono);
    }
    CHECK(FadeGainAt(FadeShape::Linear, 0.25f) == 0.25f);
    CHECK(std::fabs(FadeGainAt(FadeShape::SCurve, 0.5f) - 0.5f) < 1e-6f);
    // Equal power: the two mirrored ramps sum to constant power.
    for (int i = 0; i <= 10; i++) {
        const float t = (float)i / 10.0f;
        const float in  = FadeGainAt(FadeShape::EqualPower, t);
        const float out = FadeGainAt(FadeShape::EqualPower, 1.0f - t);
        CHECK(std::fabs(in * in + out * out - 1.0f) < 1e-5f);
    }
    // An unknown stored shape clamps to the default rather than dropping the
    // clip's fade entirely.
    CHECK(FadeShapeFromInt(99) == FadeShape::Linear);
    CHECK(FadeShapeFromInt(FadeShapeToInt(FadeShape::SCurve)) == FadeShape::SCurve);

    // ClipFadeEnvelope is the ONE envelope the engine, the exporter and the
    // drawn waveform apply: outside the fades it is 1, inside a ramp it is the
    // shape's gain at that position, and two overlapping ramps combine as the
    // MIN (the engine's long-standing rule, which the exporter now shares).
    {
        const Frame len = 1000;
        CHECK(ClipFadeEnvelope(500, len, 0, FadeShape::Linear,
                               0, FadeShape::Linear) == 1.0f);
        CHECK(ClipFadeEnvelope(250, len, 500, FadeShape::Linear,
                               0, FadeShape::Linear) == 0.5f);
        CHECK(ClipFadeEnvelope(750, len, 0, FadeShape::Linear,
                               500, FadeShape::Linear) == 0.5f);
        // Fade-in 800 and fade-out 800 overlap between 200 and 800: at 500 the
        // ramps are 0.625 (in) and 0.625 (out) -> the min, not their product.
        const float v = ClipFadeEnvelope(500, len, 800, FadeShape::Linear,
                                         800, FadeShape::Linear);
        CHECK(std::fabs(v - 0.625f) < 1e-5f);
    }

    // --- SliceClipToRange: both edges, offset, fades -----------------------
    {
        Clip c;
        c.startFrame = 1000; c.lengthFrames = 1000;
        c.sourceOffset = 5000;
        c.fadeInFrames = 200; c.fadeOutFrames = 300;
        c.gain = 0.5f;
        c.sourcePath = "x.wav";
        c.name = "take";

        Clip out;
        // Whole clip inside: the start becomes its offset from the range start
        // (which is what preserves the range's internal spacing on a paste).
        CHECK(SliceClipToRange(c, 0, 5000, &out));
        CHECK(out.startFrame == 1000 && out.lengthFrames == 1000);
        CHECK(out.sourceOffset == 5000);
        CHECK(out.fadeInFrames == 200 && out.fadeOutFrames == 300);
        CHECK(out.gain == 0.5f && out.name == "take");

        // Cut the front by 400: offset advances, fade-in shrinks by the trim.
        CHECK(SliceClipToRange(c, 1400, 5000, &out));
        CHECK(out.startFrame == 0 && out.lengthFrames == 600);
        CHECK(out.sourceOffset == 5400);
        CHECK(out.fadeInFrames == 0);          // 200 - 400 -> none left
        CHECK(out.fadeOutFrames == 300);

        // Cut the back: fade-out shrinks, fade-in untouched.
        CHECK(SliceClipToRange(c, 0, 1900, &out));
        CHECK(out.lengthFrames == 900);
        CHECK(out.fadeInFrames == 200);
        CHECK(out.fadeOutFrames == 200);       // 300 - 100

        // A range-relative start: a range starting mid-clip puts the piece at
        // (piece start - range start) and keeps its internal offset.
        CHECK(SliceClipToRange(c, 1200, 1700, &out));
        CHECK(out.startFrame == 0 && out.lengthFrames == 500);
        // ... but a piece that starts after the range start keeps the gap.
        CHECK(SliceClipToRange(c, 900, 1700, &out));
        CHECK(out.startFrame == 100 && out.lengthFrames == 700);

        // Disjoint: nothing to slice.
        CHECK(!SliceClipToRange(c, 0, 1000, &out));
        CHECK(!SliceClipToRange(c, 2000, 3000, &out));
    }

    // --- SliceRegionToRange: notes rebased, dropped and clipped ------------
    {
        MidiClip c;
        c.startFrame = 1000; c.lengthFrames = 1000;
        // Absolute starts: -500, 1600, 1950, 2050(2 outside), 1100 (before).
        c.notes = { { 60, 100, -500, 100 },   // starts before the clip window
                    { 61, 100,   100, 200 },  // absolute 1100: before the range
                    { 62, 100,   600, 200 },  // absolute 1600: inside, kept
                    { 64, 100,   950, 900 },  // absolute 1950: straddles the end
                    { 65, 100,  1050, 50 },   // absolute 2050: past the range
                  };
        MidiClip out;
        CHECK(SliceRegionToRange(c, 1500, 2000, &out));
        CHECK(out.startFrame == 0 && out.lengthFrames == 500);
        CHECK(out.notes.size() == 2);                   // the other three dropped
        CHECK(out.notes[0].pitch == 62);
        CHECK(out.notes[0].startFrame == 100);          // 1600 abs - 1500
        CHECK(out.notes[0].lengthFrames == 200);
        CHECK(out.notes[1].pitch == 64);
        CHECK(out.notes[1].startFrame == 450);          // 1950 abs - 1500
        CHECK(out.notes[1].lengthFrames == 50);         // clipped to the range
    }

    // --- ExtractRange: both kinds, track order, inactive takes skipped -----
    {
        Project p;
        const Track ta = MakeTrack(p, TrackType::Audio, "A");
        const Track tm = MakeTrack(p, TrackType::Midi, "M");
        AddAudioClip(p, ta.id, p.NextClipId(), 0, 1000);
        AddAudioClip(p, ta.id, p.NextClipId(), 2000, 1000);
        AddRegion(p, tm.id, p.NextClipId(), 500, 1000, { { 60, 100, 200, 500 } });
        // An inactive take is not copied (you copy what you can see).
        {
            MidiClip mc;
            mc.id = p.NextClipId();
            mc.startFrame = 500; mc.lengthFrames = 1000;
            mc.takeGroup = 3; mc.takeActive = false;
            p.AddMidiClip(tm.id, mc);
        }
        const std::vector<RangeItem> got = ExtractRange(p, 600, 1600);
        CHECK(got.size() == 2);                 // clip A[0] and region M
        CHECK(!got[0].midi && got[0].track == ta.id);
        CHECK(got[0].audio.startFrame == 0 && got[0].audio.lengthFrames == 400);
        CHECK(got[0].audio.sourceOffset == 1000 + 600);
        CHECK(got[1].midi && got[1].track == tm.id);
        CHECK(got[1].region.startFrame == 0);
        CHECK(got[1].region.lengthFrames == 900);   // 600..1500 of 500..1500
        CHECK(got[1].region.notes.size() == 1);     // rebased to the range
        CHECK(got[1].region.notes[0].startFrame == 100);
        CHECK(got[1].region.notes[0].lengthFrames == 500);
        // A reversed range is not a range.
        CHECK(ExtractRange(p, 1600, 600).empty());
    }

    // --- RangeDeleteCommand: touching / inside / straddle / cover / gap ----
    //
    // Range [1400, 2400):
    //   id 1 [1000,1400) ends AT the range start   -> untouched
    //   id 2 [1500,1800) wholly inside             -> removed
    //   id 3 [1200,1700) straddles the start       -> trimmed to [1200,1400)
    //   id 4 [2200,2600) straddles the end         -> trimmed to [2400,2600)
    //   id 5 [1300,2500) covers it                 -> split, new right half
    //   id 6 [4000,4500) outside                   -> untouched
    {
        Project p;
        const Track ta = MakeTrack(p, TrackType::Audio, "A");
        const Track tm = MakeTrack(p, TrackType::Midi, "M");
        AddAudioClip(p, ta.id, 1, 1000, 400);
        AddAudioClip(p, ta.id, 2, 1500, 300, 50, 60);
        AddAudioClip(p, ta.id, 3, 1200, 500, 100, 400);
        AddAudioClip(p, ta.id, 4, 2200, 400);
        AddAudioClip(p, ta.id, 5, 1300, 1200, 100, 90);
        AddAudioClip(p, ta.id, 6, 4000, 500);
        AddRegion(p, tm.id, 7, 1000, 2000,
                  { { 60, 100, 100, 400 },    // absolute 1100: the left half's
                    { 62, 100, 1000, 200 } });// absolute 2000: inside the range

        CommandStack stack;
        CHECK(stack.Execute(std::make_unique<RangeDeleteCommand>(1400, 2400), p));

        const Track* at = p.FindTrack(ta.id);
        CHECK(at && at->clips.size() == 6);       // 2 gone, 5 split in two
        CHECK(ClipAt(p, ta.id, 1) && ClipAt(p, ta.id, 1)->lengthFrames == 400);
        CHECK(ClipAt(p, ta.id, 2) == nullptr);
        {
            const Clip* c = ClipAt(p, ta.id, 3);
            CHECK(c && c->startFrame == 1200 && c->lengthFrames == 200);
            // The fade-in is untouched; the fade-out loses the 300 frames the
            // trim removed (400 - 300), leaving 100.
            CHECK(c && c->fadeInFrames == 100 && c->fadeOutFrames == 100);
        }
        {
            const Clip* c = ClipAt(p, ta.id, 4);
            CHECK(c && c->startFrame == 2400 && c->lengthFrames == 200);
            CHECK(c && c->sourceOffset == 1000 + 200);
        }
        {
            const Clip* l = ClipAt(p, ta.id, 5);
            CHECK(l && l->startFrame == 1300 && l->lengthFrames == 100);
            CHECK(l && l->fadeInFrames == 100 && l->fadeOutFrames == 0);
            // Two clips start at 2400 (id 4's trim and 5's new right half);
            // the split half is the one that is neither 4 nor 5.
            const Clip* r = nullptr;
            for (const Clip& c : at->clips)
                if (c.startFrame == 2400 && c.id != 4 && c.id != 5) r = &c;
            CHECK(r && r->lengthFrames == 100);
            CHECK(r && r->sourceOffset == 1000 + 1100);
        }
        {
            const Clip* c = ClipAt(p, ta.id, 6);
            CHECK(c && c->startFrame == 4000 && c->lengthFrames == 500);
        }
        {
            const Track* mt = p.FindTrack(tm.id);
            CHECK(mt && mt->midiClips.size() == 2);
            const MidiClip& l = mt->midiClips[0];
            CHECK(l.startFrame == 1000 && l.lengthFrames == 400);
            CHECK(l.notes.size() == 2);      // kept: a region is a WINDOW
            const MidiClip& r = mt->midiClips[1];
            CHECK(r.startFrame == 2400 && r.lengthFrames == 600);
            CHECK(r.id != 7);                // the new right half has a fresh id
            // The region is a non-destructive window: the notes the delete
            // covered are rebased and kept (silent until a later edit widens
            // the window again), exactly as a manual trim leaves them.
            CHECK(r.notes.size() == 2);
            CHECK(r.notes[0].startFrame == 100 - 1400);
            CHECK(r.notes[1].startFrame == 1000 - 1400);
        }

        // Undo restores every list exactly.
        CHECK(stack.Undo(p));
        CHECK(at->clips.size() == 6);
        CHECK(ClipAt(p, ta.id, 2) != nullptr);
        {
            const Clip* l = ClipAt(p, ta.id, 3);
            CHECK(l && l->startFrame == 1200 && l->lengthFrames == 500);
            const Clip* c5 = ClipAt(p, ta.id, 5);
            CHECK(c5 && c5->lengthFrames == 1200 && c5->sourceOffset == 1000);
            const Track* mt = p.FindTrack(tm.id);
            CHECK(mt && mt->midiClips.size() == 1
                  && mt->midiClips[0].startFrame == 1000
                  && mt->midiClips[0].lengthFrames == 2000
                  && mt->midiClips[0].notes.size() == 2);
        }

        // Redo re-applies, and a range nothing touches is a failed command
        // (so it never lands on the undo stack).
        CHECK(stack.Redo(p));
        CHECK(ClipAt(p, ta.id, 2) == nullptr);
        CHECK(!stack.Execute(std::make_unique<RangeDeleteCommand>(9000, 9500), p));
    }

    // --- RangeInsertSilenceCommand: ripple, split, exact undo --------------
    {
        Project p;
        const Track ta = MakeTrack(p, TrackType::Audio, "A");
        const Track tm = MakeTrack(p, TrackType::Midi, "M");
        AddAudioClip(p, ta.id, 1, 0,    1000);     // before: untouched
        AddAudioClip(p, ta.id, 2, 2000, 1000, 100, 50);  // straddles a=2500
        AddAudioClip(p, ta.id, 3, 4000, 1000);     // after: shifts
        AddRegion(p, tm.id, 4, 3000, 1000, { { 60, 100, 300, 200 } });  // after

        CommandStack stack;
        CHECK(stack.Execute(std::make_unique<RangeInsertSilenceCommand>(2500, 3500), p));
        {
            const Track* at = p.FindTrack(ta.id);
            CHECK(at && at->clips.size() == 4);
            CHECK(at->clips[0].startFrame == 0 && at->clips[0].lengthFrames == 1000);
            // The straddling clip's left half stays, the right half moves one
            // range length right and keeps its source position.
            const Clip* l = nullptr; const Clip* r = nullptr;
            for (const Clip& c : at->clips) {
                if (c.startFrame == 2000 && c.lengthFrames == 500) l = &c;
                if (c.startFrame == 3500) r = &c;
            }
            CHECK(l && l->id == 2);
            CHECK(r && r->lengthFrames == 500);
            CHECK(r && r->sourceOffset == 1000 + 500);
            const Clip* moved = ClipAt(p, ta.id, 3);
            CHECK(moved && moved->startFrame == 5000);   // 4000 + 1000
            // A MIDI region that starts after the point shifts; its notes do not
            // move relative to it.
            const Track* mt = p.FindTrack(tm.id);
            CHECK(mt && mt->midiClips.size() == 1
                  && mt->midiClips[0].startFrame == 4000
                  && mt->midiClips[0].notes[0].startFrame == 300);
        }
        CHECK(stack.Undo(p));
        {
            const Track* at = p.FindTrack(ta.id);
            CHECK(at && at->clips.size() == 3);
            const Clip* c2 = ClipAt(p, ta.id, 2);
            CHECK(c2 && c2->startFrame == 2000 && c2->lengthFrames == 1000
                  && c2->sourceOffset == 1000 && c2->fadeOutFrames == 50);
            const Clip* c3 = ClipAt(p, ta.id, 3);
            CHECK(c3 && c3->startFrame == 4000);
            const Track* mt = p.FindTrack(tm.id);
            CHECK(mt && mt->midiClips[0].startFrame == 3000);
        }
        // Inserting silence where nothing exists still counts as an edit only
        // when the range has length; an empty range is not a command.
        CHECK(!stack.Execute(std::make_unique<RangeInsertSilenceCommand>(5000, 5000), p));
    }

    // --- Clip depth commands ----------------------------------------------
    {
        Project p;
        const Track ta = MakeTrack(p, TrackType::Audio, "A");
        const Track tm = MakeTrack(p, TrackType::Midi, "M");
        AddAudioClip(p, ta.id, 1, 0, 1000);
        AddRegion(p, tm.id, 2, 0, 1000, { { 60, 100, 0, 100 } });
        CommandStack stack;

        CHECK(stack.Execute(std::make_unique<SetClipNameCommand>(ta.id, 1, "Verse"),
                            p));
        CHECK(ClipAt(p, ta.id, 1)->name == "Verse");
        CHECK(stack.Undo(p) && ClipAt(p, ta.id, 1)->name.empty());
        CHECK(stack.Redo(p));
        CHECK(stack.UndoName() == "Rename Clip");   // the Edit menu's label

        CHECK(stack.Execute(std::make_unique<SetClipColorCommand>(ta.id, 1, 3), p));
        CHECK(ClipAt(p, ta.id, 1)->colorIndex == 3);
        CHECK(stack.Undo(p) && ClipAt(p, ta.id, 1)->colorIndex == 0);

        CHECK(stack.Execute(std::make_unique<SetClipMuteCommand>(ta.id, 1, false, true),
                            p));
        CHECK(ClipAt(p, ta.id, 1)->muted);
        CHECK(stack.UndoName() == "Mute Clip");
        CHECK(stack.Undo(p) && !ClipAt(p, ta.id, 1)->muted);
        // The same command mutes a MIDI region.
        CHECK(stack.Execute(std::make_unique<SetClipMuteCommand>(tm.id, 2, true, true),
                            p));
        CHECK(p.FindTrack(tm.id)->midiClips[0].muted);
        // ... and a muted region contributes no notes to the render -- in the
        // PLAYBACK form too, which is what the engine and the Exporter play
        // (it lost the mute when M7.3's sustain pass met M2.5's mute).
        CHECK(p.FindTrack(tm.id)->CollectNotes().empty());
        CHECK(p.FindTrack(tm.id)->CollectPlaybackNotes().empty());
        CHECK(stack.Undo(p));
        CHECK(p.FindTrack(tm.id)->CollectNotes().size() == 1);
        CHECK(p.FindTrack(tm.id)->CollectPlaybackNotes().size() == 1);

        CHECK(stack.Execute(std::make_unique<SetClipFadeShapeCommand>(
                                ta.id, 1, FadeShape::SCurve), p));
        CHECK(ClipAt(p, ta.id, 1)->fadeShape == FadeShape::SCurve);
        CHECK(stack.Undo(p)
              && ClipAt(p, ta.id, 1)->fadeShape == FadeShape::Linear);

        // A command against a clip that is gone fails, so nothing is pushed.
        CHECK(!stack.Execute(std::make_unique<SetClipNameCommand>(ta.id, 99, "x"), p));
        CHECK(!stack.Execute(std::make_unique<SetClipMuteCommand>(ta.id, 1, true, true),
                             p));   // wrong list: 1 is an audio clip
    }

    // --- A muted clip is not a crossfade partner ---------------------------
    {
        std::vector<Clip> cs(2);
        cs[0].startFrame = 0;    cs[0].lengthFrames = 1000;
        cs[1].startFrame = 700;  cs[1].lengthFrames = 1000;
        const auto plain = ComputeCrossfades(cs);
        CHECK(plain[0].fadeOut == 300 && plain[0].outShape == FadeShape::EqualPower);
        CHECK(plain[1].fadeIn == 300 && plain[1].inShape == FadeShape::EqualPower);
        cs[1].muted = true;
        const auto muted = ComputeCrossfades(cs);
        CHECK(muted[0].fadeOut == 0);
        CHECK(muted[1].fadeIn == 0);
        // A clip's own fade keeps its chosen shape; only the auto fade is
        // equal-power.
        cs[1].muted = false;
        cs[0].fadeShape = FadeShape::SCurve;
        cs[0].fadeOutFrames = 500;      // longer than the 300 overlap
        const auto own = ComputeCrossfades(cs);
        CHECK(own[0].fadeOut == 500 && own[0].outShape == FadeShape::SCurve);
        CHECK(own[0].fadeIn == 0 && own[0].inShape == FadeShape::SCurve);
        CHECK(own[1].fadeIn == 300 && own[1].inShape == FadeShape::EqualPower);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
