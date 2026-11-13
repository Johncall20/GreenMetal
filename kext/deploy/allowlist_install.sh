#!/bin/bash
# Instala a lista de apps do Metal da GTX 960. Rodar com sudo. Desfazer: sudo rm /Library/GPUBundles/nvmtl-allow.txt
set -u
D=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 2; }
install -o root -g wheel -m 644 "$D/nvmtl-allow.txt" /Library/GPUBundles/nvmtl-allow.txt || exit 1
ls -la /Library/GPUBundles/
grep -v -E '^\s*(#|$)' /Library/GPUBundles/nvmtl-allow.txt | sed 's/^/   /'
echo "ok. Metal armado agora? $(sysctl -n debug.maxwell_metal 2>/dev/null)"
