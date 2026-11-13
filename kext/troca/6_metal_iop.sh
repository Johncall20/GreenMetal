#!/bin/bash
# Metal armado + IOPresentment juntos (o caminho do NullMoth): o WindowServer compoe no nosso Metal e entrega cada
# quadro pelo display pipe do acelerador, que faz o flip do canal base. Ainda nao testado nessa combinacao.
# Se a tela ficar preta e o cursor piscando por mais de 30 s:  sudo sysctl -w debug.nvaccel_iop=0  (ou reiniciar).
# Tudo some no proximo boot.
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
sysctl debug.nvaccelfb >/dev/null 2>&1 || { echo "PARADA: acelerador nao carregado (boot-arg nvaccel=1?)"; exit 1; }
sysctl debug.nvrmfb_agdc >/dev/null 2>&1 && { sysctl -w debug.nvrmfb_agdc_k5=1; sysctl -w debug.nvrmfb_agdc=1; }
sysctl -w debug.nvaccelfb=3
sysctl -w debug.nvaccel_iop=1
sleep 1
ioreg -r -c NVAccel -d 1 -l 2>/dev/null | grep -E "MetalPluginName" | sed 's/^ */   /'
echo "reiniciando o WindowServer (Metal + IOPresentment)..."
launchctl kickstart -k system/com.apple.WindowServer
echo "feito."
