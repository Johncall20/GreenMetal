#!/bin/bash
# Depois do boot com nvaccel=1: liga, em ordem, o caminho da tela pelo acelerador e reinicia o WindowServer.
#   1. AGDC (MaxwellAGDC): o IOPresentment pede informacao da GPU a ele
#   2. debug.nvaccelfb=3: portao de scanout (superficie home na tela pelo canal base) + registro do framebuffer
#   3. debug.nvaccel_iop=1: transacoes do display pipe (os quadros do WindowServer vao para a tela)
#   4. reinicia o WindowServer (fecha a sessao grafica)
# Tudo some no proximo boot. Se o WindowServer entrar em loop: sudo sysctl -w debug.nvaccel_iop=0 (ou reiniciar).
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
sysctl debug.nvrmfb_agdc >/dev/null 2>&1 || { echo "PARADA: MaxwellAGDC nao carregado (sysctl debug.nvrmfb_agdc ausente)"; exit 1; }
sysctl debug.nvaccelfb >/dev/null 2>&1 || { echo "PARADA: acelerador nao carregado (boot-arg nvaccel=1?)"; exit 1; }
sysctl -w debug.nvrmfb_agdc=1
sleep 1
sysctl -w debug.nvaccelfb=3
sleep 1
sysctl -w debug.nvaccel_iop=1
sleep 1
echo "reiniciando o WindowServer..."
launchctl kickstart -k system/com.apple.WindowServer
echo "feito. Logs: ~/maxwell_metal/kext/troca/3_conferir.sh"
