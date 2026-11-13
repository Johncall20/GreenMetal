#!/bin/bash
# TROCA, passo 1 (nao mexe em nada que o boot usa): monta, num arquivo a parte, a AuxKC do sistema SEM Non-Metal.
#   - le o snapshot selado original da Apple (o sistema sem os patches do OCLP), so leitura;
#   - liga a AuxKC nova contra a Boot/System KC DESSE snapshot: kexts do web driver (sem GeForceWeb/Tesla),
#     MaxwellMetal e MaxwellAccel (o acelerador portado, que precisa da IOAcceleratorFamily2 original);
#   - guarda a AuxKC atual (Non-Metal) e um tar dos kexts da NVIDIA, para a volta.
# Resultado: /Library/KernelCollections/AuxiliaryKernelExtensions.kc.troca. Nada e publicado. Rodar com sudo.
set -u
D=$(cd "$(dirname "$0")" && pwd)
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
NEW=$KC.troca; PRE=$KC.pre-troca
BK=$D/backup; MNT=/private/var/tmp/troca-snap
T=$(mktemp -d /tmp/troca.XXXXXX)
die() { echo "PARADA: $*"; mount | grep -q " $MNT " && umount "$MNT"; rm -rf "$T"; exit 1; }
ids() { kmutil inspect -a x86_64 -A "$1" 2>/dev/null | awk -F'\t' 'NF>=3 && $1 ~ /\./ {print $1}' | sort -u; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
[ -f "$D/MaxwellAccel.kext/Contents/MacOS/MaxwellAccel" ] || die "MaxwellAccel.kext ausente em $D"
[ -f /Library/Extensions/MaxwellMetal.kext/Contents/Info.plist ] || die "MaxwellMetal nao instalado"
v=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /Library/Extensions/MaxwellMetal.kext/Contents/Info.plist)
case "$v" in 0.[0-4].*) die "MaxwellMetal $v: a troca precisa do 0.5.0 ou mais novo (backend + ABI)";; esac
ids "$KC" > "$T/old.txt"
grep -q '^com.nvidia.web.GeForceWeb$' "$T/old.txt" || die "a AuxKC atual nao tem o GeForceWeb: a troca ja foi feita? (nada mexido)"

echo "== 1. snapshot selado original"
SNAP=$(diskutil apfs listSnapshots / 2>/dev/null | awk -F': *' '/Name:/ && /com.apple.os.update-/ {print $2; exit}')
[ -n "$SNAP" ] || die "nao achei o snapshot com.apple.os.update-* (o sistema original)"
DEV=$(diskutil info / | awk -F': *' '/Device Node/ {print $2}')
case "$DEV" in /dev/disk*s*s*) DEV=${DEV%s*};; esac
echo "   $SNAP em $DEV"
mkdir -p "$MNT"
mount | grep -q " $MNT " || mount_apfs -o rdonly -s "$SNAP" "$DEV" "$MNT" || die "mount_apfs do snapshot"
SB=$MNT/System/Library/KernelCollections/BootKernelExtensions.kc
SS=$MNT/System/Library/KernelCollections/SystemKernelExtensions.kc
SK=$MNT/System/Library/Kernels/kernel
[ -f "$SB" ] && [ -f "$SS" ] || die "o snapshot nao tem Boot/System KC"
fam=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$MNT/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/Info.plist" 2>/dev/null)
echo "   IOAcceleratorFamily2 do snapshot: ${fam:-AUSENTE} (a atual e 378.26, do Non-Metal)"
case "$fam" in 378.*|"") die "o snapshot nao tem a IOAcceleratorFamily2 original";; esac

echo "== 2. MaxwellAccel em /Library/Extensions (root:wheel; fora de qualquer AuxKC ate a troca)"
rm -rf /Library/Extensions/MaxwellAccel.kext
ditto "$D/MaxwellAccel.kext" /Library/Extensions/MaxwellAccel.kext && chown -R root:wheel /Library/Extensions/MaxwellAccel.kext \
  && chmod -R 755 /Library/Extensions/MaxwellAccel.kext || die "copia do MaxwellAccel"

echo "== 3. kexts da AuxKC nova"
P=()
while IFS=$'\t' read -r id ver path _; do
  [[ "$id" == *.* && -n "${path:-}" && "$id" != *" "* ]] || continue
  case "$id" in com.nvidia.web.GeForceWeb|com.nvidia.web.GeForceTeslaWeb|com.nvidia.web.NVDAResmanTeslaWeb|com.nvidia.web.NVDANV50HalTeslaWeb|local.maxwell.MaxwellAccel)
    echo "   (fora) $id"; continue;; esac
  [ -d "$path" ] || die "kext da colecao atual sumiu do disco: $id ($path)"
  P+=(-p "$path"); echo "   $id"
done < <(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null)
P+=(-p /Library/Extensions/MaxwellAccel.kext); echo "   local.maxwell.MaxwellAccel"

echo "== 4. kmutil create contra o sistema original (temporario)"
KARG=(--allow-missing-kdk); [ -f "$SK" ] && KARG+=(--kernel "$SK")
kmutil create -n aux --volume-root / "${KARG[@]}" -B "$SB" -S "$SS" "${P[@]}" -x -A "$T/aux.kc" -z > "$T/kmutil.log" 2>&1
rc=$?; grep -v '^No variant' "$T/kmutil.log" | tail -25 | sed 's/^/   /'
[ $rc -eq 0 ] && [ -s "$T/aux.kc" ] || die "kmutil create falhou rc=$rc"
ids "$T/aux.kc" > "$T/new.txt"
for want in com.nvidia.web.NVDAResmanWeb com.nvidia.NVDAStartupWeb com.nvidia.web.NVDAGM100HalWeb local.maxwell.MaxwellMetal local.maxwell.MaxwellAccel; do
  grep -qx "$want" "$T/new.txt" || die "a colecao nova nao tem $want"
done
grep -qx com.nvidia.web.GeForceWeb "$T/new.txt" && die "o GeForceWeb entrou na colecao nova"
echo "   ok: $(grep -c . "$T/new.txt") kexts:"; sed 's/^/     /' "$T/new.txt"
umount "$MNT" || echo "   (aviso: umount do snapshot falhou; some no proximo boot)"

echo "== 5. guarda para a volta"
mkdir -p "$BK"
cp -p "$KC" "$PRE" || die "copia da AuxKC atual"
tar -C /Library/Extensions -czf "$BK/kexts-nvidia-pre-troca.tgz" $(cd /Library/Extensions && ls -d NVDA*.kext GeForce*.kext MaxwellMetal.kext) \
  || die "tar dos kexts"
cp -p "$T/aux.kc" "$NEW" && chown root:wheel "$NEW" && chmod 744 "$NEW" || die "gravar $NEW"
ls -la "$KC" "$PRE" "$NEW" "$BK/kexts-nvidia-pre-troca.tgz"
rm -rf "$T"
echo
echo "PRONTO (nada publicado; o proximo boot continua igual). Passo seguinte: sudo $D/2_trocar.sh"
