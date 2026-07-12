# hello_beep — Milestone 0

Plays a 440 Hz sine for 3 seconds through the Haiku Media Kit.
Gate before building anything real: proves toolchain + Media Kit link +
audio output + shows negotiated format and latency.

## Build & run (on Haiku)

With CMake:

```
cd prototypes/hello_beep
cmake -B build
cmake --build build
./build/hello_beep
```

Or the one-liner (no CMake needed):

```
g++ -std=c++17 main.cpp -o hello_beep -lbe -lmedia -lroot
./hello_beep
```

## Expected output

```
Negotiated format:
  frame_rate   = 48000.0 Hz
  channels     = 2
  buffer_size  = <server chooses> bytes
  latency      = <N> us (<N> ms)
Playing 440 Hz for 3 seconds...
Done.
```

You should hear a clean, steady tone.

## What to report back

- The full printed block (especially **latency** and **buffer_size**).
- Did the tone sound **clean**, or **crackly/glitchy/stuttering**?
- VM or real hardware?

Latency + glitching here decides the dev strategy: if VM audio is bad but
it compiles/runs, we develop logic in the VM and do audio-latency testing
on real hardware. A clean low-latency number means the VM is fine for now.
