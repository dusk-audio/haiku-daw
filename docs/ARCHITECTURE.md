# Haiku DAW — Architecture

A native digital audio workstation for Haiku, built on the BeOS/Haiku
**Media Kit**, **Midi Kit 2**, and **Interface Kit**. Language: C++ (the
Haiku API is C++). This document is the design of record; build milestones
live at the bottom.

---

## 1. Design goals & non-goals

**Goals**
- Native Haiku app. Use the platform kits, don't fight them.
- Multitrack audio playback and recording.
- MIDI sequencing driving internal or external synths.
- Non-destructive editing on a timeline.
- A plugin path for DSP effects and instruments.
- Sample-accurate mixing; latency as low as the Haiku media stack allows.

**Non-goals (v1)**
- Not chasing Pro Tools / Ableton feature parity.
- No VST3 GUI hosting at first (headless/native DSP only).
- No video, no notation engraving, no cloud.

**Guiding principle:** the Media Kit is already a real-time, graph-based
media-routing framework. We build a DAW *on top of* it rather than
re-implementing an audio engine from scratch.

---

## 2. Layered overview

```
+-----------------------------------------------------------+
|  UI Layer (Interface Kit: BWindow / BView)                |
|  - Timeline / arrangement view                            |
|  - Track headers, mixer strips, transport                 |
|  - Meters, plugin windows                                 |
+-----------------------------------------------------------+
|  Application / Session Layer                              |
|  - Project model (tracks, clips, automation)             |
|  - Command stack (undo/redo)                              |
|  - Transport & timeline clock                            |
|  - Serialization (project file + BFS attributes)         |
+-----------------------------------------------------------+
|  Engine Layer                                            |
|  - Audio graph (BMediaNode subclasses)                  |
|  - Mixer node, track player nodes, record node          |
|  - DSP / plugin host chain                              |
|  - MIDI engine (Midi Kit 2)                             |
+-----------------------------------------------------------+
|  Platform Kits                                          |
|  - Media Kit: BMediaRoster, BMediaNode, BBuffer...      |
|  - Midi Kit 2: BMidiRoster, BMidiProducer/Consumer      |
|  - Storage Kit: BFile, BNode attributes, queries        |
+-----------------------------------------------------------+
|  Haiku media_server + audio drivers (hda / ac97)         |
+-----------------------------------------------------------+
```

Threading rule that shapes everything: **the audio graph runs in Media Kit
service threads. The UI runs in BLooper/BWindow threads. They never touch
each other's data directly.** All cross-thread contact goes through
lock-free queues or Media Kit buffers (see §6).

---

## 3. Engine layer — the audio graph

### 3.1 Media Kit primitives we use
- `BMediaRoster` — singleton that connects nodes and starts/stops the graph.
- `BMediaNode` — base class for every processing element.
- `BBufferProducer` / `BBufferConsumer` — the two mix-in interfaces a node
  implements to send / receive audio buffers.
- `BBuffer` — a chunk of audio samples flowing between nodes, timestamped
  with a `performance_time`.
- `BTimeSource` — the clock the whole graph slaves to. The audio output
  node's time source is our master clock.
- `media_format` — negotiated format (raw audio, float, sample rate,
  channel count, buffer size).

### 3.2 Node topology (v1)

```
 [TrackPlayerNode]  (one per audio track)
        |  raw_audio buffers
        v
 [EffectChainNode]  (per-track DSP: gain, pan, plugins)
        |
        v
     [MixerNode]  <---- sums all tracks, applies master bus
        |
        v
 [SystemAudioOutput]  (BMediaRoster::GetAudioOutput / the mixer node
                       provided by media_server -> soundcard)
```

- **TrackPlayerNode**: a `BBufferProducer`. Owns a read cursor into the
  track's clip list, streams samples from disk (see §5 streaming), pushes
  `BBuffer`s downstream on the graph clock.
- **EffectChainNode**: `BBufferConsumer` + `BBufferProducer`. Runs the
  track's ordered DSP chain in place on the buffer.
- **MixerNode**: consumes N inputs, sums to the master bus, produces one
  output. (v1 can lean on the media_server's built-in mixer for final
  output; a custom MixerNode gives us metering + master FX.)
- **RecordNode** (record path): a `BBufferConsumer` connected to
  `BMediaRoster::GetAudioInput`. Writes incoming buffers to disk.

### 3.3 Prototyping shortcut: BSoundPlayer
Before wiring full nodes, `BSoundPlayer` gives a pull-model callback:
you get a buffer to fill, you fill it, it goes to the card. Perfect for
milestone 1 (hello-beep) and for validating latency/format on a given
machine. The real engine graduates to `BMediaNode`s for routing and
recording, which `BSoundPlayer` can't do.

---

## 4. Session / project model

Pure data, engine-agnostic, UI-agnostic. Lives in its own module so it can
be unit-tested without Media Kit.

