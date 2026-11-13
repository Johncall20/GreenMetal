#!/bin/bash
# METAL TOTAL: o WindowServer compoe pelo Metal da GTX 960 e entrega cada quadro pelo IOPresentment (display pipe do
# acelerador), que copia o quadro para a superficie que o canal base EVO mostra (MaxwellFB).
# Teste com VOLTA AUTOMATICA: se o WindowServer nao subir, cair em loop ou nenhum quadro chegar na tela em 45 s, o
# script volta sozinho para a tela por CPU. As preferencias do caminho por CPU voltam para o disco assim que o
# WindowServer novo sobe, entao um reboot (ou o panic do watchdog) tambem volta para a tela por CPU.
#   sudo ~/maxwell_metal/kext/troca/10_metal_total.sh          roda o teste
#   sudo ~/maxwell_metal/kext/troca/10_metal_total.sh volta    volta para a tela por CPU na mao
#   sudo SURFSHARE=1 ~/maxwell_metal/kext/troca/10_metal_total.sh   idem, com o plugin desenhando na VRAM da superficie
#   PERFIL=1 grava amostras de CPU do WindowServer antes e depois; FLIP=1 liga debug.nvaccel_iop_flip
#   FLIP SEM COPIA (MaxwellAccel 0.4.4 + MaxwellMetal 0.7.3 + plugin com linha larga):
#     sudo SURFSHARE=3 FLIP=1 PERFIL=1 ~/maxwell_metal/kext/troca/10_metal_total.sh
#   SURFSHARE=3 = so as superficies de tela do WindowServer vao para a VRAM compartilhada (com o stride do scanout);
#   se o canal base travar ou os quadros cairem no page-off lento, o script volta sozinho para a tela por CPU.
# Registro: ~/maxwell_metal/logs/metal_total/ (resumo.txt, ws.log, kern.log)
#
# Licoes de 2026-11-11 (as duas travaram a maquina):
#  - no caminho por CPU o WindowServer NAO pode ter o Metal: com o acelerador armado ele ativa o "Metal compositor",
#    compoe pelo CompositorSW e aborta em CA::OGL::Context::push_surface, em loop. O arme nao tem volta sem reboot,
#    entao quem tira o Metal do WindowServer e a lista do plugin (-WindowServer).
#  - depois de um loop de SIGABRT o launchd segura o respawn do WindowServer; `launchctl kickstart -k` fica esperando
#    e, com 120 s sem WindowServer, o watchdog da panic. Por isso: nunca rodar com o WindowServer caido, e o kickstart
#    vai em segundo plano com prazo.
set -u
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
U=${SUDO_USER:-$(stat -f %Su /dev/console)}
UH=$(eval echo "~$U")
CD=/Library/Preferences/com.apple.CoreDisplay
D=$(cd "$(dirname "$0")" && pwd)
GB=/Library/GPUBundles
L="$UH/maxwell_metal/logs/metal_total"
ESPERA=${ESPERA:-45}

wspid() { pgrep -x WindowServer | head -1; }
ctr() { sysctl -n "$1" 2>/dev/null || echo "?"; }

# quadros por segundo no pipe, medidos em $1 segundos
fps() { local a b; a=$(ctr debug.nvaccel_iop_ok); sleep "$1"; b=$(ctr debug.nvaccel_iop_ok); echo $(( (b - a) / $1 )); }
# perfil de CPU do WindowServer (PERFIL=1): 4 s de amostras em $L/$1
perfil() {
  [ "${PERFIL:-0}" = 1 ] || return 0
  local p; p=$(wspid); [ -n "$p" ] || return 0
  /usr/bin/sample "$p" 4 -file "$L/$1" >/dev/null 2>&1 && echo "   perfil: $L/$1"
}

# o WindowServer esta de pe e com o mesmo pid ha $1 segundos?
ws_estavel() {
  local a b; a=$(wspid); [ -n "$a" ] || return 1
  sleep "$1"; b=$(wspid); [ "$a" = "$b" ]
}

