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
    src/engine/WavSource.cpp    # kit-free RIFF/WAVE reader, host-testable
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

# --- Haiku-only targets: the Media Kit engine and its driver ----------
# CMake sets HAIKU=TRUE when building on Haiku. On other hosts these are
# skipped so the model layer + tests still build for fast iteration.
if(HAIKU)
    add_library(daw_engine
        src/engine/Engine.cpp
    )
    target_include_directories(daw_engine PUBLIC src)
    # -lbe (app/interface), -lmedia (BSoundPlayer), -lroot. WavSource is
    # now kit-free and lives in daw_model.
    target_link_libraries(daw_engine PUBLIC daw_model be media root)

    add_executable(play_clip prototypes/play_clip/main.cpp)
    target_link_libraries(play_clip PRIVATE daw_engine)
endif()
