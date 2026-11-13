#!/bin/bash
# Vigia do Metal total. Uso (root): guarda_ws.sh <pid do WindowServer que esta no Metal>
# Se esse WindowServer morrer, tira o Metal do WindowServer na lista do plugin, deixa as preferencias no caminho por CPU e
# desliga o IOPresentment (o painel volta para a home). O proximo WindowServer que o launchd subir vem pela CPU, sem Metal.
# Motivo (2026-11-12): o WindowServer no Metal abortou por falta de VRAM; os seguintes subiram no caminho por CPU ainda com
# o Metal, cairam 13 vezes seguidas e o watchdog deu panic. Um que suba antes da troca da lista cai sozinho em segundos.
P=${1:-}
D=$(cd "$(dirname "$0")/.." && pwd)
GB=/Library/GPUBundles
CD=/Library/Preferences/com.apple.CoreDisplay
LOG=/var/log/maxwell_guarda_ws.log
[ "$(id -u)" -eq 0 ] && [ -n "$P" ] && [ -f "$D/nvmtl-allow-troca.txt" ] || exit 1
echo "$(date '+%F %T') vigiando o WindowServer $P" >> "$LOG"
while kill -0 "$P" 2>/dev/null; do sleep 0.2; done
sed 's/^!WindowServer$/-WindowServer/' "$D/nvmtl-allow-troca.txt" > "$GB/nvmtl-allow.txt.new" \
  && chown root:wheel "$GB/nvmtl-allow.txt.new" && chmod 644 "$GB/nvmtl-allow.txt.new" && mv -f "$GB/nvmtl-allow.txt.new" "$GB/nvmtl-allow.txt"
defaults write $CD useIOP -boolean NO
defaults write $CD useMetal -boolean NO
sysctl -w debug.nvaccel_iop_flip=0 >/dev/null 2>&1
sysctl -w debug.nvaccel_iop=0 >/dev/null 2>&1
{
  echo "$(date '+%F %T') o WindowServer $P morreu: lista sem o WindowServer, IOPresentment desligado"
  echo "   lista: $(grep -E '^[-!]?WindowServer$' "$GB/nvmtl-allow.txt")  BAR1 $(sysctl -n debug.mx_vram_mapped_kb 2>/dev/null) KB  sem mapa $(sysctl -n debug.mx_vram_unmapped_kb 2>/dev/null) KB  recusas $(sysctl -n debug.mx_vram_refused 2>/dev/null)  flips $(sysctl -n debug.nvaccel_iop_flips 2>/dev/null)  linha larga $(sysctl -n debug.nvaccel_wide_granted 2>/dev/null)/$(sysctl -n debug.nvaccel_wide_refused 2>/dev/null)"
  sleep 30
  echo "$(date '+%F %T') 30 s depois: WindowServer pid $(pgrep -x WindowServer | head -1)"
} >> "$LOG" 2>&1
