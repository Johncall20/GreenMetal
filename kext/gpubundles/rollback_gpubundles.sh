#!/bin/bash
# Volta o plugin anterior (pilha em ~/maxwell_metal/nvmtl). Rodar com sudo.
set -u; GB=/Library/GPUBundles
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 2; }
[ -d "$GB/NVMTLDriver.bundle.pre-lib" ] || { echo "sem backup $GB/NVMTLDriver.bundle.pre-lib"; exit 1; }
rm -rf "$GB/NVMTLDriver.bundle" && ditto "$GB/NVMTLDriver.bundle.pre-lib" "$GB/NVMTLDriver.bundle" && echo "plugin anterior de volta (o $GB/nvmtl fica, sem uso)"
