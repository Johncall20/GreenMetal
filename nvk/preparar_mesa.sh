#!/bin/bash
# Monta a arvore do Mesa usada no NVK deste projeto:
#   Mesa 17ca6174  +  nvk-macos.patch do NullMoth (v2.7.2)  +  patches/0002-nvk-maxwell-r387.patch (nosso)
#   + links para a libnvrm387 e para os headers do open-gpu-kernel-modules 615.78.08.
# Nao compila nada. Baixa o que faltar (git e curl); para usar copias locais, aponte as variaveis abaixo.
#
# Uso: nvk/preparar_mesa.sh [destino]        destino padrao: nvk/mesa
#   MESA_TARBALL=arquivo.tar.gz   tarball do Mesa 17ca6174 ja baixado
#   NULLMOTH_DIR=pasta            checkout do nullmoth/nvidia-macos-driver em v2.7.2
#   OGKM_DIR=pasta                checkout do NVIDIA/open-gpu-kernel-modules em 615.78.08
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$D/.." && pwd)
DEST=${1:-$D/mesa}
MESA_REV=17ca6174
NULLMOTH_TAG=v2.7.2
OGKM_TAG=615.78.08
MESA_TARBALL=${MESA_TARBALL:-$D/dl/mesa-$MESA_REV.tar.gz}
NULLMOTH_DIR=${NULLMOTH_DIR:-$R/ref/nullmoth}
OGKM_DIR=${OGKM_DIR:-$R/ref/ogkm}

[ -e "$DEST" ] && { echo "PARADA: $DEST ja existe"; exit 1; }

if [ ! -f "$MESA_TARBALL" ]; then
  mkdir -p "$(dirname "$MESA_TARBALL")"
  curl -fL -o "$MESA_TARBALL" "https://gitlab.freedesktop.org/mesa/mesa/-/archive/$MESA_REV/mesa-$MESA_REV.tar.gz"
fi
[ -d "$NULLMOTH_DIR" ] || git clone --depth 1 --branch "$NULLMOTH_TAG" https://github.com/nullmoth/nvidia-macos-driver.git "$NULLMOTH_DIR"
[ -d "$OGKM_DIR" ] || git clone --depth 1 --branch "$OGKM_TAG" https://github.com/NVIDIA/open-gpu-kernel-modules.git "$OGKM_DIR"
[ -f "$NULLMOTH_DIR/nvk/nvk-macos.patch" ] || { echo "PARADA: $NULLMOTH_DIR/nvk/nvk-macos.patch ausente"; exit 1; }

T=$(mktemp -d "${TMPDIR:-/tmp}/mesa.XXXXXX")
tar xzf "$MESA_TARBALL" -C "$T"
mkdir -p "$(dirname "$DEST")"
mv "$T"/mesa-* "$DEST"; rmdir "$T"

cd "$DEST"
patch -p1 -s < "$NULLMOTH_DIR/nvk/nvk-macos.patch"
patch -p1 -s < "$D/patches/0002-nvk-maxwell-r387.patch"

N=src/nouveau/vulkan/nvkmd/nvrm
ln -s "$R/libnvrm387/src/nvrm387.c"     "$N/nvrm387.c"
ln -s "$R/libnvrm387/include/nvrm387.h" "$N/nvrm387.h"
ln -s "$R/nvrm_r387.h"                  "$N/nvrm_r387.h"
ln -s "$OGKM_DIR"                       "$N/open-gpu-kernel-modules"
echo "ok: $DEST"
