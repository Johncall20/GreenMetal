#!/bin/bash
# Instala a lista de processos do plugin Metal (nvmtl-allow-troca.txt) em /Library/GPUBundles e reinicia os processos
# de papel de parede, que passam a desenhar pela CPU. Rodar com sudo. Nao precisa reiniciar a maquina.
# Volta: sudo cp ~/maxwell_metal/kext/troca/backup/nvmtl-allow.txt.antes /Library/GPUBundles/nvmtl-allow.txt
set -u
D=$(cd "$(dirname "$0")" && pwd)
GB=/Library/GPUBundles
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
[ -f "$D/nvmtl-allow-troca.txt" ] || { echo "PARADA: $D/nvmtl-allow-troca.txt ausente"; exit 1; }
mkdir -p "$D/backup"
[ -f "$GB/nvmtl-allow.txt" ] && cp -p "$GB/nvmtl-allow.txt" "$D/backup/nvmtl-allow.txt.antes"
cp "$D/nvmtl-allow-troca.txt" "$GB/nvmtl-allow.txt.new" && chown root:wheel "$GB/nvmtl-allow.txt.new" \
  && chmod 644 "$GB/nvmtl-allow.txt.new" && mv -f "$GB/nvmtl-allow.txt.new" "$GB/nvmtl-allow.txt" || { echo "PARADA: copia"; exit 1; }
echo "== lista instalada (regras de papel de parede):"
grep -E "^-" "$GB/nvmtl-allow.txt" | sed 's/^/   /'
echo "== reiniciando os processos de papel de parede"
for p in WallpaperImageExtension WallpaperAerialsExtension WallpaperLegacyExtension NeptuneOneWallpaper Wallpaper WallpaperAgent; do
  killall "$p" 2>/dev/null && echo "   $p"
done
echo "feito. O fundo deve aparecer em alguns segundos."
