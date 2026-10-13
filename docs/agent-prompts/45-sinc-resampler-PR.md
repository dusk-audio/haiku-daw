# feat(dsp): the exporter resamples with a windowed sinc (M4.5)

Branch `feature/latency-sinc-sidechain` (the package's three items share it;
this record covers M4.5). Spec: `45-sinc-resampler.md`.

```
ec116ac feat(dsp): the exporter resamples with a windowed sinc (M4.5)
ad67f61 docs: spec the windowed-sinc resampler (M4.5)
```

## What was built

`src/dsp/SincResampler.{h,cpp}` — a streaming Kaiser-windowed-sinc resampler,
192 taps, β = 9.0, 1024 polyphase phases, in the `daw_model` list (no option
gate: no external dependency). `Exporter.cpp`'s per-clip decode (`DecodeClip`)
uses it; the linear `Resampler` is untouched and still serves the live monitor
path.

The kernel's band edges come from the window's own parameters rather than a
hand-tuned cutoff: the Kaiser design formulas give the transition width for
(192, 9.0) as ≈ 0.03 cycles/input-sample, the cutoff is placed half a
transition below the lower Nyquist, and every phase is then normalised so its
weights sum to exactly 1. Equal rates are a bit-exact passthrough.

## Measured

| Measurement | Sinc | Linear (same input) |
| --- | --- | --- |
| 30 kHz → 18 kHz alias, 96k → 48k | **-122.3 dBFS** | **-0.0 dBFS** (fully aliased) |
| Passband, 100 Hz … 19.2 kHz, 96k → 48k | **-0.000 dB** worst | fails near the edge |
| 14 kHz image of a 24k → 48k upsample | **-119.7 dB** re the tone | — |

`tests/sinc_resampler_tests.cpp`, **27 checks**: identity bit-exactness,
aliasing rejection (both an absolute threshold and "quieter than the linear
one by 30 dB"), passband flatness ≤ 0.1 dB, DC/level exactness to 2e-6,
upsample image rejection, chunked == one call sample-for-sample, frame counts,
Reset, mono, and degenerates (NaN/zero rates, null input, channel clamps).

## Mutations (each broken, watched failing, restored)

| Mutation | Caught by |
| --- | --- |
| identity routed through the kernel | `r.IsIdentity()`, the bit-exactness check |
| Kaiser window removed (rectangular) | both alias thresholds (-70 dB; and vs linear) |
| per-phase DC normalisation dropped | the constant/level check |

## One behaviour change outside the package

The linear `Resampler`'s equal-rate path is **not** a passthrough: its first two
outputs are the same sample, so every clip already at the project rate was
decoded one frame late and its last frame dropped. The sinc path passes equal
rates through exactly, so `sidechain_tests`' hand-built one-pole envelope checks
(2 of 106) moved by that one frame; the test now carries the reason in-line.

## Verification

```
$ cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2
exit 0
$ ctest --test-dir build-host
100% tests passed out of 69
$ sh scripts/haiku_syntax_check.sh
75 OK, 4 FAIL — all four the pre-existing codec files (FlacSink/FlacSource/
VorbisSink/VorbisSource) whose FLAC/ogg headers the cross toolchain lacks; no
file this package touches is among them.
```

Nothing Haiku-only was touched, so the VM pass has no M4.5-specific item.
