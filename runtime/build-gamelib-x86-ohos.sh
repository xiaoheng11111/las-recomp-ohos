#!/bin/bash
f="$1"
obj="build/game-x86/obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
NDKCC="/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/probe/sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin/x86_64-linux-android21-clang.cmd"
"$NDKCC" -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -fPIC \
  -DGAMELIB_ID='"4bc80a11ba5b5919"' -Isrc/runtime -c "$f" -o "$obj" 2>"$obj.err" || echo "FAIL: $f"