```cpp
struct Project {
    double        sampleRate;      // e.g. 48000
    double        tempoBPM;
    TimeSignature timeSig;
    std::vector<Track> tracks;
    Transport     transport;       // playhead, loop points, state
};

struct Track {
    enum Type { Audio, Midi } type;
    BString           name;
    float             gain, pan;
    bool              muted, soloed, armed;
    std::vector<Clip> clips;       // ordered, non-overlapping per track
    EffectChain       fx;          // ordered plugin/DSP list
    AutomationLanes   automation;
};

struct Clip {
    int64  startFrame;     // position on timeline, in frames
    int64  lengthFrames;
    int64  sourceOffset;   // where in the source file playback begins
    BString sourcePath;    // audio file, or ref to MIDI sequence
    float   fadeInFrames, fadeOutFrames;
};
```

Frames (not seconds) are the timeline unit — sample-accurate, integer,
no float drift. Convert to/from seconds only at the UI edge.

**Undo/redo:** command pattern. Every edit is a `Command` object with
`Do()` / `Undo()`. The command stack is the *only* way the UI mutates the
project. This also gives us a clean serialization point and a natural
audit of "what changed."

---

## 5. Storage & streaming

### 5.1 Files
- Audio import/export via Media Kit's `BMediaFile` / `BMediaTrack` — these
  decode/encode wav, aiff, and whatever translators are installed.
- Project file: a directory (a "bundle") or a single serialized file.
  Recommend a directory bundle: `project.daw/` containing `project.xml`
  (or a simple binary/JSON) + a `media/` folder of recorded takes.

### 5.2 Use BFS attributes (a real Haiku superpower)
Haiku's file system stores typed attributes and indexes them. Use it:
- Tag audio files with `BPM`, `Key`, `SampleRate`, `Duration` attributes.
- A live **query** (`BQuery`) becomes an instant sample browser:
  "all loops at 120 BPM in A minor" with zero database code.
- This is the kind of feature that justifies being native instead of a
  Qt port. Lean into it.

### 5.3 Disk streaming
Audio tracks stream from disk, not RAM (a DAW can't hold hours of audio in
64-bit-address-space-but-limited RAM). Design:
- Each TrackPlayerNode owns a **ring buffer** filled by a low-priority
  **disk reader thread**, drained by the real-time audio thread.
- The audio thread *never* does file I/O or allocation. It only reads from
  the ring buffer. If the buffer underruns → emit silence + flag xrun,
  never block.

---

## 6. Threading & real-time safety

This is where DAWs live or die. Rules:

1. **Real-time (audio) thread** = the Media Kit service thread running the
   graph. Inside it: no `malloc`/`new`, no locks that the UI holds, no file
   I/O, no `BMessage` sends that block. Only arithmetic on pre-allocated
   buffers.
2. **UI thread** = BWindow/BLooper. Sends *intents* (commands) to the
   engine via a lock-free single-producer/single-consumer queue.
3. **Disk threads** = per-track streaming, low priority.
4. Parameter changes (a fader move) reach the audio thread as small
   messages in the SPSC queue, applied at buffer boundaries — never by the
   UI writing engine memory directly.
5. Meters/level data flow *back* UI-ward through a second lock-free queue
   or atomics; the UI polls at ~30–60 Hz via `BMessageRunner`.

Haiku's pervasive threading and cheap `BLooper`s make this idiomatic —
this architecture is *why* BeOS was the media OS.

---

## 7. MIDI engine (Midi Kit 2)

- `BMidiRoster` — discover producers/consumers on the system MIDI graph.
- `BMidiLocalProducer` — our sequencer emits note on/off, CC, etc.
- `BMidiLocalConsumer` — receive from a MIDI keyboard for recording/input.
- Connect our producer to a soft-synth consumer or a hardware MIDI port.
- MIDI clips store event lists (delta-timed). The transport's timeline
  clock schedules events; Midi Kit timestamps handle delivery timing.
- v1 internal instrument can be a simple wavetable/sample synth node so
  MIDI makes sound with zero external gear.

**Audio↔MIDI sync:** both slave to the same transport frame clock. Convert
frames→MIDI ticks via tempo. Keep tempo map separate from clip data so
tempo changes don't rewrite clips.

---

## 8. Plugin / DSP host

Staged plan — do not block v1 on this.

- **Stage 1 — built-in DSP.** Ship a small set of native effects (gain,
  pan, EQ, delay, a compressor) as C++ classes implementing a common
  `IEffect { process(float** io, int frames) }` interface. This is the
  `EffectChainNode`'s payload. No external format, no ABI risk.
- **Stage 2 — a stable internal plugin ABI.** Define our own C ABI so
  effects can be separate add-ons loaded via Haiku `image` (`load_add_on`).
  Haiku's add-on mechanism is clean for this.
