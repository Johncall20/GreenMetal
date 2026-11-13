#!/bin/bash
# Confere os simbolos importados pelo MaxwellAccel contra o KDK 26.7.2 e o sistema. Roda no hackintosh.
# Uso: bash symcheck.sh imports.txt
K=/Library/Developer/KDKs/KDK_26.7.2_25G332.kdk
T=$(mktemp -d)
sort -u "$1" > $T/imports.txt
ex() { nm -arch x86_64 -gU "$1" 2>/dev/null | awk '{print $3}'; }
ex $K/System/Library/Kernels/kernel | sort -u > $T/kernel.txt
for k in IOAcceleratorFamily2 IOSurface; do ex $K/System/Library/Extensions/$k.kext/Contents/MacOS/$k; done | sort -u > $T/fam.txt
for k in IOGraphicsFamily IOPCIFamily; do
  for b in /System/Library/Extensions/$k.kext/Contents/MacOS/$k /System/Library/Extensions/$k.kext/$k; do [ -f "$b" ] && ex "$b"; done
done | sort -u > $T/gfx.txt
for p in IOKit Libkern Mach BSD; do
  for b in /System/Library/Extensions/System.kext/PlugIns/$p.kext/$p /System/Library/Extensions/System.kext/PlugIns/$p.kext/Contents/MacOS/$p; do
    [ -f "$b" ] && nm -arch x86_64 -g "$b" 2>/dev/null | awk '{print $NF}'
  done
done | sort -u > $T/kpi.txt
echo "kernel $(wc -l <$T/kernel.txt)  kpi publico $(wc -l <$T/kpi.txt)  familia(KDK) $(wc -l <$T/fam.txt)  IOGraphics/IOPCI $(wc -l <$T/gfx.txt)"
sort -u $T/kpi.txt $T/fam.txt $T/gfx.txt > $T/ok.txt
comm -23 $T/imports.txt $T/ok.txt > $T/miss.txt
echo "importados $(wc -l <$T/imports.txt), sem dono publico: $(wc -l <$T/miss.txt)"
comm -12 $T/miss.txt $T/kernel.txt | sed 's/^/KERNEL-PRIVADO /'
comm -23 $T/miss.txt $T/kernel.txt | sed 's/^/AUSENTE /'
rm -rf $T
