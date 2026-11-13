#!/bin/bash
# Tira o MaxwellFB (volta a tela para o NVDA, como antes). Rodar com sudo; funciona por ssh com a tela preta.
# O acelerador e o AGDC voltam sozinhos ao NVDA quando o MaxwellFB nao esta carregado. Reiniciar depois do PRONTO.
set -u
D=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
mkdir -p "$D/backup"
if [ -d /Library/Extensions/MaxwellFB.kext ]; then
  rm -rf "$D/backup/MaxwellFB.kext"; mv /Library/Extensions/MaxwellFB.kext "$D/backup/" && echo "== MaxwellFB guardado em $D/backup"
fi
rm -f /Library/LaunchDaemons/local.maxwell.mxfb-ws.plist /usr/local/libexec/maxwell/mxfb_ws.sh && echo "== LaunchDaemon mxfb-ws removido"
SKIP_FB=1 exec "$D/../deploy/install_maxwellmetal.sh"