# reinicia o WindowServer sem ficar preso no launchctl; devolve 0 quando aparece um pid novo (prazo $1 s)
reinicia_ws() {
  local velho novo t=0
  velho=$(wspid)
  launchctl kickstart -k system/com.apple.WindowServer >/dev/null 2>&1 &
  while [ $t -lt "$1" ]; do
    sleep 1; t=$((t+1))
    novo=$(wspid)
    [ -n "$novo" ] && [ "$novo" != "${velho:-0}" ] && return 0
  done
  return 1
}

legado_no_disco() {
  defaults write $CD useIOP -boolean NO
  defaults write $CD useMetal -boolean NO
  sync
}

lista() {
  local src="$D/nvmtl-allow-troca.txt"
  [ -f "$src" ] || { echo "   AVISO: $src ausente, lista do plugin nao alterada"; return 1; }
  if [ "$1" = semws ]; then sed 's/^!WindowServer$/-WindowServer/' "$src" > "$GB/nvmtl-allow.txt.new"
  else cp "$src" "$GB/nvmtl-allow.txt.new"; fi
  chown root:wheel "$GB/nvmtl-allow.txt.new" && chmod 644 "$GB/nvmtl-allow.txt.new" && mv -f "$GB/nvmtl-allow.txt.new" "$GB/nvmtl-allow.txt"
  echo "   lista do plugin: $(grep -E '^[-!]?WindowServer$' "$GB/nvmtl-allow.txt")"
}

volta() {
  echo "== VOLTA: tela por CPU (useIOP=NO, useMetal=NO), WindowServer sem Metal"
  pkill -f "ws/guarda_ws.sh" 2>/dev/null   # o vigia do WindowServer anterior nao pode agir no meio desta troca
  legado_no_disco
  sysctl -w debug.nvaccel_iop=0 >/dev/null
  lista semws
  if reinicia_ws 25; then
    sleep 12
    sysctl -w debug.nvaccelfb=1 >/dev/null
    if ws_estavel 12; then echo "   WindowServer estavel (pid $(wspid)); Metal para apps armado. A area de trabalho por CPU deve estar na tela."; return 0; fi
  fi
  echo "   WindowServer NAO ficou de pe (pid $(wspid)). Reinicie AGORA, antes do watchdog:  sync && sudo reboot"
  return 1
}

if [ "${1:-}" = volta ]; then
  [ -n "$(wspid)" ] || { echo "PARADA: o WindowServer esta caido (launchd segurando o respawn). Use:  sync && sudo reboot"; legado_no_disco; lista semws; exit 1; }
  volta; exit $?
fi

sysctl debug.nvaccelfb >/dev/null 2>&1 || { echo "PARADA: acelerador nao carregado (boot-arg nvaccel=1?)"; exit 1; }
sysctl debug.nvrmfb_agdc >/dev/null 2>&1 || { echo "PARADA: MaxwellAGDC nao carregado"; exit 1; }
ioreg -r -c MaxwellFB -d 1 2>/dev/null | grep -q "+-o MaxwellFB" || { echo "PARADA: MaxwellFB nao esta no ar neste boot"; exit 1; }
IDX=$(ioreg -r -c MaxwellFB -d 1 -l -w0 2>/dev/null | awk '/"IOAccelIndex"/ {print $NF}')
[ -n "$IDX" ] || { echo "PARADA: MaxwellFB sem IOAccelIndex (o WindowServer ainda nao abriu ele)"; exit 1; }
PRIM=$(ioreg -r -c AppleGraphicsDeviceControl -w0 -d1 | grep "+-o" | grep -v AppleGPUWrangler | head -1)
case "$PRIM" in *NVDADeviceControl*) ;; *) echo "PARADA: o primeiro AGDC nao e o nosso: $PRIM"; exit 1;; esac
echo "conferindo se o WindowServer esta estavel (8 s)..."
ws_estavel 8 || { echo "PARADA: o WindowServer esta caido ou caindo. Nao rodo o teste assim (trava a maquina). Use:  sync && sudo reboot"; exit 1; }

