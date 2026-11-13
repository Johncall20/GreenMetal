#!/bin/bash
# AGDC do NullMoth (PolyForm Noncommercial: uso pessoal) apontado para o framebuffer NVDA do web driver.
set -uo pipefail
cd "$(dirname "$0")"
S=$(xcrun --sdk macosx --show-sdk-path); K="$S/System/Library/Frameworks/Kernel.framework/Headers"
RT=$(dirname "$(find "$(dirname "$(xcrun -f clang)")/../lib/clang" -name libclang_rt.cc_kext.a | head -1)")
EX=$(plutil -extract CFBundleExecutable raw Info.plist)
rm -rf build && mkdir -p build/MaxwellAGDC.kext/Contents/MacOS
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -mkernel -nostdinc -isysroot "$S" -I"$K" -I. \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=gnu++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
  -Wall -Wno-unused-parameter -Wno-unused-function -O2 -c nvrm-agdc.cpp -o build/agdc.o 2> build/cc.err || { grep error build/cc.err | head -20; exit 1; }
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -nostdlib -Xlinker -kext -L"$S/usr/lib" -L"$RT" \
  build/agdc.o -lkmodc++ -lkmod -lclang_rt.cc_kext -o "build/MaxwellAGDC.kext/Contents/MacOS/$EX" 2> build/ld.err || { head -20 build/ld.err; exit 1; }
cp Info.plist build/MaxwellAGDC.kext/Contents/Info.plist
codesign -s - -f build/MaxwellAGDC.kext >/dev/null 2>&1
echo "ok: build/MaxwellAGDC.kext ($EX)"
