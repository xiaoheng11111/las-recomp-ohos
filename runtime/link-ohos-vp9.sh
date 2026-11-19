#!/bin/bash
# Temporary OHOS link script: relink liblasrecomp.so including ff_vp9_oh_decoder
set -u
ROOT=/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/repo/letsgo-recomp-android-master
OH="/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/hdc/ohsdk/native/llvm/bin"
SYS="C:/Users/y3430/WorkBuddy/2026-11-16-01-09-41/hdc/ohsdk/native/sysroot"
SB="/c/Users/y3430/WorkBuddy/2026-11-16-01-09-41/shaderc-build"
FF=$ROOT/platform/ohos/ffmpeg-arm64/lib
OUT=$ROOT/build/liblasrecomp.so
OBJS=$(ls $ROOT/build/obja64/*.o)
"$OH/clang++.exe" --target=aarch64-linux-ohos --sysroot="$SYS" -shared -Wl,-Bsymbolic -Wl,-soname,liblasrecomp.so -o $OUT $OBJS \
  -L$FF -lavformat -lavcodec -lswscale -lavutil -lswresample -lavdevice -lavfilter \
  -lopus -ldav1d -lbluray -luavs3d -lxml2 -lssl -lcrypto -lSoundTouch \
  -lvulkan -lEGL -lGLESv3 -lace_ndk.z -lace_napi.z -lnative_window -lhilog_ndk.z -lohaudio -lrawfile.z \
  -lnative_media_vdec -lnative_media_venc -lnative_media_codecbase -lnative_media_core \
  $SB/libshaderc_ohos_arm64.a \
  -lc++_shared -lz -lm -lc \
  > $ROOT/build/link-ohos-vp9.log 2>&1
echo "LINK_EXIT=$?"
tail -5 $ROOT/build/link-ohos-vp9.log
