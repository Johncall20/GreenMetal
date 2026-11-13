#!/bin/bash
# Compila o plugin Metal (metal/plugin-lib -> metal/out-lib/NVMTLDriver), x86_64, e assina ad-hoc.
# Mesmas opcoes do binario instalado em 2026-11-07 (ref/nullmoth/build/build_plugin.sh + IOKit).
# Uso: metal/build_plugin.sh          depois copiar para kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/ e assinar o bundle.
set -u
D=$(cd "$(dirname "$0")" && pwd)
O=$D/out-lib; mkdir -p "$O"
cd "$D/plugin-lib" || exit 2
xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 -Wno-protocol -Wno-incomplete-implementation \
  -I"$D/../nvk/mesa/include" -o "$O/NVMTLDriver.new" NVMTLDevice.m \
  -framework Foundation -framework Metal -framework IOSurface -framework MetalFX -framework IOKit 2>"$O/NVMTLDriver.err" \
  || { echo "PARADA: nao compilou"; grep -A3 ': error:' "$O/NVMTLDriver.err" | head -40; exit 5; }
codesign -f -s - "$O/NVMTLDriver.new" 2>/dev/null
mv -f "$O/NVMTLDriver.new" "$O/NVMTLDriver"
echo "ok: $O/NVMTLDriver  uuid $(otool -l "$O/NVMTLDriver" | awk '/uuid/{print $2; exit}')  avisos $(grep -c ': warning:' "$O/NVMTLDriver.err")"
