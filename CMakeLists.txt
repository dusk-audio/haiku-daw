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
    src/synth/SampleBank.cpp    # kit-free loaded-soundfont store + cache
    src/synth/SfzParser.cpp     # kit-free SFZ -> LoadedInstrument
    src/synth/Sampler.cpp       # kit-free stateless multisample player
    src/synth/Sf2Reader.cpp     # kit-free SF2 metadata reader (DuskStudio port)
    src/synth/Sf2ToRegions.cpp  # kit-free SF2 preset -> regions + PCM
    src/synth/InstrumentFactory.cpp # InstrumentDesc -> IInstrument
    src/midi/MidiRecorder.cpp   # kit-free live-MIDI -> MidiClip note pairing
)
target_include_directories(daw_model PUBLIC src)

# --- LV2 plugin hosting (lilv) ----------------------------------------
# On by default, but it DEGRADES rather than fails: with lilv absent no LV2 code
# is compiled, the EffectFactory hook is never registered, and an
# EffectType::Lv2 insert loads as an index-aligned nullptr hole in the chain —
# the same graceful path an unavailable native add-on already takes. So a project
# saved on a machine with LV2 still opens everywhere else, minus the effect.
# -DDAW_LV2=OFF forces that stub path even where lilv IS installed, which is what
# the Haiku VM currently builds (HaikuPorts has lilv, it just isn't installed).
# daw_model itself never links lilv: the LV2 host is its own target so the
# kit-free model layer stays dependency-free.
option(DAW_LV2 "Host LV2 plugins via lilv (auto-disabled when lilv is missing)" ON)
set(DAW_LV2_ENABLED OFF)
if(DAW_LV2)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LILV lilv-0)
    endif()
    if(LILV_FOUND)
        set(DAW_LV2_ENABLED ON)
        message(STATUS "LV2 hosting enabled (lilv ${LILV_VERSION})")
    else()
        message(STATUS "lilv not found - building without LV2 support")
    endif()
endif()

if(DAW_LV2_ENABLED)
    add_library(daw_lv2 src/plugin/Lv2Host.cpp)
    target_include_directories(daw_lv2 PUBLIC src ${LILV_INCLUDE_DIRS})
    target_link_directories(daw_lv2 PUBLIC ${LILV_LIBRARY_DIRS})
    target_link_libraries(daw_lv2 PUBLIC daw_model ${LILV_LIBRARIES})
    # PUBLIC so anything linking this target (the app's factory registration,
    # and the effects editor's param listing later) compiles its LV2 branch only
    # where the host actually exists.
    target_compile_definitions(daw_lv2 PUBLIC DAW_HAVE_LV2=1)
endif()

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

add_executable(sfz_parser_tests tests/sfz_parser_tests.cpp)
target_link_libraries(sfz_parser_tests PRIVATE daw_model)
add_test(NAME sfz_parser_tests COMMAND sfz_parser_tests)

add_executable(sampler_tests tests/sampler_tests.cpp)
target_link_libraries(sampler_tests PRIVATE daw_model)
add_test(NAME sampler_tests COMMAND sampler_tests)

add_executable(sf2_tests tests/sf2_tests.cpp)
target_link_libraries(sf2_tests PRIVATE daw_model)
add_test(NAME sf2_tests COMMAND sf2_tests)

add_executable(instrument_io_tests tests/instrument_io_tests.cpp)
target_link_libraries(instrument_io_tests PRIVATE daw_model)
add_test(NAME instrument_io_tests COMMAND instrument_io_tests)

# RunInsertSlot itself, block by block: delay-line state carried ACROSS blocks
# and a bypass toggle mid-stream — the live-only case no whole-render test can
# reach, testable here only because the engine and Exporter now share it.
add_executable(insertslot_tests tests/insertslot_tests.cpp)
target_link_libraries(insertslot_tests PRIVATE daw_model)
add_test(NAME insertslot_tests COMMAND insertslot_tests)

# FX insert slots: per-insert bypass / wet-dry mix, their optional `fxin` /
# `masterfxin` lines, and the EffectType::Lv2 factory hook.
add_executable(fx_insert_tests tests/fx_insert_tests.cpp)
target_link_libraries(fx_insert_tests PRIVATE daw_model)
add_test(NAME fx_insert_tests COMMAND fx_insert_tests)

# Insert bypass / wet-dry through the real render graph, asserted sample-exactly
# against dry and fully-wet reference bounces (32-bit float, so no dither).
add_executable(fx_insert_render_tests tests/fx_insert_render_tests.cpp)
target_link_libraries(fx_insert_render_tests PRIVATE daw_model)
add_test(NAME fx_insert_render_tests COMMAND fx_insert_render_tests)

# LV2 port mapping: topology classification, param slot ordering, latency-port
# precedence, block chunking. Pure data -> plan logic with no lilv in it, so it
# is built and run UNCONDITIONALLY — including on machines with no LV2 at all,
# which is exactly where a regression in it would otherwise go unnoticed.
add_executable(lv2_portmap_tests tests/lv2_portmap_tests.cpp)
target_link_libraries(lv2_portmap_tests PRIVATE daw_model)
add_test(NAME lv2_portmap_tests COMMAND lv2_portmap_tests)

