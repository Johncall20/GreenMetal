#!/bin/sh
# Integracao do Metal (rodar com sudo). Para no primeiro problema.
#  1. estado do nvAccelerator
#  2. teste com chave inofensiva: o acelerador aceita propriedade vinda do user space?
#  3. instala /Library/GPUBundles/NVMTLDriver.bundle (root:wheel)
#  4. MetalPluginName=NVMTLDriver no nvAccelerator (vale ate o reboot)
#  5. mtlprobe: MTLCopyAllDevices + kernel MSL na GTX 960
#  6. MetalPluginName=NVMTLDisabled de novo (desliga; o bundle fica, inofensivo sem o nome)
# Sem nvmtl-allow.txt o plugin so cria device para mtlprobe/nvmtltest/nvmtlrender: os demais processos
# (WindowServer inclusive) recebem "sem device", igual a hoje.
set -u
D=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 2; }
echo "== 1. estado"; "$D/metalprop" status
echo "== 2. teste de propriedade inofensiva"
"$D/metalprop" probe | tee /tmp/metalprop_probe.txt
grep -q 'ACEITA propriedades' /tmp/metalprop_probe.txt || { echo "PARADA: o acelerador recusa propriedades do user space -> precisa do kext minimo"; exit 3; }
[ ! -e /Library/GPUBundles/nvmtl-allow.txt ] || { echo "PARADA: ja existe /Library/GPUBundles/nvmtl-allow.txt (nao mexo)"; exit 4; }
echo "== 3. bundle"
mkdir -p /Library/GPUBundles && rm -rf /Library/GPUBundles/NVMTLDriver.bundle && ditto "$D/NVMTLDriver.bundle" /Library/GPUBundles/NVMTLDriver.bundle && chown -R root:wheel /Library/GPUBundles/NVMTLDriver.bundle && chmod -R go-w,a+rX /Library/GPUBundles/NVMTLDriver.bundle || { echo "PARADA: copia do bundle"; exit 5; }
ls -la /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS
echo "== 4. MetalPluginName"; "$D/metalprop" on
echo "== 5. mtlprobe"; NVMTL_LOG_PATH="$D/nvmtl-integ.log" "$D/mtlprobe"; rc=$?
echo "== 6. desliga o nome"; "$D/metalprop" off
echo "mtlprobe rc=$rc  (log do plugin: $D/nvmtl-integ.log)"
exit $rc
