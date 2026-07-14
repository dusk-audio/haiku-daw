#!/bin/sh
# Compile-check Haiku-only sources with the local cross-compiler (syntax only,
# no link). Catches build breaks in engine/UI without the VM.
#   sh scripts/haiku_syntax_check.sh [files...]   (default: all Haiku targets)
XG=~/haiku-cross/haiku/generated/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++
H=/home/marc/haiku-cross/haiku/headers
INCS="-idirafter $H -idirafter $H/posix"
for d in "$H"/os "$H"/os/*/; do INCS="$INCS -I$d"; done
FILES="$*"
[ -z "$FILES" ] && FILES="src/main.cpp src/ui/TimelineView.cpp src/ui/MainWindow.cpp \
  src/ui/MeterView.cpp src/ui/EffectsWindow.cpp src/ui/SendsWindow.cpp \
  src/ui/MixerWindow.cpp src/ui/RenameWindow.cpp src/engine/Engine.cpp \
  src/engine/Recorder.cpp"
rc=0
for f in $FILES; do
  if $XG -fsyntax-only -std=c++17 -Isrc $INCS "$f" 2>/tmp/hsc.err; then
    echo "OK   $f"
  else
    echo "FAIL $f"; sed 's/^/    /' /tmp/hsc.err | head -25; rc=1
  fi
done
exit $rc
