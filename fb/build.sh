#!/bin/bash
# MaxwellFB.kext: framebuffer nosso para a GTX 960 (ver MaxwellFB.cpp).
set -uo pipefail
cd "$(dirname "$0")"
S=$(xcrun --sdk macosx --show-sdk-path); K="$S/System/Library/Frameworks/Kernel.framework/Headers"
RT=$(dirname "$(find "$(dirname "$(xcrun -f clang)")/../lib/clang" -name libclang_rt.cc_kext.a | head -1)")
rm -rf build && mkdir -p build/MaxwellFB.kext/Contents/MacOS
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -mkernel -nostdinc -isysroot "$S" -I"$K" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=gnu++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
  -Wall -Wno-unused-parameter -Wno-unused-function -Wno-inconsistent-missing-override -O2 -c MaxwellFB.cpp -o build/fb.o 2> build/cc.err \
  || { grep -E "error" build/cc.err | head -30; exit 1; }
grep -c warning build/cc.err | sed 's/^/avisos: /'
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.0 -fapple-kext -nostdlib -Xlinker -kext -L"$S/usr/lib" -L"$RT" \
  build/fb.o -lkmodc++ -lkmod -lclang_rt.cc_kext -o build/MaxwellFB.kext/Contents/MacOS/MaxwellFB 2> build/ld.err || { head -20 build/ld.err; exit 1; }
cp Info.plist build/MaxwellFB.kext/Contents/Info.plist
codesign -s - -f build/MaxwellFB.kext >/dev/null 2>&1
nm -u build/MaxwellFB.kext/Contents/MacOS/MaxwellFB > build/imports.txt
echo "ok: build/MaxwellFB.kext ($(wc -l < build/imports.txt) imports)"
