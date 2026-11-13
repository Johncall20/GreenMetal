#!/bin/bash
# Porte do NVAccel (NullMoth, PolyForm Noncommercial: uso pessoal) para o RM r387. So compila e linka.
set -uo pipefail
cd "$(dirname "$0")"
S=$(xcrun --sdk macosx --show-sdk-path); K="$S/System/Library/Frameworks/Kernel.framework/Headers"
RT=$(dirname "$(find "$(dirname "$(xcrun -f clang)")/../lib/clang" -name libclang_rt.cc_kext.a | head -1)")
rm -rf build && mkdir -p build/MaxwellAccel.kext/Contents/MacOS
CF=( -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -mkernel -nostdinc -isysroot "$S" -I"$K" -Isrc -Isrc/iofam -Isrc/re -I.
     -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DNM_TAHOE=1 -std=gnu++17 -fno-rtti -fno-exceptions
     -fno-builtin -fno-common -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 )
xcrun clang++ "${CF[@]}" -c src/nvrm-accel.cpp -o build/nvrm-accel.o 2> build/cc.err; rc=$?
grep -c "error:" build/cc.err | sed 's/^/erros: /'; grep "error:" build/cc.err | head -40
[ $rc -eq 0 ] || exit 1
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -nostdlib -Xlinker -kext -L"$S/usr/lib" -L"$RT" \
  build/nvrm-accel.o -lkmodc++ -lkmod -lclang_rt.cc_kext -o build/MaxwellAccel.kext/Contents/MacOS/MaxwellAccel 2> build/ld.err \
  || { head -30 build/ld.err; exit 1; }
cp Info.plist build/MaxwellAccel.kext/Contents/Info.plist
codesign -s - -f build/MaxwellAccel.kext >/dev/null 2>&1
echo "ok: build/MaxwellAccel.kext"
