# Task: Windowed-sinc resampling for the offline render (M4.5)

Item M4.5 of `docs/PLAN_1.0.md`: "Windowed-sinc resampler (kit-free `src/dsp/`)
for export at another rate and for file-rate conversion. The current linear
resampler stays only for the live monitor path. Host tests check aliasing
rejection and passband flatness."

## Where this sits

`Resampler` (`src/engine/Resampler.h/.cpp`) is a streaming LINEAR interpolator:
cheap, kit-free, and every clip whose file rate differs from the output rate
goes through it. Its aliasing rejection is that of a first-order interpolator —
the images of the source spectrum fold back at only -10 to -20 dB — which is
inaudible on a monitor path nobody listens analytically to, and is not good
enough for a file that leaves the app as the master.

So: a second, better resampler for the offline path, and no change to the live
one.

| Path | Resampler | Why |
| --- | --- | --- |
| Live monitor (`Engine::FillBuffer`, linear, carried across blocks) | unchanged | RT path; a few extra multiply-adds per frame buy nothing audible in a monitor mix |
| Exporter's per-clip decode (`Exporter.cpp`, `DecodeClip`) | **this one** | the bounce is the master; aliasing here is printed into the file |
| Live track playback (disk threads) | `Resampler`, unchanged | v1 keeps the linear one behind the existing interface; a follow-up decides if it moves |

"File-rate conversion" from the plan is the same code path as the export case:
a clip recorded at 44.1 kHz in a 96 kHz project is converted by the exporter on
its way to the mix, which is exactly `DecodeClip`.

## Design

`src/dsp/SincResampler.h/.cpp` — kit-free (STL only, no allocation in
`Process` beyond the caller's `out` vector growing), same shape as `Resampler`:

```cpp
SincResampler(double inRate, double outRate, int channels = 2);
void Process(const float* in, size_t inFrames, std::vector<float>& out);
void Reset();
double Ratio() const;   bool IsIdentity() const;   int Channels() const;
```

- **Kernel: Kaiser-windowed sinc, β = 8.6.** A windowed sinc is the standard
  choice because the filter's job here is pure band-limiting: reconstruct the
  source band and reject everything that would fold. The window decides how
  much of the ideal sinc's stopband is given up to make it finite. Blackman was
  the obvious alternative and is easier to compute — but its sidelobes stop at
  **-58 dB** regardless of length, and 16-bit material has a -96 dB floor, so a
  Blackman-interpolated brickwall is the loudest error in the file. Kaiser has
  a continuous stopband/transition trade-off through β: β = 8.6 gives ≈ -90 dB
  sidelobes at a transition width of ≈ 0.06 × Fs (in input-rate cycles), which
  puts every alias at least ~30 dB under a 16-bit floor. β and the tap count
  are documented constants, so the trade is one line to move.
- **64 taps per output sample, 512 polyphase phases.** The kernel is sampled
  into a `kPhases × kTaps` table at construction (once, off the RT thread): an
  output sample's fractional position selects the nearest phase, so the cost is
  64 multiply-adds per output frame per channel — irrelevant offline, and the
  512-phase quantisation keeps the phase error ~-90 dB. 64 taps at 48 kHz is a
  ~2.4 kHz transition band, comfortably inside the guard band the design
  leaves below the lower Nyquist.
- **Cutoff = 0.5 × min(1, out/in) × 0.92, then DC-normalised.** Downsampling
  must band-limit to the OUTPUT Nyquist; upsampling must only remove the
  source's own images. The 0.92 leaves the transition band inside the guard so
  the passband edge stays flat (a filter cut exactly at Nyquist would droop
  into the top octave). Every phase is then scaled so it sums to exactly 1.0 —
  DC gain is exactly unity, so a constant signal survives bit-for-bit in
  value, which is what a file-rate conversion of a level has to do.
- **History across calls.** `Process` keeps the last `kTaps` input frames and
  the fractional read position, so feeding one 4096-frame chunk equals feeding
  four 1024-frame ones — the property the disk/export decode loop depends on
  (`WavSource::ReadChunk` decides the chunk size, not us).
- **`IsIdentity()` (in == out rate, exactly) is a bit-exact passthrough.** Not
  an optimisation: the exporter's own tests assert sample-exact renders, and
  every clip already at the output rate must stay untouched — including the
  "resample" of a project exported at its own rate being bit-identical to
  before this package.

## Work items

1. `src/dsp/SincResampler.h` + `.cpp`, added to the `daw_model` source list in
   `CMakeLists.txt` (no option gate — it has no external dependency, unlike
   `DAW_LV2` / `DAW_FLAC`).
2. `Exporter.cpp::DecodeClip` uses it instead of `Resampler`. Nothing else in
   the exporter changes: `PlaceClip` reads `out.size()/2` and is rate-agnostic.
3. `tests/sinc_resampler_tests.cpp`, registered in the flat test list:
   - **identity**: equal rates pass through bit-exactly (mutation: route the
     identity case through the kernel → the exactness check fails);
   - **aliasing rejection**: 96 k → 48 k, a 30 kHz tone (well above the 24 kHz
     output Nyquist) must come back below -70 dBFS. Linear interpolation lands
     around -20 dBFS here, so the threshold is a real discriminator;
   - **passband flatness**: 96 k → 48 k, tones from 100 Hz to 0.4 × 48 kHz must
     come back within ±0.1 dB — the check the linear resampler fails near the
     band edge;
   - **DC / level**: a constant survives as the same constant (exact unity gain);
   - **streaming continuity**: one `Process` of N frames == two of N/2, sample
     for sample;
   - **frame count**: `outFrames ≈ inFrames × Ratio()` (±1 frame) both ways;
   - **channels**: mono stays mono (no read past the frame), 0/9 clamp as the
     linear one does.
4. Record: `45-sinc-resampler-PR.md`, with the measured alias level, the
   measured passband error and the exact ctest counts.

## Definition of done

- `cmake --build build-host` exits 0, `ctest --test-dir build-host` all green
  (new suite included), `sh scripts/haiku_syntax_check.sh` 0 FAIL (nothing
  Haiku-only is touched, so this is a courtesy check).
- Every new behaviour above is mutation-checked: break it, watch the named
  test fail, restore it.
- The record names what the resampler measures (alias level, flatness, count
  of checks) rather than asserting it is "better".
