#!/bin/sh
# Compile-check Haiku-only sources with the local cross-compiler (syntax only,
# no link). Catches build breaks in engine/UI without the VM.
#   sh scripts/haiku_syntax_check.sh [files...]   (default: all Haiku targets)
XG=~/haiku-cross/haiku/generated/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++
H=/home/marc/haiku-cross/haiku/headers
INCS="-idirafter $H -idirafter $H/posix"
# CMake-generated headers (Version.h) live in the build tree; take whichever
# build dir exists so the check sees what a real build would.
for g in build/generated build-off/generated build-host/generated; do
  [ -d "$g" ] && INCS="$INCS -I$g"
done
# Two levels under os/: the kit headers, and the add-on headers a kit header can
# pull in (<Screen.h> includes <Accelerant.h> from os/add-ons/graphics, which is
# why EffectsWindow.cpp used to "fail" this check on a healthy tree).
for d in "$H"/os "$H"/os/*/ "$H"/os/add-ons/*/; do INCS="$INCS -I$d"; done
# LV2 sources need lilv's headers; take them from pkg-config when it is there
# (the VM build has them, so a syntax break in plugin/ or Lv2UiWindow.cpp is
# caught here too instead of on the VM).
if command -v pkg-config >/dev/null 2>&1; then
  for mod in lilv-0 lv2; do
    pkg-config --exists $mod && INCS="$INCS $(pkg-config --cflags $mod)"
  done
  # DAW_HAVE_LV2 comes from the daw_lv2 target's PUBLIC definition, so the VM
  # build compiles the LV2 branches of src/ui/ and tests/ while a check without
  # it silently compiles them OUT — which is how a use-before-declaration in a
  # DAW_HAVE_LV2-only test function passed this check and failed the VM build.
  # Only when both modules are there, since the branches need lilv AND the LV2
  # headers (the same condition CMake's target uses).
  if pkg-config --exists lilv-0 lv2; then
    INCS="$INCS -DDAW_HAVE_LV2=1"
  fi
  # <lv2/core/lv2.h> lives under the SYSTEM include root, which the cross
  # compiler's sysroot does not search. Adding that root itself is not an
  # option (it drags glibc's headers into libstdc++) -- so a directory holding
  # nothing but a symlink to lv2/ is built and searched last.
  lv2root=$(pkg-config --variable=includedir lv2 2>/dev/null)
  if [ -n "$lv2root" ]; then
    lv2shim=${TMPDIR:-/tmp}/daw-haiku-syntax-lv2
    mkdir -p "$lv2shim" && ln -sfn "$lv2root/lv2" "$lv2shim/lv2"
    INCS="$INCS -idirafter $lv2shim"
  fi
fi
FILES="$*"
if [ -z "$FILES" ]; then
  # Everything that is compiled for Haiku: the app, the UI, the engine, the
  # plugins and the functional suite. The kit-free sources are in here too --
  # they compile either way, and a check that covers "all of it" is the only
  # one worth reporting as 0 FAIL.
  FILES=$(ls src/*.cpp src/*/*.cpp src/ui/widgets/*.cpp \
             tests/ui_functional_tests.cpp 2>/dev/null)
fi
rc=0
for f in $FILES; do
  if $XG -fsyntax-only -std=c++17 -Isrc $INCS "$f" 2>/tmp/hsc.err; then
    echo "OK   $f"
  else
    echo "FAIL $f"; sed 's/^/    /' /tmp/hsc.err | head -25; rc=1
  fi
done
exit $rc
