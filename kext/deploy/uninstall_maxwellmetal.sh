#!/bin/bash
# Remove o MaxwellMetal.kext e o NVMTLDriver.bundle e volta a AuxKC do backup. Rodar com sudo; depois reiniciar.
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 2; }
rm -rf /Library/Extensions/MaxwellMetal.kext /Library/GPUBundles/NVMTLDriver.bundle
if [ -f "$KC.maxwell-backup" ]; then cp -p "$KC.maxwell-backup" "$KC" && echo "AuxKC restaurada do backup"; else echo "sem backup da AuxKC"; fi
echo "Reinicie."
