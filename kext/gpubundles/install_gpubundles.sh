#!/bin/bash
# Pilha Metal em /Library/GPUBundles (onde apps com sandbox podem ler). Rodar com sudo.
#   /Library/GPUBundles/NVMTLDriver.bundle   plugin (caminhos /Library/GPUBundles/nvmtl/...)
#   /Library/GPUBundles/nvmtl/               tradutor AIR->SPIR-V + NVK
# Backup do bundle anterior (que usa ~/maxwell_metal/nvmtl): /Library/GPUBundles/NVMTLDriver.bundle.pre-lib
# Voltar: sudo ./rollback_gpubundles.sh
set -u
D=$(cd "$(dirname "$0")" && pwd); GB=/Library/GPUBundles
die() { echo "PARADA: $*"; exit 1; }
[ "$(id -u)" -eq 0 ] || die "precisa de root (sudo)"
for f in NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib \
         nvmtl/libnvmtl_translate.dylib nvmtl/libvulkan_nouveau.dylib; do
  [ -f "$D/$f" ] || die "falta $f"; codesign -v "$D/$f" 2>/dev/null || die "assinatura invalida: $f"; done
[ -d "$GB" ] || die "$GB nao existe"
echo "== 1. backup do bundle atual"
if [ -d "$GB/NVMTLDriver.bundle" ] && [ ! -d "$GB/NVMTLDriver.bundle.pre-lib" ]; then
  ditto "$GB/NVMTLDriver.bundle" "$GB/NVMTLDriver.bundle.pre-lib" || die "backup"; fi
ls -d "$GB"/NVMTLDriver.bundle*
echo "== 2. nvmtl/ (temporario + mv: inode novo, sem cache de assinatura velho)"
rm -rf "$GB/.nvmtl.new" && ditto "$D/nvmtl" "$GB/.nvmtl.new" && chown -R root:wheel "$GB/.nvmtl.new" && chmod 755 "$GB/.nvmtl.new" && chmod 755 "$GB/.nvmtl.new"/*.dylib || die "copia nvmtl"
rm -rf "$GB/.nvmtl.old"; [ -d "$GB/nvmtl" ] && mv "$GB/nvmtl" "$GB/.nvmtl.old"; mv "$GB/.nvmtl.new" "$GB/nvmtl" || die "publicar nvmtl"
rm -rf "$GB/.nvmtl.old"
echo "== 3. bundle do plugin"
rm -rf "$GB/.bundle.new" && ditto "$D/NVMTLDriver.bundle" "$GB/.bundle.new" && chown -R root:wheel "$GB/.bundle.new" && chmod -R go-w,a+rX "$GB/.bundle.new" || die "copia bundle"
rm -rf "$GB/.bundle.old"; mv "$GB/NVMTLDriver.bundle" "$GB/.bundle.old" 2>/dev/null; mv "$GB/.bundle.new" "$GB/NVMTLDriver.bundle" || die "publicar bundle"
rm -rf "$GB/.bundle.old"
echo "== 4. conferencia"
ls -la "$GB" "$GB/nvmtl"
strings -a "$GB/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver" | grep -c maxwell_metal | sed 's/^/   referencias a ~\/maxwell_metal no plugin (tem que ser 0): /'
codesign -v "$GB/NVMTLDriver.bundle" && codesign -v "$GB/nvmtl/libvulkan_nouveau.dylib" && echo "   assinaturas ok"
echo "PRONTO. Apps abertos depois disto usam a pilha nova (os ja abertos seguem com a velha)."
