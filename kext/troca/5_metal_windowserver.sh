#!/bin/bash
# Depois do boot com nvaccel=1: abre o scanout, ARMA o Metal (MetalPluginName no acelerador) e reinicia o WindowServer
# para ele compor pelo Metal da GTX 960 (plugin NVMTLDriver + NVK). Sem IOPresentment (debug.nvaccel_iop fica 0): a
# troca legada faz o flip do canal base para o buffer que o Metal do WindowServer pintou.
# Tudo some no proximo boot. Logs: ~/maxwell_metal/kext/troca/3_conferir.sh  e  /tmp/nvmtl.log
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
sysctl debug.nvaccelfb >/dev/null 2>&1 || { echo "PARADA: acelerador nao carregado (boot-arg nvaccel=1?)"; exit 1; }
sysctl -w debug.nvaccel_iop=0
sysctl -w debug.nvaccelfb=3
sleep 1
ioreg -r -c NVAccel -d 1 -l 2>/dev/null | grep -E "MetalPluginName|IOGLBundleName" | sed 's/^ */   /'
echo "reiniciando o WindowServer (com Metal)..."
launchctl kickstart -k system/com.apple.WindowServer
echo "feito."
