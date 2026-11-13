#!/bin/bash
# Desfaz o exp_sem_geforce.sh: volta a AuxKC guardada (com o GeForceWeb). Rodar com sudo e reiniciar.
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
SAVE=$KC.pre-semgeforce
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
[ -f "$SAVE" ] || { echo "nao achei $SAVE"; exit 1; }
kmutil inspect -a x86_64 -A "$SAVE" 2>/dev/null | grep -q 'com.nvidia.web.GeForceWeb' || { echo "$SAVE nao tem o GeForceWeb?!"; exit 1; }
cp -p "$SAVE" "$KC.new" && mv -f "$KC.new" "$KC" || { echo "falhou a copia"; exit 1; }
echo "AuxKC com o GeForceWeb restaurada. Reinicie."