rm -rf "$L"; mkdir -p "$L"
R="$L/resumo.txt"
say() { echo "$@" | tee -a "$R"; }

say "== antes ($(date '+%F %T'))"
say "   MaxwellFB IOAccelIndex $IDX; WindowServer pid $(wspid); debug.nvaccelfb $(ctr debug.nvaccelfb)"
say "   fbmap: $(ctr debug.nvrmfb_agdc_fbmap)"
say "   iop_ok $(ctr debug.nvaccel_iop_ok) iop_fail $(ctr debug.nvaccel_iop_fail) k5_link $(ctr debug.nvrmfb_agdc_k5_link) k5_caps $(ctr debug.nvrmfb_agdc_k5_caps)"
OK0=$(ctr debug.nvaccel_iop_ok)
if [ "$(ctr debug.nvaccel_iop)" = 1 ]; then
  say "   ja no Metal: $(fps 5) quadros/s agora (surfshare $(ctr debug.nvaccel_surfshare), flip $(ctr debug.nvaccel_iop_flip))"
  perfil ws_antes.txt | tee -a "$R"
fi

# `script -F` grava linha a linha: o registro sobrevive a um panic (com o redirecionamento simples ele se perdeu)
script -q -F "$L/ws.log" /usr/bin/log stream --level debug --style compact --predicate 'process == "WindowServer"' </dev/null >/dev/null 2>&1 &
LS1=$!
script -q -F "$L/kern.log" /usr/bin/log stream --level debug --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "nvaccel" OR eventMessage CONTAINS[c] "maxwell" OR eventMessage CONTAINS[c] "nvrmfb" OR eventMessage CONTAINS[c] "agdc" OR eventMessage CONTAINS "iop:" OR eventMessage CONTAINS[c] "IOPresentment" OR eventMessage CONTAINS[c] "NVMTL")' </dev/null >/dev/null 2>&1 &
LS2=$!
fim_logs() { pkill -P $LS1 2>/dev/null; pkill -P $LS2 2>/dev/null; kill $LS1 $LS2 2>/dev/null; chown -R "$U" "$UH/maxwell_metal/logs" 2>/dev/null; sync; }
T0=$(date '+%F %T')
sleep 2

# um vigia de um teste anterior dispararia quando o WindowServer atual for reiniciado e desfaria o que vem a seguir
pkill -f "ws/guarda_ws.sh" 2>/dev/null && say "   vigia anterior encerrado"
say "== ligando: AGDC K5, scanout do acelerador, IOPresentment"
sysctl -w debug.nvrmfb_agdc_k5=1 >/dev/null
sysctl -w debug.nvrmfb_agdc=1 >/dev/null
sysctl -w debug.nvaccelfb=3 >/dev/null
sysctl -w debug.nvaccel_iop=1 >/dev/null
# opcoes do MaxwellAccel 0.4.2 (ausentes na 0.4.1: o sysctl so reclama):  SURFSHARE=1  L10R=0|1|2
[ -n "${SURFSHARE:-}" ] && sysctl -w debug.nvaccel_surfshare="$SURFSHARE" 2>&1 | tee -a "$R"
[ -n "${L10R:-}" ] && sysctl -w debug.nvaccel_iop_l10r="$L10R" 2>&1 | tee -a "$R"
[ -n "${FLIP:-}" ] && sysctl -w debug.nvaccel_iop_flip="$FLIP" 2>&1 | tee -a "$R"
lista comws | tee -a "$R"
defaults delete $CD useIOP 2>/dev/null
defaults delete $CD useMetal 2>/dev/null
sleep 1
say "   MetalPluginName: $(ioreg -r -c NVAccel -d 1 -l -w0 2>/dev/null | awk -F'= ' '/"MetalPluginName"/ {print $2}')"
say "== reiniciando o WindowServer (Metal + IOPresentment)"
sync
if ! reinicia_ws 25; then
  say "== RESULTADO: NAO FOI (o WindowServer novo nao apareceu em 25 s)"
  fim_logs; volta | tee -a "$R"; chown -R "$U" "$UH/maxwell_metal/logs" 2>/dev/null
  exit 2
