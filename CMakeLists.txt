cmake_minimum_required(VERSION 3.16)
project(haiku_daw CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# The model layer is kit-free (STL only) so it builds on any host. Keep it
# in its own library target; engine/ui targets (which link Haiku kits) will
# depend on this but never the reverse.
add_library(daw_model
    src/model/Project.cpp
    src/model/Commands.cpp
    src/model/PeakCache.cpp     # kit-free min/max waveform envelope
    src/model/ProjectIO.cpp     # kit-free project save/load
    src/engine/WavSource.cpp    # kit-free RIFF/WAVE reader, host-testable
    src/engine/WavWriter.cpp    # kit-free RIFF/WAVE writer, host-testable
    src/engine/Resampler.cpp    # kit-free linear SRC, host-testable
    src/dsp/Biquad.cpp          # kit-free biquad filter effect
    src/dsp/Delay.cpp           # kit-free feedback delay effect
    src/dsp/EffectFactory.cpp   # EffectDesc -> IEffect
    src/synth/Synth.cpp         # kit-free polyphonic sine synth
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

# --- Haiku-only targets: the Media Kit engine and its driver ----------
# CMake sets HAIKU=TRUE when building on Haiku. On other hosts these are
# skipped so the model layer + tests still build for fast iteration.
if(HAIKU)
    add_library(daw_engine
        src/engine/Engine.cpp
        src/engine/Recorder.cpp
    )
    target_include_directories(daw_engine PUBLIC src)
    # -lbe (app/interface), -lmedia (BSoundPlayer/BMediaRecorder), -lroot.
    # WavSource + WavWriter are kit-free and live in daw_model.
    target_link_libraries(daw_engine PUBLIC daw_model be media root)

    add_executable(play_clip prototypes/play_clip/main.cpp)
    target_link_libraries(play_clip PRIVATE daw_engine)

    add_executable(mix_project prototypes/mix_project/main.cpp)
    target_link_libraries(mix_project PRIVATE daw_engine)

    add_executable(record_clip prototypes/record_clip/main.cpp)
    target_link_libraries(record_clip PRIVATE daw_engine)

    # --- The app: timeline UI (milestone 4) ---------------------------
    add_executable(daw
        src/main.cpp
        src/ui/TimelineView.cpp
        src/ui/MainWindow.cpp
        src/ui/MeterView.cpp
    )
    # -lbe (Application/Interface Kit), -ltracker (BFilePanel). daw_engine
    # pulls in daw_model + media.
    target_link_libraries(daw PRIVATE daw_engine be tracker)
endif()
