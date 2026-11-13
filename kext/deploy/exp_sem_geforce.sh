#!/bin/bash
# EXPERIMENTO da fase 6: um boot sem o GeForceWeb (o acelerador da NVIDIA), com o resto igual.
# A AuxKC nova tem exatamente os kexts da atual MENOS com.nvidia.web.GeForceWeb. O RM (NVDAResmanWeb), o
# framebuffer NVDA, o HAL Maxwell e o MaxwellMetal continuam. O display fica so com o NDRV do RM.
# Pergunta que o boot responde: sem o canal de display do GeForceWeb, o 0x40d {registrar + aplicar} troca a tela?
#   depois do boot:  ~/maxwell_metal/tools/ndrv_present probe   e   ~/maxwell_metal/tools/ndrv_present show 0
# Nesse boot nao ha OpenGL acelerado nem Metal do sistema (os dois dependem do GeForceWeb): a tela fica lenta.
# Desfazer: sudo ./exp_restaura_geforce.sh e reiniciar (ou pelo Recovery: RECUPERACAO.txt).
set -u
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
SAVE=$KC.pre-semgeforce
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
KARG=(--allow-missing-kdk); [ -f /System/Library/Kernels/kernel ] && KARG+=(--kernel /System/Library/Kernels/kernel)
T=$(mktemp -d /tmp/semgeforce.XXXXXX)
die() { echo "PARADA: $*"; rm -rf "$T"; exit 1; }
ids() { kmutil inspect -a x86_64 -A "$1" 2>/dev/null | awk -F'\t' 'NF>=3 && $1 ~ /\./ {print $1}' | sort -u; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
[ -f "$KC" ] || die "AuxKC atual nao encontrada"

echo "== 1. kexts na AuxKC atual"
ids "$KC" > "$T/old.txt"
grep -q '^com.nvidia.web.GeForceWeb$' "$T/old.txt" || die "o GeForceWeb ja nao esta na AuxKC atual -- nada mexido"
grep -q '^com.nvidia.web.NVDAResmanWeb$' "$T/old.txt" || die "NVDAResmanWeb nao esta na AuxKC atual -- nada mexido"
P=()
while IFS=$'\t' read -r id ver path _; do
  [[ "$id" == *.* && -n "${path:-}" && "$id" != *" "* ]] || continue
  [ "$id" = com.nvidia.web.GeForceWeb ] && { echo "   (fora) $id"; continue; }
  [ -d "$path" ] || die "kext da colecao atual sumiu do disco: $id ($path)"
  P+=(-p "$path"); echo "   $id  $path"
done < <(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null)

echo "== 2. guarda a AuxKC atual (com o GeForceWeb) em $SAVE"
cp -p "$KC" "$SAVE" || die "copia de seguranca"
ls -la "$SAVE"

echo "== 3. nova AuxKC sem o GeForceWeb (temporaria)"
kmutil create -n aux --volume-root / "${KARG[@]}" -B "$KB" -S "$KS" "${P[@]}" -x -A "$T/aux.kc" -z > "$T/kmutil.log" 2>&1
rc=$?; grep -v '^No variant' "$T/kmutil.log" | tail -20 | sed 's/^/   /'
[ $rc -eq 0 ] && [ -s "$T/aux.kc" ] || die "kmutil create falhou rc=$rc (AuxKC intacta)"
ids "$T/aux.kc" > "$T/new.txt"
grep -v '^com.nvidia.web.GeForceWeb$' "$T/old.txt" > "$T/want.txt"
missing=$(comm -23 "$T/want.txt" "$T/new.txt")
[ -z "$missing" ] || { echo "$missing" | sed 's/^/   FALTARIA: /'; die "a colecao nova perderia kexts (AuxKC intacta)"; }
grep -q '^com.nvidia.web.GeForceWeb$' "$T/new.txt" && die "o GeForceWeb continuou na colecao (AuxKC intacta)"
echo "   ok: $(grep -c . "$T/new.txt") kexts, sem o GeForceWeb"

echo "== 4. publica"
cp "$T/aux.kc" "$KC.new" && chown root:wheel "$KC.new" && chmod 744 "$KC.new" && mv -f "$KC.new" "$KC" || die "nao consegui publicar (atual guardada em $SAVE)"
ids "$KC" | grep -q '^com.nvidia.web.GeForceWeb$' && die "colecao publicada ainda tem o GeForceWeb?!"
rm -rf "$T"
echo
echo "PRONTO. Reinicie. A tela deve subir sem aceleracao (lenta). Depois, no hackintosh ou por ssh:"
echo "   ~/maxwell_metal/tools/ndrv_present probe"
echo "   ~/maxwell_metal/tools/ndrv_present show 0"
echo "Voltar ao normal: sudo ~/maxwell_metal/kext/deploy/exp_restaura_geforce.sh e reiniciar."
