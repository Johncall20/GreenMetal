#!/bin/bash
# Depois da troca: poe o MaxwellAccel desta pasta em /Library/Extensions e remonta a AuxKC com o instalador normal
# (que liga contra o sistema atual, agora o original). Rodar com sudo; reiniciar depois do PRONTO.
set -u
D=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
kmutil inspect -a x86_64 -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -q local.maxwell.MaxwellAccel \
  || { echo "PARADA: a AuxKC atual nao tem o MaxwellAccel (a troca foi feita?)"; exit 1; }
echo "== MaxwellAccel $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$D/MaxwellAccel.kext/Contents/Info.plist")"
rm -rf /Library/Extensions/MaxwellAccel.kext
ditto "$D/MaxwellAccel.kext" /Library/Extensions/MaxwellAccel.kext && chown -R root:wheel /Library/Extensions/MaxwellAccel.kext \
  && chmod -R 755 /Library/Extensions/MaxwellAccel.kext || { echo "PARADA: copia"; exit 1; }
for x in MaxwellAGDC MaxwellFB; do
  [ -d "$D/$x.kext" ] || continue
  echo "== $x $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$D/$x.kext/Contents/Info.plist")"
  rm -rf "/Library/Extensions/$x.kext"
  ditto "$D/$x.kext" "/Library/Extensions/$x.kext" && chown -R root:wheel "/Library/Extensions/$x.kext" \
    && chmod -R 755 "/Library/Extensions/$x.kext" || { echo "PARADA: copia do $x"; exit 1; }
done
if [ -d /Library/Extensions/MaxwellFB.kext ] && [ -f "$D/ws/mxfb_ws.sh" ]; then
  echo "== LaunchDaemon local.maxwell.mxfb-ws (reinicia o WindowServer uma vez quando o MaxwellFB aparece)"
  mkdir -p /usr/local/libexec/maxwell
  install -o root -g wheel -m 755 "$D/ws/mxfb_ws.sh" /usr/local/libexec/maxwell/mxfb_ws.sh || { echo "PARADA: script do daemon"; exit 1; }
  install -o root -g wheel -m 644 "$D/ws/local.maxwell.mxfb-ws.plist" /Library/LaunchDaemons/local.maxwell.mxfb-ws.plist || { echo "PARADA: plist do daemon"; exit 1; }
  echo "   ok (roda no proximo boot; log em /var/log/mxfb_ws.log)"
fi
# NVK (libvulkan_nouveau.dylib) novo em /Library/GPUBundles/nvmtl, se o desta pasta for diferente do instalado.
# Vale para processos abertos depois; o anterior fica em backup/nvmtl.antes.
G="$D/../gpubundles/nvmtl"; GB=/Library/GPUBundles
if [ -f "$G/libvulkan_nouveau.dylib" ] && [ -d "$GB/nvmtl" ] && ! cmp -s "$G/libvulkan_nouveau.dylib" "$GB/nvmtl/libvulkan_nouveau.dylib"; then
  echo "== NVK novo em $GB/nvmtl"
  for f in "$G"/*.dylib; do codesign -v "$f" 2>/dev/null || { echo "PARADA: assinatura invalida: $f"; exit 1; }; done
  rm -rf "$GB/.nvmtl.new" && ditto "$G" "$GB/.nvmtl.new" && chown -R root:wheel "$GB/.nvmtl.new" && chmod 755 "$GB/.nvmtl.new" \
    && chmod 755 "$GB/.nvmtl.new"/*.dylib || { echo "PARADA: copia do NVK"; exit 1; }
  mkdir -p "$D/backup"; rm -rf "$D/backup/nvmtl.antes"
  mv "$GB/nvmtl" "$D/backup/nvmtl.antes" && mv "$GB/.nvmtl.new" "$GB/nvmtl" || { echo "PARADA: publicar o NVK"; exit 1; }
  codesign -v "$GB/nvmtl/libvulkan_nouveau.dylib" && echo "   ok (anterior em $D/backup/nvmtl.antes)"
fi
exec "$D/../deploy/install_maxwellmetal.sh"
