#!/bin/bash
# VOLTA da troca: o sistema como estava antes (Non-Metal do OCLP + GeForceWeb + MaxwellMetal). Rodar com sudo; funciona
# por ssh com a tela preta (ssh no hackintosh a partir do MacBook). Depois: reiniciar. Ultimo recurso: o clone.
#   - recoloca o auto-patch do OCLP, o CoreDisplay (useMetal/useIOP = NO) e a lista do plugin de antes;
#   - tira o MaxwellAccel de /Library/Extensions (guardado em backup/);
#   - recoloca a AuxKC de antes (com o GeForceWeb);
#   - reaplica os patches do OCLP no sistema (OpenCore-Patcher --patch_sys_vol).
# Se depois do boot a tela vier sem aceleracao: sudo ~/maxwell_metal/kext/deploy/install_maxwellmetal.sh e reiniciar.
set -u
D=$(cd "$(dirname "$0")" && pwd)
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
PRE=$KC.pre-troca; BK=$D/backup; OFF=$BK/launchd-oclp
GB=/Library/GPUBundles
OCLP=/Applications/OpenCore-Patcher.app/Contents/MacOS/OpenCore-Patcher
die() { echo "PARADA: $*"; exit 1; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
[ -f "$PRE" ] || die "nao achei $PRE (a AuxKC de antes da troca)"
[ -x "$OCLP" ] || die "OpenCore-Patcher ausente em $OCLP"

echo "== 1. auto-patch do OCLP de volta"
for f in "$OFF"/*.plist; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in *auto-patch*) dst=/Library/LaunchAgents;; *) dst=/Library/LaunchDaemons;; esac
  cp -p "$f" "$dst/" && chown root:wheel "$dst/$(basename "$f")" && echo "   $dst/$(basename "$f")"
done

echo "== 2. CoreDisplay e lista do plugin de antes"
if [ -f "$BK/com.apple.CoreDisplay.plist.pre-troca" ]; then cp -p "$BK/com.apple.CoreDisplay.plist.pre-troca" /Library/Preferences/com.apple.CoreDisplay.plist
else defaults write /Library/Preferences/com.apple.CoreDisplay useMetal -boolean no; defaults write /Library/Preferences/com.apple.CoreDisplay useIOP -boolean no; fi
defaults read /Library/Preferences/com.apple.CoreDisplay 2>/dev/null | sed 's/^/   /'
[ -f "$BK/nvmtl-allow.txt.pre-troca" ] && cp -p "$BK/nvmtl-allow.txt.pre-troca" "$GB/nvmtl-allow.txt"

echo "== 3. MaxwellAccel fora de /Library/Extensions"
if [ -d /Library/Extensions/MaxwellAccel.kext ]; then
  rm -rf "$BK/MaxwellAccel.kext"; mv /Library/Extensions/MaxwellAccel.kext "$BK/" && echo "   guardado em $BK"
fi

echo "== 4. AuxKC de antes"
cp "$PRE" "$KC.new" && chown root:wheel "$KC.new" && chmod 744 "$KC.new" && mv -f "$KC.new" "$KC" || die "recolocar a AuxKC"
kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | grep -q com.nvidia.web.GeForceWeb && echo "   ok (com o GeForceWeb)"

echo "== 5. patches do OCLP no sistema (alguns minutos)"
"$OCLP" --patch_sys_vol 2>&1 | tail -25 | sed 's/^/   /'
echo
echo "PRONTO. Reinicie (tire nvaccel=1 nvaccelarm=1 dos boot-args, se quiser). Sem aceleracao depois do boot:"
echo "   sudo ~/maxwell_metal/kext/deploy/install_maxwellmetal.sh   e reiniciar de novo."
