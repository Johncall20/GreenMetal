#!/bin/bash
# Instala o MaxwellMetal.kext na AuxKC e o NVMTLDriver.bundle em /Library/GPUBundles. Rodar com sudo.
# A colecao nova tem exatamente os kexts da atual + local.maxwell.MaxwellMetal (kmutil -x, caminhos explicitos).
# Backup: /Library/KernelCollections/AuxiliaryKernelExtensions.kc.maxwell-backup
# Desfazer: sudo ./uninstall_maxwellmetal.sh   (ou pelo Recovery: RECUPERACAO.txt)
set -u
D=$(cd "$(dirname "$0")" && pwd)
EXT=/Library/Extensions; GB=/Library/GPUBundles
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
BKP=$KC.maxwell-backup
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
KARG=(--allow-missing-kdk); [ -f /System/Library/Kernels/kernel ] && KARG+=(--kernel /System/Library/Kernels/kernel)
T=$(mktemp -d /tmp/maxwellmetal.XXXXXX)
die() { echo "PARADA: $*"; rm -rf "$T"; exit 1; }
undo_kext() { rm -rf "$EXT/MaxwellMetal.kext"; }
ids() { kmutil inspect -a x86_64 -A "$1" 2>/dev/null | awk -F'\t' 'NF>=3 && $1 ~ /\./ {print $1}' | sort -u; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
csrutil status 2>/dev/null | grep -q 'Kext Signing: disabled' || die "Kext Signing do SIP nao esta desligado"
[ -f "$D/MaxwellMetal.kext/Contents/MacOS/MaxwellMetal" ] && [ -x "$D/../gpubundles/install_gpubundles.sh" ] || die "arquivos ausentes em $D (kext ou ../gpubundles)"
[ -f "$KC" ] || die "AuxKC atual nao encontrada em $KC"

echo "== 1. kexts na AuxKC atual"
ids "$KC" | grep -v '^local.maxwell.MaxwellMetal$' | { [ "${SKIP_FB:-0}" = 1 ] && grep -v '^local.maxwell.MaxwellFB$' || cat; } > "$T/old.txt"
P=()
while IFS=$'\t' read -r id ver path _; do
  [[ "$id" == *.* && -n "${path:-}" && "$id" != *" "* ]] || continue
  [ "$id" = local.maxwell.MaxwellMetal ] && continue
  [ "${SKIP_FB:-0}" = 1 ] && [ "$id" = local.maxwell.MaxwellFB ] && { echo "   (fora) $id"; continue; }
  [ -d "$path" ] || die "kext da colecao atual sumiu do disco: $id ($path)"
  P+=(-p "$path"); echo "   $id  $path"
done < <(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null)
grep -q '^com.nvidia.web.NVDAResmanWeb$' "$T/old.txt" || die "NVDAResmanWeb nao esta na AuxKC atual -- nada mexido"
for x in MaxwellAGDC MaxwellFB; do
  [ "${SKIP_FB:-0}" = 1 ] && [ $x = MaxwellFB ] && continue
  k=/Library/Extensions/$x.kext
  [ -d "$k" ] || continue
  printf '%s\n' "${P[@]}" | grep -qx "$k" || { P+=(-p "$k"); echo "   (extra) $k"; }
done

echo "== 2. backup da AuxKC"
[ -f "$BKP" ] || cp -p "$KC" "$BKP" || die "backup"
cp -p "$KC" "$KC.maxwell-prev" || die "backup da colecao atual"
ls -la "$BKP" "$KC.maxwell-prev"

echo "== 3. kext em $EXT (root:wheel)"
undo_kext; ditto "$D/MaxwellMetal.kext" "$EXT/MaxwellMetal.kext" && chown -R root:wheel "$EXT/MaxwellMetal.kext" && chmod -R 755 "$EXT/MaxwellMetal.kext" || die "copia do kext"
kmutil print-diagnostics -p "$EXT/MaxwellMetal.kext" 2>&1 | grep -v '^No variant' | sed 's/^/   /'

echo "== 4. nova AuxKC (temporaria)"
kmutil create -n aux --volume-root / "${KARG[@]}" -B "$KB" -S "$KS" "${P[@]}" -p "$EXT/MaxwellMetal.kext" -x -A "$T/aux.kc" -z > "$T/kmutil.log" 2>&1
rc=$?; grep -v '^No variant' "$T/kmutil.log" | tail -20 | sed 's/^/   /'
[ $rc -eq 0 ] && [ -s "$T/aux.kc" ] || { undo_kext; die "kmutil create falhou rc=$rc (kext removido, AuxKC intacta)"; }
ids "$T/aux.kc" > "$T/new.txt"
missing=$(comm -23 "$T/old.txt" "$T/new.txt")
[ -z "$missing" ] || { echo "$missing" | sed 's/^/   FALTARIA: /'; undo_kext; die "a colecao nova perderia kexts (kext removido, AuxKC intacta)"; }
grep -q '^local.maxwell.MaxwellMetal$' "$T/new.txt" || { undo_kext; die "kmutil nao incluiu o MaxwellMetal (kext removido, AuxKC intacta)"; }
echo "   ok: $(grep -c . "$T/old.txt") kexts antigos + local.maxwell.MaxwellMetal"

echo "== 5. pilha Metal em $GB (../gpubundles)"
"$D/../gpubundles/install_gpubundles.sh" | sed 's/^/   /'; [ "${PIPESTATUS[0]}" -eq 0 ] || die "instalacao da pilha"

echo "== 6. publica a nova AuxKC"
cp "$T/aux.kc" "$KC.new" && chown root:wheel "$KC.new" && chmod 744 "$KC.new" && mv -f "$KC.new" "$KC" || die "nao consegui publicar (backup em $BKP)"
ids "$KC" | grep -q '^local.maxwell.MaxwellMetal$' || die "colecao publicada sem o kext?! restaure com o uninstall"
rm -rf "$T"
echo
echo "PRONTO. Reinicie. Depois do boot, no hackintosh:  ~/maxwell_metal/kext/deploy/pos_boot.sh"
echo "Problema no boot: boot-arg -mmeoff, ou sudo ./uninstall_maxwellmetal.sh, ou Recovery (RECUPERACAO.txt)."
