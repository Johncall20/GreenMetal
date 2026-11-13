#!/bin/bash
# Depois do reboot: confere o kext, arma o Metal e roda o mtlprobe SEM root.
# Uso: ./pos_boot.sh        (vai pedir sudo so para o sysctl)
set -u
D=$(cd "$(dirname "$0")" && pwd)
echo "== 1. kext carregado?"
kmutil showloaded --list-only 2>/dev/null | grep -i maxwell || kextstat 2>/dev/null | grep -i maxwell || { echo "PARADA: MaxwellMetal nao carregou"; exit 1; }
log show --last 10m --predicate 'eventMessage CONTAINS "MaxwellMetal"' --style compact 2>/dev/null | tail -5
sysctl debug.maxwell_metal || { echo "PARADA: sysctl ausente (kext inerte? -mmeoff?)"; exit 1; }
echo "== 2. arma o MetalPluginName (sudo)"
sudo sysctl -w debug.maxwell_metal=1 || exit 1
ioreg -r -c nvAccelerator -d 1 | grep -E 'MetalPlugin' 
echo "== 3. mtlprobe como $(id -un) (uid $(id -u))"
NVMTL_LOG_PATH="$D/nvmtl-posboot.log" "$D/mtlprobe"; rc=$?
echo "mtlprobe rc=$rc (log: $D/nvmtl-posboot.log)"
echo "O Metal fica armado ate: sudo sysctl -w debug.maxwell_metal=0 (ou reboot)."
exit $rc