- **Stage 3 — LV2.** Portable, open, C-based, no proprietary SDK. Best
  fit for Haiku. Host the DSP; native Interface Kit renders generic
  parameter GUIs from the LV2 port metadata.
- **Stage 4 (maybe) — VST.** Steinberg SDK is portable C++, but GUI
  binding to Haiku is real work. Consider headless-only or skip.

Keep the effect interface identical across stages so the engine never
cares which backend produced an `IEffect`.

---

## 9. UI layer (Interface Kit)

- `BWindow` per top-level window; `BView` tree inside.
- **Custom-drawn timeline**: subclass `BView`, override `Draw()`,
  `MouseDown/Moved/Up`. Waveforms rendered from cached peak files
  (min/max per pixel column, precomputed on import — never scan full audio
  on redraw).
- Transport bar: play/stop/record/loop, tempo, time display.
- Track headers: name, mute/solo/arm, gain fader, pan.
- Mixer view: channel strips, meters (fed by the level queue in §6).
- Native `BMenuBar`, `BScrollView`, drag-and-drop via Haiku's `BMessage`
  negotiation (drag a file from Tracker → drop as a clip).

Qt alternative: Haiku has a Qt port. Using it buys portability and richer
widgets but forfeits the native feel, BFS integration ergonomics, and the
"this is a real Haiku app" identity. **Recommendation: native Interface
Kit.** Revisit only if UI velocity becomes the bottleneck.

---

## 10. Prior art to read in the Haiku source tree

- **Cortex** — Media Kit node-graph editor. The canonical example of
  discovering, connecting, starting nodes with `BMediaRoster`. Read this
  first; it teaches the whole engine layer.
- **MediaPlayer** — a complete playback pipeline (`BMediaFile` →
  decode → output). Model for §5 file handling.
- **BSoundPlayer** docs/examples — milestone 1 basis.
- Haiku Book (API reference) for Media Kit, Midi Kit 2, Interface Kit.

---

## 11. Build system

- **Native gcc** on Haiku (`pkgman install gcc make cmake git`).
- Recommend **CMake** for structure + potential host-side unit tests of the
  pure model layer (§4 compiles without Media Kit).
- Link against Haiku kits: `-lbe -lmedia -lmidi2 -lroot -ltracker`.
- Repo layout:

```
haiku-daw/
├── docs/ARCHITECTURE.md      <- this file
├── CMakeLists.txt
├── src/
│   ├── model/                <- Project/Track/Clip, commands (no kit deps)
│   ├── engine/               <- BMediaNode subclasses, mixer, streaming
│   ├── midi/                 <- Midi Kit 2 wrappers
│   ├── dsp/                  <- IEffect + built-in effects
│   ├── ui/                   <- BWindow/BView timeline, mixer, transport
│   └── main.cpp
├── tests/                    <- host-buildable model tests
└── prototypes/
    └── hello_beep/           <- milestone 1 BSoundPlayer sine
```

---

## 12. Milestones

| # | Milestone | Proves | Depends on |
|---|-----------|--------|------------|
| 0 | Toolchain + `hello_beep` (BSoundPlayer sine) ✅ | Build works, audio out works, latency measured | — |
| 1 | Model layer + tests (host-buildable) ✅ | Data model + undo/redo solid | — |
| 2 | Single-track WAV playback (BSoundPlayer + WavSource) ✅ | Disk streaming, transport clock | 0,1 |
| 3 | Multitrack mix + gain/pan/mute/solo ✅ | MixerNode, per-track control, metering | 2 |
| 4 | Timeline UI: clips + waveforms, drag/move, transport, meter, live params, seek ✅ | Interface Kit, peak cache, command stack ↔ UI | 1,3 |
| 5 | Recording (audio in → disk → clip) ✅ | BMediaRecorder capture, arm, WAV take | 3 |
| 6 | MIDI playback + internal synth | Midi Kit 2, transport sync | 3 |
| 7 | Built-in DSP effects in EffectChainNode | IEffect chain, RT-safe processing | 3 |
| 8 | Project save/load (bundle + BFS attrs) | Serialization, sample browser query | 4 |

Ship each milestone runnable. Milestone 0 is the gate: if BSoundPlayer
latency in your VM is unusable, that decides "VM for dev, real hardware for
audio testing" early.

---

## 13. Known risks

- **Audio latency / driver quality.** Haiku's `hda` driver is the wildcard.
  Measure at milestone 0. Real hardware likely beats the VM.
- **VM audio** may be glitchy regardless of code — expected. Use VM for
  building/logic, real hardware for latency-sensitive testing.
- **Community size.** Fewer libs, fewer answers. Reading Haiku source is
  the primary reference, not Stack Overflow.
- **Media Kit learning curve.** Node negotiation/connection is fiddly.
  Cortex is the antidote — study before writing engine code.
- **Scope.** A DAW is huge. The milestone ladder exists so there's always
  a working build. Resist adding features mid-milestone.
