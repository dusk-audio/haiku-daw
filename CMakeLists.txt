cmake_minimum_required(VERSION 3.16)
project(haiku_daw CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Optional AddressSanitizer/UBSan for the fuzz corpora (and everything else).
# Off by default so the normal ctest loop stays fast; the fuzz tests still catch
# hangs/aborts without it, but ASan is what catches silent OOB reads.
option(DAW_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(DAW_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# The model layer is kit-free (STL only) so it builds on any host. Keep it
# in its own library target; engine/ui targets (which link Haiku kits) will
# depend on this but never the reverse.
add_library(daw_model
    src/model/Project.cpp
    src/model/Commands.cpp
    src/model/PeakCache.cpp     # kit-free min/max waveform envelope
    src/model/ProjectIO.cpp     # kit-free project save/load
    src/model/Automation.cpp    # kit-free breakpoint automation lanes
    src/model/RegionOps.cpp     # kit-free clip region DSP (peak/reverse/silence)
    src/model/SmfIO.cpp         # kit-free Standard MIDI File reader/writer
    src/app/AppSettings.cpp     # kit-free app settings text serializer
    src/engine/WavSource.cpp    # kit-free RIFF/WAVE reader, host-testable
    src/engine/WavWriter.cpp    # kit-free RIFF/WAVE writer, host-testable
    src/engine/Resampler.cpp    # kit-free linear SRC, host-testable
    src/engine/Exporter.cpp     # kit-free offline mix -> WAV, host-testable
    src/engine/Metronome.cpp    # kit-free click generator, host-testable
    src/dsp/Biquad.cpp          # kit-free biquad filter effect
    src/dsp/Delay.cpp           # kit-free feedback delay effect
    src/dsp/Reverb.cpp          # kit-free Schroeder reverb + DuskVerb engines
    src/dsp/duskverb/DattorroPlateVintage.cpp   # DuskVerb plate (verbatim port)
    src/dsp/duskverb/DattorroTank.cpp           # DuskVerb Dattorro tank (verbatim)
    src/dsp/duskverb/OctaveGEQDesign.cpp        # DuskVerb octave GEQ design (verbatim)
    src/dsp/duskverb/FDNReverb.cpp              # DuskVerb FDN / AccurateHall (verbatim)
    src/dsp/Compressor.cpp      # kit-free dynamics compressor
    src/dsp/Eq.cpp              # kit-free 5-band EQ (Multi-Q filter math)
    src/dsp/Loudness.cpp        # kit-free BS.1770 LUFS + true-peak meter
    src/dsp/Limiter.cpp         # kit-free offline look-ahead true-peak limiter
    src/dsp/LookaheadLimiter.cpp # kit-free live look-ahead limiter (reports PDC latency)
    src/dsp/Saturator.cpp       # kit-free tape saturation (tape-echo port)
    src/dsp/Gate.cpp            # kit-free noise gate / expander
    src/dsp/Widener.cpp         # kit-free M/S stereo widener
    src/dsp/EffectFactory.cpp   # EffectDesc -> IEffect
    src/synth/Synth.cpp         # kit-free polyphonic sine synth
    src/midi/MidiRecorder.cpp   # kit-free live-MIDI -> MidiClip note pairing
)
target_include_directories(daw_model PUBLIC src)

# Host-buildable tests. Run with: ctest  (or the binaries directly)
enable_testing()
add_executable(model_tests tests/model_tests.cpp)
target_link_libraries(model_tests PRIVATE daw_model)
add_test(NAME model_tests COMMAND model_tests)

add_executable(wav_tests tests/wav_tests.cpp)
target_link_libraries(wav_tests PRIVATE daw_model)
add_test(NAME wav_tests COMMAND wav_tests)

add_executable(peak_tests tests/peak_tests.cpp)
target_link_libraries(peak_tests PRIVATE daw_model)
add_test(NAME peak_tests COMMAND peak_tests)

add_executable(wavwriter_tests tests/wavwriter_tests.cpp)
target_link_libraries(wavwriter_tests PRIVATE daw_model)
add_test(NAME wavwriter_tests COMMAND wavwriter_tests)

add_executable(resampler_tests tests/resampler_tests.cpp)
target_link_libraries(resampler_tests PRIVATE daw_model)
add_test(NAME resampler_tests COMMAND resampler_tests)

add_executable(effect_tests tests/effect_tests.cpp)
target_link_libraries(effect_tests PRIVATE daw_model)
add_test(NAME effect_tests COMMAND effect_tests)

add_executable(synth_tests tests/synth_tests.cpp)
target_link_libraries(synth_tests PRIVATE daw_model)
add_test(NAME synth_tests COMMAND synth_tests)

add_executable(projectio_tests tests/projectio_tests.cpp)
target_link_libraries(projectio_tests PRIVATE daw_model)
add_test(NAME projectio_tests COMMAND projectio_tests)

add_executable(grid_tests tests/grid_tests.cpp)
target_link_libraries(grid_tests PRIVATE daw_model)
add_test(NAME grid_tests COMMAND grid_tests)

add_executable(dsp_extra_tests tests/dsp_extra_tests.cpp)
target_link_libraries(dsp_extra_tests PRIVATE daw_model)
add_test(NAME dsp_extra_tests COMMAND dsp_extra_tests)

add_executable(exporter_tests tests/exporter_tests.cpp)
target_link_libraries(exporter_tests PRIVATE daw_model)
add_test(NAME exporter_tests COMMAND exporter_tests)

add_executable(exporter_pdc_tests tests/exporter_pdc_tests.cpp)
target_link_libraries(exporter_pdc_tests PRIVATE daw_model)
add_test(NAME exporter_pdc_tests COMMAND exporter_pdc_tests)

add_executable(metronome_tests tests/metronome_tests.cpp)
target_link_libraries(metronome_tests PRIVATE daw_model)
add_test(NAME metronome_tests COMMAND metronome_tests)

add_executable(automation_tests tests/automation_tests.cpp)
target_link_libraries(automation_tests PRIVATE daw_model)
add_test(NAME automation_tests COMMAND automation_tests)

add_executable(routing_tests tests/routing_tests.cpp)
target_link_libraries(routing_tests PRIVATE daw_model)
add_test(NAME routing_tests COMMAND routing_tests)

add_executable(pdc_tests tests/pdc_tests.cpp)
target_link_libraries(pdc_tests PRIVATE daw_model)
add_test(NAME pdc_tests COMMAND pdc_tests)

add_executable(framedelay_tests tests/framedelay_tests.cpp)
target_link_libraries(framedelay_tests PRIVATE daw_model)
add_test(NAME framedelay_tests COMMAND framedelay_tests)

add_executable(midicontrol_tests tests/midicontrol_tests.cpp)
target_link_libraries(midicontrol_tests PRIVATE daw_model)
add_test(NAME midicontrol_tests COMMAND midicontrol_tests)

add_executable(crossfade_tests tests/crossfade_tests.cpp)
target_link_libraries(crossfade_tests PRIVATE daw_model)
add_test(NAME crossfade_tests COMMAND crossfade_tests)

add_executable(tempomap_tests tests/tempomap_tests.cpp)
target_link_libraries(tempomap_tests PRIVATE daw_model)
add_test(NAME tempomap_tests COMMAND tempomap_tests)

add_executable(recordplan_tests tests/recordplan_tests.cpp)
target_link_libraries(recordplan_tests PRIVATE daw_model)
add_test(NAME recordplan_tests COMMAND recordplan_tests)

add_executable(appsettings_tests tests/appsettings_tests.cpp)
target_link_libraries(appsettings_tests PRIVATE daw_model)
add_test(NAME appsettings_tests COMMAND appsettings_tests)

add_executable(loudness_tests tests/loudness_tests.cpp)
target_link_libraries(loudness_tests PRIVATE daw_model)
add_test(NAME loudness_tests COMMAND loudness_tests)

add_executable(limiter_tests tests/limiter_tests.cpp)
target_link_libraries(limiter_tests PRIVATE daw_model)
add_test(NAME limiter_tests COMMAND limiter_tests)

add_executable(lookahead_limiter_tests tests/lookahead_limiter_tests.cpp)
target_link_libraries(lookahead_limiter_tests PRIVATE daw_model)
add_test(NAME lookahead_limiter_tests COMMAND lookahead_limiter_tests)

add_executable(smoothing_tests tests/smoothing_tests.cpp)
target_link_libraries(smoothing_tests PRIVATE daw_model)
add_test(NAME smoothing_tests COMMAND smoothing_tests)

add_executable(saturator_tests tests/saturator_tests.cpp)
target_link_libraries(saturator_tests PRIVATE daw_model)
add_test(NAME saturator_tests COMMAND saturator_tests)

add_executable(gate_tests tests/gate_tests.cpp)
target_link_libraries(gate_tests PRIVATE daw_model)
add_test(NAME gate_tests COMMAND gate_tests)

add_executable(widener_tests tests/widener_tests.cpp)
target_link_libraries(widener_tests PRIVATE daw_model)
add_test(NAME widener_tests COMMAND widener_tests)

add_executable(regionops_tests tests/regionops_tests.cpp)
target_link_libraries(regionops_tests PRIVATE daw_model)
add_test(NAME regionops_tests COMMAND regionops_tests)

add_executable(smfio_tests tests/smfio_tests.cpp)
target_link_libraries(smfio_tests PRIVATE daw_model)
add_test(NAME smfio_tests COMMAND smfio_tests)

add_executable(midi_tests tests/midi_tests.cpp)
target_link_libraries(midi_tests PRIVATE daw_model)
add_test(NAME midi_tests COMMAND midi_tests)

# Adversarial fuzz corpora: malformed WAV / project input must fail cleanly,
# never crash / hang / OOM. Run under ASan for teeth: cmake -DDAW_SANITIZE=ON.
add_executable(wavsource_fuzz_tests tests/wavsource_fuzz_tests.cpp)
target_link_libraries(wavsource_fuzz_tests PRIVATE daw_model)
add_test(NAME wavsource_fuzz_tests COMMAND wavsource_fuzz_tests)

add_executable(projectio_fuzz_tests tests/projectio_fuzz_tests.cpp)
target_link_libraries(projectio_fuzz_tests PRIVATE daw_model)
add_test(NAME projectio_fuzz_tests COMMAND projectio_fuzz_tests)

# Fuzz corpora must terminate quickly; a short CTest timeout turns any accidental
# non-terminating loop (a regression in the parsers' bounds) into a test failure
# instead of a hung CI. Generous vs the ~1 s they actually take.
set_tests_properties(wavsource_fuzz_tests projectio_fuzz_tests
                     PROPERTIES TIMEOUT 60)

# --- Haiku-only targets: the Media Kit engine and its driver ----------
# CMake sets HAIKU=TRUE when building on Haiku. On other hosts these are
# skipped so the model layer + tests still build for fast iteration.
if(HAIKU)
    add_library(daw_engine
        src/engine/Engine.cpp
        src/engine/Recorder.cpp
        src/midi/MidiPort.cpp       # Midi Kit 2 adapter (external MIDI I/O)
    )
    target_include_directories(daw_engine PUBLIC src)
    # -lbe (app/interface), -lmedia (BSoundPlayer/BMediaRecorder),
    # -lmidi2 (Midi Kit 2: BMidiRoster/BMidiLocalProducer/Consumer), -lroot.
    # WavSource + WavWriter are kit-free and live in daw_model.
    target_link_libraries(daw_engine PUBLIC daw_model be media midi2 root)

    add_executable(play_clip prototypes/play_clip/main.cpp)
    target_link_libraries(play_clip PRIVATE daw_engine)

    add_executable(mix_project prototypes/mix_project/main.cpp)
    target_link_libraries(mix_project PRIVATE daw_engine)

    add_executable(record_clip prototypes/record_clip/main.cpp)
    target_link_libraries(record_clip PRIVATE daw_engine)

    # MIDI loopback: producer -> our consumer -> MidiRecorder. Proves the Midi
    # Kit 2 adapter end-to-end with no hardware (a keyboard stand-in).
    add_executable(midi_loop prototypes/midi_loop/main.cpp)
    target_link_libraries(midi_loop PRIVATE daw_engine)

    # --- The app: timeline UI (milestone 4) ---------------------------
    add_executable(daw
        src/main.cpp
        src/ui/TimelineView.cpp
        src/ui/InspectorView.cpp
        src/ui/MainWindow.cpp
        src/ui/TransportBar.cpp
        src/ui/MeterView.cpp
        src/ui/EffectsWindow.cpp
        src/ui/SendsWindow.cpp
        src/ui/InstrumentWindow.cpp
        src/ui/PianoRoll.cpp
        src/ui/MixerWindow.cpp
        src/ui/RenameWindow.cpp
        src/ui/SampleBrowser.cpp
        src/storage/BfsAttr.cpp
        src/plugin/PluginHost.cpp
    )
    # -lbe (Application/Interface Kit), -ltracker (BFilePanel). daw_engine
    # pulls in daw_model + media.
    target_link_libraries(daw PRIVATE daw_engine be tracker)

    # --- Sample effect add-on (plugin ABI demo) -----------------------
    # A Haiku shared add-on (.so) the host loads at runtime. Only needs the
    # kit-free IEffect/PluginApi headers, so it links nothing from the app.
    add_library(tremolo_plugin MODULE src/plugin/sample/TremoloPlugin.cpp)
    set_target_properties(tremolo_plugin PROPERTIES
        PREFIX "" OUTPUT_NAME "tremolo" SUFFIX ".so")
endif()
