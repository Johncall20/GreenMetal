#!/bin/bash
# Tela pelo caminho ANTIGO do CoreDisplay (sem IOPresentment), o mesmo que o Non-Metal do OCLP usa (useIOP=NO,
# useMetal=NO): o WindowServer compoe na CPU direto na memoria do MaxwellFB, que e a superficie que o canal base EVO
# ja mostra no monitor. Metal para os apps continua pelo NVAccel (armado depois que o WindowServer sobe).
# Volta ao caminho novo:  sudo ~/maxwell_metal/kext/troca/8_tela_legado.sh volta
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
CD=/Library/Preferences/com.apple.CoreDisplay
if [ "${1:-}" = volta ]; then
  defaults delete $CD useIOP 2>/dev/null; defaults delete $CD useMetal 2>/dev/null
  echo "CoreDisplay sem useIOP/useMetal (caminho novo). Reinicie o WindowServer ou a maquina."; exit 0
fi
ioreg -r -c MaxwellFB -d 1 2>/dev/null | grep -q "+-o MaxwellFB" || { echo "PARADA: MaxwellFB nao esta no ar neste boot"; exit 1; }
defaults write $CD useIOP -boolean NO
defaults write $CD useMetal -boolean NO
defaults read $CD | sed 's/^/   /'
sysctl -w debug.nvaccel_iop=0
echo "reiniciando o WindowServer (caminho antigo, composicao na CPU)..."
launchctl kickstart -k system/com.apple.WindowServer
sleep 15
sysctl -w debug.nvaccelfb=1 >/dev/null && echo "Metal para apps armado (apps abertos daqui pra frente)"
echo "feito. Diga o que aparece na tela."