fi
sleep 3
P0=$(wspid)
# o WindowServer novo ja leu as preferencias: o caminho por CPU volta para o disco (reboot/panic = tela por CPU)
legado_no_disco
say "   WindowServer pid ${P0:-nenhum}"

QUEDAS=0; PA=$P0; T=0
while [ $T -lt $ESPERA ]; do
  sleep 3; T=$((T+3))
  P=$(wspid)
  if [ "${P:-0}" != "${PA:-0}" ]; then QUEDAS=$((QUEDAS+1)); PA=$P; fi
  say "   t=${T}s pid ${P:-nenhum} quedas $QUEDAS iop_ok $(ctr debug.nvaccel_iop_ok) fail $(ctr debug.nvaccel_iop_fail) vazio $(ctr debug.nvaccel_iop_empty) branco $(ctr debug.nvaccel_iop_blank) l10r $(ctr debug.nvaccel_iop_l10r_frames) ult nz $(ctr debug.nvaccel_iop_last_nz)/64 var $(ctr debug.nvaccel_iop_last_varied)/64"
  sync
  [ $QUEDAS -ge 2 ] && break
done
OK1=$(ctr debug.nvaccel_iop_ok)
NZ=$(ctr debug.nvaccel_iop_last_nz)

say "== contadores"
for s in nvaccel_iop_ok nvaccel_iop_fail nvaccel_iop_empty nvaccel_iop_blank nvaccel_iop_src_res nvaccel_iop_src_surf \
         nvaccel_iop_copy_skip nvaccel_iop_l10r nvaccel_iop_l10r_frames nvaccel_iop_last_fmt nvaccel_iop_last_nz \
         nvaccel_iop_last_varied nvaccel_iop_last_first nvaccel_surfshare nvaccel_surfshare_seen nvaccel_heads_published nvaccel_heads_registered nvaccel_heads_piped \
         nvrmfb_agdc_cmds nvrmfb_agdc_refused nvrmfb_agdc_k5_link nvrmfb_agdc_k5_caps nvrmfb_agdc_fbmap \
         mxflip_iso mxflip_exc mxflip_code mxflip_count mxflip_stuck nvaccel_iop_flip nvaccel_iop_flips nvaccel_iop_flip_home \
         nvaccel_iop_flip_novram nvaccel_iop_flip_stale nvaccel_iop_flip_fmt nvaccel_iop_flip_geom nvaccel_iop_flip_refused \
         nvaccel_iop_flip_unwritten nvaccel_wide nvaccel_wide_pitch nvaccel_wide_granted nvaccel_wide_refused nvaccel_wide_pageoff \
         nvaccel_pageoff_cpu nvaccel_pageoff_cpu_us mx_vram_mapped_kb mx_vram_unmapped_kb mx_vram_temp_maps mx_vram_refused nvaccel_vm_refused nvaccel_wide_tempmap; do
  say "   debug.$s = $(ctr debug.$s)"
done
say "== WindowServer: linhas-chave"
grep -aiE "compositor|IOPresentment|display ?pipe|No matching|capabilit|vk: UP|software|page flip|online|offline|abort|assert|transient" "$L/ws.log" | cut -c1-300 | head -120 >> "$R"
say "== relatorios de queda novos"
find /Library/Logs/DiagnosticReports -name "WindowServer*" -newermt "$T0" 2>/dev/null | tee -a "$R"

