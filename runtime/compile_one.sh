#!/bin/bash
f="$1"
obj="build/obj64/$(echo "$f" | tr '/' '_')"  # 路径压平
ext="${f##*.}"
OH="/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/hdc/ohsdk/native/llvm/bin"
SYS="C:/Users/y3430/WorkBuddy/2026-11-16-01-09-41/hdc/ohsdk/native/sysroot"
SB="/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/shaderc-build"
std="c11"
case "$ext" in cpp) CC="$OH/clang++.exe"; std="c++17";; *) CC="$OH/clang.exe"; std="c11";; esac
obj="$obj.$ext.o"
"$CC" --target=x86_64-linux-ohos --sysroot="$SYS" -std=$std -O2 -fno-strict-aliasing -Dmain=SDL_main -D__aarch64__ -DENABLE_OPT=0 -D_GNU_SOURCE -D__linux__ -D__OHOS__ \
  -Iplatform/ohos/sdl3shim -Isrc/runtime -Isrc/third_party/vma -Isrc/third_party/imgui -Isrc/third_party/imgui/backends \
  -Isrc/third_party/smaa/Textures -I"$SB/glslang-15.1.0" -I"$SB/glslang-15.1.0/glslang/Include" -I"$SB/glslang-15.1.0/SPIRV" \
  -I"$SB/SPIRV-Headers-main/include" -I"$SB/shaderc-main/libshaderc/include" \
  -c -o "$obj" "$f" 2>"$obj.err" || echo "FAIL: $f"
