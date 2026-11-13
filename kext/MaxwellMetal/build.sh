#!/bin/bash
set -e
cd "$(dirname "$0")"
S=$(xcrun --sdk macosx --show-sdk-path)
RT=$(dirname "$(xcrun clang -print-libgcc-file-name 2>/dev/null || true)")
[ -f "$RT/libclang_rt.cc_kext.a" ] || RT=$(dirname "$(find "$(dirname "$(xcrun -f clang)")/../lib/clang" -name libclang_rt.cc_kext.a | head -1)")
rm -rf build && mkdir -p build/MaxwellMetal.kext/Contents/MacOS
cp Info.plist build/MaxwellMetal.kext/Contents/
xcrun clang -arch x86_64 -mmacosx-version-min=26.0 -O2 -mkernel -fno-builtin -DKERNEL -D__KERNEL__ -I"$S/System/Library/Frameworks/Kernel.framework/Headers" -isysroot "$S" -c MaxwellMetal_info.c -o build/info.o
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -std=gnu++17 -O2 -Wall -mkernel -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -nostdlib \
  -DKERNEL -D__KERNEL__ -I"$S/System/Library/Frameworks/Kernel.framework/Headers" -isysroot "$S" \
  -Xlinker -kext -L"$S/usr/lib" -L"$RT" -lkmodc++ -lkmod -lclang_rt.cc_kext \
  MaxwellMetal.cpp MaxwellBackend.cpp build/info.o -o build/MaxwellMetal.kext/Contents/MacOS/MaxwellMetal
codesign -s - -f build/MaxwellMetal.kext
nm -u build/MaxwellMetal.kext/Contents/MacOS/MaxwellMetal