if [ $QUEDAS -ge 2 ] || [ -z "$(wspid)" ] || [ "$OK1" = "$OK0" ] || [ "$NZ" = 0 ]; then
  say "== RESULTADO: NAO FOI (quedas $QUEDAS, quadros no pipe $OK0 -> $OK1, ultimo quadro com $NZ/64 amostras nao pretas)"
  fim_logs
  volta | tee -a "$R"
  chown -R "$U" "$UH/maxwell_metal/logs" 2>/dev/null
  echo "Registro em $L."
  exit 2
fi
say "== RESULTADO: WindowServer estavel e quadros chegando pelo pipe ($OK0 -> $OK1)"
# vigia: se este WindowServer morrer depois, o proximo sobe pela CPU e sem Metal (senao cai em loop ate o panic do watchdog)
if [ -x "$D/ws/guarda_ws.sh" ]; then
  nohup "$D/ws/guarda_ws.sh" "$(wspid)" >/dev/null 2>&1 &
  disown 2>/dev/null
  say "   vigia de queda do WindowServer ligado (registro em /var/log/maxwell_guarda_ws.log)"
fi
# flip sem copia: o canal base tem que consumir os flips, e quadro de superficie larga nao pode cair no page-off lento.
# Conferido 10 s depois do WindowServer subir e de novo depois da espera do PERFIL (a area de trabalho so vem apos o login).
confere_flip() {
  [ "${FLIP:-0}" = 1 ] || return 0
  local ST WP
  ST=$(ctr debug.mxflip_stuck); WP=$(ctr debug.nvaccel_wide_pageoff)
  say "   flip ($1): $(ctr debug.nvaccel_iop_flips) quadros por flip, $(ctr debug.mxflip_count) flips no canal base, travados ${ST:-?}, page-offs de linha larga ${WP:-?}, linha larga atendida $(ctr debug.nvaccel_wide_granted) recusada $(ctr debug.nvaccel_wide_refused), BAR1 $(ctr debug.mx_vram_mapped_kb) KB, sem mapa $(ctr debug.mx_vram_unmapped_kb) KB"
  if [ "${ST:-0}" -gt 0 ] 2>/dev/null || [ "${WP:-0}" -gt 20 ] 2>/dev/null; then
    say "== RESULTADO: FLIP NAO FOI (canal base travado ou quadros caindo no page-off lento) -- voltando para a tela por CPU"
    sysctl -w debug.nvaccel_iop_flip=0 >/dev/null 2>&1
    fim_logs
    volta | tee -a "$R"
    chown -R "$U" "$UH/maxwell_metal/logs" 2>/dev/null
    echo "Registro em $L."
    exit 3
  fi
}
[ "${FLIP:-0}" = 1 ] && sleep 10
confere_flip "inicio"
if [ "${PERFIL:-0}" = 1 ]; then
  echo "   esperando 40 s para a area de trabalho carregar antes de medir (entre na sessao se pedir senha)..."
  sleep 40
  say "   flips: $(ctr debug.nvaccel_iop_flips) (canal base $(ctr debug.mxflip_count), travados $(ctr debug.mxflip_stuck), excecao $(ctr debug.mxflip_exc))"
  say "   depois: $(fps 5) quadros/s (surfshare $(ctr debug.nvaccel_surfshare) visto $(ctr debug.nvaccel_surfshare_seen), l10r $(ctr debug.nvaccel_iop_l10r_frames), pulados $(ctr debug.nvaccel_iop_copy_skip), ult nz $(ctr debug.nvaccel_iop_last_nz)/64)"
  perfil ws_depois.txt | tee -a "$R"
  confere_flip "depois de 40 s"
  if [ "${FLIP:-0}" = 1 ]; then sleep 60; say "   depois de mais 60 s: $(fps 5) quadros/s"; confere_flip "depois de 100 s"; fi
fi
fim_logs
echo "Olhe o monitor. Se a area de trabalho estiver certa, deixe assim."
echo "Se estiver preta ou errada:  sudo ~/maxwell_metal/kext/troca/10_metal_total.sh volta"