# The native editor's port/slot numbering and its inbound-value policy. Both
# live in a kit-free header (src/plugin/Lv2UiMap.h) precisely so they can be
# tested here: the window that uses them is Haiku-only, and these are the rules
# that decide whether a knob moves the parameter it is labelled with.
add_executable(lv2_ui_map_tests tests/lv2_ui_map_tests.cpp)
target_link_libraries(lv2_ui_map_tests PRIVATE daw_model)
add_test(NAME lv2_ui_map_tests COMMAND lv2_ui_map_tests)

# Which native plugin editors are open, and what a chain edit means for each.
# Kit-free (src/plugin/FxWatchTable.h) because the MainWindow that owns one
# cannot be built here, and these are the rules that decide whether an editor
# keeps driving the insert it was opened on.
add_executable(fx_watch_tests tests/fx_watch_tests.cpp)
target_link_libraries(fx_watch_tests PRIVATE daw_model)
add_test(NAME fx_watch_tests COMMAND fx_watch_tests)

# The real thing against whatever LV2 plugins the machine has: scan, filter,
# instantiate, process. Skips (and passes) when no hostable plugin is installed.
if(DAW_LV2_ENABLED)
    add_executable(lv2_host_tests tests/lv2_host_tests.cpp)
    target_link_libraries(lv2_host_tests PRIVATE daw_lv2)
    add_test(NAME lv2_host_tests COMMAND lv2_host_tests)

    # --- A real LV2 bundle, built by us, purely as a test fixture ----------
    # lv2_host_tests can only assert INVARIANTS, because it runs against
    # whatever each machine happens to have installed — and the two dev machines
    # have entirely different sets while CI would have neither. That left the
    # MonoDual path, non-zero latency and the rejection paths all dependent on
    # luck. These five plugins have exactly-known behaviour, so lv2_fixture_tests
    # asserts on VALUES and gets the same answer everywhere.
    #
    # Never installed: LV2_PATH is pointed at the build tree for that one test.
    set(DAW_LV2_FIXTURE_ROOT "${CMAKE_BINARY_DIR}/lv2fixture")
    add_library(daw_test_plugins MODULE tests/lv2fixture/daw_test_plugins.cpp)
    set_target_properties(daw_test_plugins PROPERTIES
        PREFIX "" OUTPUT_NAME "daw_test_plugins" SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${DAW_LV2_FIXTURE_ROOT}/daw_test.lv2")
    target_include_directories(daw_test_plugins PRIVATE ${LILV_INCLUDE_DIRS})

    # The .ttl files ARE the plugin as far as lilv is concerned: the port
    # descriptions, ranges and properties all live there, not in the binary. So
    # they are build OUTPUTS with a declared dependency on their sources, not a
    # POST_BUILD copy hung off the library.
    #
    # POST_BUILD was the obvious thing and was wrong: it only runs when the .so
    # itself relinks, so editing a .ttl left the built bundle stale and the tests
    # silently kept asserting against the OLD metadata. That cost a confusing
    # round of "the test says this port isn't toggled, but the TTL says it is".
    set(DAW_LV2_FIXTURE_BUNDLE "${DAW_LV2_FIXTURE_ROOT}/daw_test.lv2")
    set(_fixture_ttl_outputs "")
    foreach(_ttl manifest.ttl plugins.ttl)
        add_custom_command(
            OUTPUT  "${DAW_LV2_FIXTURE_BUNDLE}/${_ttl}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DAW_LV2_FIXTURE_BUNDLE}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/lv2fixture/${_ttl}"
                    "${DAW_LV2_FIXTURE_BUNDLE}/${_ttl}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lv2fixture/${_ttl}"
            COMMENT "Updating LV2 fixture ${_ttl}")
        list(APPEND _fixture_ttl_outputs "${DAW_LV2_FIXTURE_BUNDLE}/${_ttl}")
    endforeach()
    add_custom_target(daw_test_plugins_ttl DEPENDS ${_fixture_ttl_outputs})
    add_dependencies(daw_test_plugins daw_test_plugins_ttl)

    add_executable(lv2_fixture_tests tests/lv2_fixture_tests.cpp)
    target_link_libraries(lv2_fixture_tests PRIVATE daw_lv2)
    add_dependencies(lv2_fixture_tests daw_test_plugins)
    add_test(NAME lv2_fixture_tests COMMAND lv2_fixture_tests)

    # LV2 inserts through the real render graph, bounced to WAV and compared
    # sample-exactly. The only test covering MakeEffect -> chain -> RunInsertSlot
    # -> Exporter with a real plugin, and the only one covering PDC across the
    # LV2 boundary. Shares the fixture and its LV2_PATH.
    add_executable(lv2_render_tests tests/lv2_render_tests.cpp)
    target_link_libraries(lv2_render_tests PRIVATE daw_lv2)
    add_dependencies(lv2_render_tests daw_test_plugins)
    add_test(NAME lv2_render_tests COMMAND lv2_render_tests)

    # LV2_PATH = our fixture + the system SPEC bundles.
    #
    # The spec bundles are not optional decoration. With the fixture dir alone,
    # lilv has no lv2core to resolve the plugin-class hierarchy against and takes
    # a path that leaks a node inside lilv_world_load_plugin_classes (caught by
    # ASan, reproducible, and absent as soon as the spec dir is on the path). It
    # is also simply not a world any real host ever sees. pkg-config knows where
    # the bundles live, which keeps this working across distros and on Haiku.
    #
    # The system dir also contains whatever plugins the machine has, so the test
    # scopes every count to its OWN four URIs rather than to the world total —
    # that keeps the assertions deterministic without faking the environment.
    pkg_get_variable(DAW_LV2_BUNDLE_DIR lv2 lv2dir)
    if(DAW_LV2_BUNDLE_DIR)
        set(DAW_LV2_TEST_PATH "${DAW_LV2_FIXTURE_ROOT}:${DAW_LV2_BUNDLE_DIR}")
    else()
        message(STATUS "lv2 bundle dir not found via pkg-config - "
                       "lv2_fixture_tests will run against the fixture alone")
        set(DAW_LV2_TEST_PATH "${DAW_LV2_FIXTURE_ROOT}")
    endif()
    # The host's Haiku default path uses setenv(overwrite=0), so this wins there
    # too and the test sees the same world on both platforms.
    set_tests_properties(lv2_fixture_tests lv2_render_tests PROPERTIES
        ENVIRONMENT "LV2_PATH=${DAW_LV2_TEST_PATH}")
endif()

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

# The export job surface: format (bit depth + dither), the timeline range,
# progress, cancellation, and the temp+rename write.
add_executable(exporter_job_tests tests/exporter_job_tests.cpp)
target_link_libraries(exporter_job_tests PRIVATE daw_model)
add_test(NAME exporter_job_tests COMMAND exporter_job_tests)

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

# Live-input routing (which keyboard/channel reaches which track). Header-only
# and kit-free, so it needs no library at all.
add_executable(midirouting_tests tests/midirouting_tests.cpp)
target_link_libraries(midirouting_tests PRIVATE daw_model)
add_test(NAME midirouting_tests COMMAND midirouting_tests)

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

    # Opens an LV2 plugin's OWN GUI in a Haiku window. A prototype, not part of
    # the app: it is how the native-UI path gets developed and watched without
    # the engine, model and undo stack attached. Needs lilv, so it is skipped
    # when LV2 support is off.
    if(DAW_LV2_ENABLED)
        add_executable(lv2_ui_host prototypes/lv2_ui_host/main.cpp)
        target_include_directories(lv2_ui_host PRIVATE ${LILV_INCLUDE_DIRS})
        target_link_directories(lv2_ui_host PRIVATE ${LILV_LIBRARY_DIRS})
        target_link_libraries(lv2_ui_host PRIVATE ${LILV_LIBRARIES} be root)
    endif()

    # --- The live LV2 editor, engine side ---------------------------------
    # Engine.cpp links the Media Kit, so the publishing half of the
    # engine -> editor path can only be built and run here. Needs lilv (the
    # insert under test is a real plugin) and an audio output device; it says
    # so and passes where there is none, like the other target-dependent tests.
    if(DAW_LV2_ENABLED)
        add_executable(lv2_live_editor_tests tests/lv2_live_editor_tests.cpp)
        target_link_libraries(lv2_live_editor_tests PRIVATE daw_engine daw_lv2)
        add_test(NAME lv2_live_editor_tests COMMAND lv2_live_editor_tests)
        # It skips (exit 77) without a plugin to host or an audio device to open;
        # without this, "the engine could not open an output" would be reported
        # as a pass.
        set_tests_properties(lv2_live_editor_tests PROPERTIES SKIP_RETURN_CODE 77)
    endif()

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
        src/ui/PluginBrowser.cpp
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

    # Brings in DAW_HAVE_LV2, which is what gates the registration in main.cpp.
    # Lv2UiWindow hosts a plugin's own GUI, so it needs lilv AND the Interface
    # Kit; it only exists in an LV2-enabled build.
    if(DAW_LV2_ENABLED)
        target_sources(daw PRIVATE src/ui/Lv2UiWindow.cpp)
        target_link_libraries(daw PRIVATE daw_lv2)
    endif()

    # --- Sample effect add-on (plugin ABI demo) -----------------------
    # A Haiku shared add-on (.so) the host loads at runtime. Only needs the
    # kit-free IEffect/PluginApi headers, so it links nothing from the app.
    add_library(tremolo_plugin MODULE src/plugin/sample/TremoloPlugin.cpp)
    set_target_properties(tremolo_plugin PROPERTIES
        PREFIX "" OUTPUT_NAME "tremolo" SUFFIX ".so")
endif()
