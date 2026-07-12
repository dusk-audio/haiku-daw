cmake_minimum_required(VERSION 3.16)
project(hello_beep CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hello_beep main.cpp)

# Haiku kits: -lbe (app/interface), -lmedia (BSoundPlayer), -lroot (snooze).
target_link_libraries(hello_beep be media root)
