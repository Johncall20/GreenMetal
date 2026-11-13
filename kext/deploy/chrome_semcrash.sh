#!/bin/bash
# Abre o SEU Chrome (perfil normal) sem o "crash on exception" do AppKit: uma excecao no desenho do icone do Dock
# (bug do SkyLight Non-Metal do OCLP) vira linha de log em vez de derrubar o navegador.
# Feche o Chrome antes. Log: ~/maxwell_metal/kext/deploy/chrome-semcrash.log
D=$(cd "$(dirname "$0")" && pwd)
pgrep -x "Google Chrome" >/dev/null && { echo "feche o Chrome antes (Cmd+Q)"; exit 1; }
open -n -a "Google Chrome" --stderr "$D/chrome-semcrash.log" --args -NSApplicationCrashOnExceptions NO
echo "Chrome aberto. Tente o download; depois me avise para eu ler o log."
