#!/bin/bash
# TROCA, passo 2 (o que muda o proximo boot). Rodar com sudo, depois do 1_preparar.sh, com o CLONE pronto.
#   - desliga o auto-patch do OCLP (senao ele reaplica o Non-Metal no login);
#   - apaga o "useMetal=NO / useIOP=NO" que o OCLP gravou no CoreDisplay;
#   - lista do plugin com o WindowServer liberado quando o acelerador estiver armado;
#   - volta o volume do sistema para o snapshot selado original (bless --last-sealed-snapshot: tira TODOS os patches
#     do OCLP no sistema; /Library e os dados ficam);
#   - publica a AuxKC preparada (web driver sem GeForceWeb + MaxwellMetal + MaxwellAccel).
# Boot-args (config.plist do OpenCore, voce edita): acrescentar  nvaccel=1 nvaccelarm=1
#   sem nvaccel=1 o acelerador nao sobe; sem nvaccelarm=1 ele sobe mas o WindowServer so ganha Metal com
#   sudo sysctl -w debug.nvaccelfb=1 + logout.
# Volta: sudo ./9_voltar.sh (funciona por ssh com a tela preta), ou o clone.
set -u
D=$(cd "$(dirname "$0")" && pwd)
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
NEW=$KC.troca; PRE=$KC.pre-troca; BK=$D/backup
LA=/Library/LaunchAgents; LD=/Library/LaunchDaemons; OFF=$BK/launchd-oclp
CD=/Library/Preferences/com.apple.CoreDisplay.plist
GB=/Library/GPUBundles
die() { echo "PARADA: $*"; exit 1; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
[ -f "$NEW" ] && [ -f "$PRE" ] && [ -f "$BK/kexts-nvidia-pre-troca.tgz" ] || die "rode antes o 1_preparar.sh"
kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | grep -q 'com.nvidia.web.GeForceWeb' || die "a AuxKC atual nao tem o GeForceWeb: troca ja feita?"
[ -d /Library/Extensions/MaxwellAccel.kext ] || die "MaxwellAccel ausente em /Library/Extensions"
echo "Isto tira o Non-Metal. Sem o acelerador funcionando, a tela fica preta ate a volta (9_voltar.sh por ssh, ou o clone)."
read -r -p "Tem o clone pronto e quer trocar? Digite TROCAR: " ok
[ "$ok" = "TROCAR" ] || die "cancelado"

echo "== 1. auto-patch do OCLP desligado (guardado em $OFF)"
mkdir -p "$OFF"
for f in "$LA"/com.dortania.opencore-legacy-patcher.auto-patch.plist "$LD"/com.dortania.opencore-legacy-patcher.*.plist; do
  [ -f "$f" ] || continue
  launchctl bootout system "$f" 2>/dev/null
  mv -f "$f" "$OFF/" && echo "   $(basename "$f")"
done

echo "== 2. CoreDisplay: tira useMetal/useIOP = NO"
[ -f "$CD" ] && cp -p "$CD" "$BK/com.apple.CoreDisplay.plist.pre-troca"
defaults delete /Library/Preferences/com.apple.CoreDisplay useMetal 2>/dev/null
defaults delete /Library/Preferences/com.apple.CoreDisplay useIOP 2>/dev/null
defaults read /Library/Preferences/com.apple.CoreDisplay 2>/dev/null | sed 's/^/   /'

echo "== 3. lista do plugin"
[ -f "$GB/nvmtl-allow.txt" ] && cp -p "$GB/nvmtl-allow.txt" "$BK/nvmtl-allow.txt.pre-troca"
cp "$D/nvmtl-allow-troca.txt" "$GB/nvmtl-allow.txt.new" && chmod 644 "$GB/nvmtl-allow.txt.new" && mv -f "$GB/nvmtl-allow.txt.new" "$GB/nvmtl-allow.txt" \
  || die "lista do plugin"

echo "== 4. sistema de volta ao snapshot selado original"
bless --mount / --bootefi --last-sealed-snapshot || die "bless --last-sealed-snapshot falhou (AuxKC ainda NAO foi trocada; desfaca 1-3 com o 9_voltar.sh)"

echo "== 5. AuxKC nova"
cp "$NEW" "$KC.new" && chown root:wheel "$KC.new" && chmod 744 "$KC.new" && mv -f "$KC.new" "$KC" || die "publicar a AuxKC (o bless ja foi: rode o 9_voltar.sh)"
kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | grep -q local.maxwell.MaxwellAccel || die "AuxKC publicada sem o MaxwellAccel?!"

echo
echo "PRONTO. Antes de reiniciar, no config.plist do OpenCore acrescente aos boot-args:  nvaccel=1 nvaccelarm=1"
echo "Depois do boot (ou por ssh se a tela ficar preta):  ~/maxwell_metal/kext/troca/3_conferir.sh"
echo "Volta:  sudo ~/maxwell_metal/kext/troca/9_voltar.sh   (ou o clone)"
