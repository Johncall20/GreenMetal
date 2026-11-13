#!/bin/bash
# Abre o Chrome num perfil separado com o processo de GPU fora da sandbox, para ele poder abrir o NVDAUser e
# carregar o NVK de ~/maxwell_metal/nvmtl. Feche o Chrome normal antes. Depois abra chrome://gpu.
# Log do plugin: ~/maxwell_metal/kext/deploy/nvmtl-chrome.log
set -u
D=$(cd "$(dirname "$0")" && pwd)
[ "$(sysctl -n debug.maxwell_metal 2>/dev/null)" = 1 ] || { echo "Metal desarmado: sudo sysctl -w debug.maxwell_metal=1"; exit 1; }
grep -qx 'Google Chrome Helper (Aperitif)' /Library/GPUBundles/nvmtl-allow.txt 2>/dev/null || { echo "rode antes: sudo ./allowlist_install.sh"; exit 1; }
export NVMTL_LOG_PATH="$D/nvmtl-chrome.log"
exec "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --user-data-dir="$HOME/.chrome-gtx960" --disable-gpu-sandbox --use-angle=metal --enable-logging=stderr --v=0 \
  chrome://gpu 2> "$D/chrome-gtx960.log"
