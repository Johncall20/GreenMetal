# NVK for the r387 resource manager

NVK here is Mesa `17ca6174` with two patches on top:

1. NullMoth's `nvk/nvk-macos.patch` (v2.7.2), which ports NVK to macOS and adds the `nvkmd/nvrm` backend;
2. [`patches/0002-nvk-maxwell-r387.patch`](patches/0002-nvk-maxwell-r387.patch), ours: Maxwell as a host class,
   submission by writing `GPPut` to the channel's USERD instead of the doorbell, and `libnvrm387` as the transport.

`libnvrm387` is compiled into the driver from [`../libnvrm387`](../libnvrm387) through symbolic links.

## Assemble the tree

```bash
nvk/preparar_mesa.sh
```

This downloads the Mesa tarball, NullMoth v2.7.2 and NVIDIA's `open-gpu-kernel-modules` 615.78.08 if they are not
already under `nvk/dl` and `ref/`, applies both patches and creates the links. It builds nothing. The result has been
checked to be identical to the tree the author builds from.

## Build

The author builds on an Apple Silicon Mac and cross-compiles to x86_64, in two stages. The commands below were
reconstructed from the Meson build directories, not from a script, so expect to adjust paths.

Dependencies (Homebrew): `meson ninja llvm libclc spirv-llvm-translator spirv-tools glslang pkgconf`, Rust through
`rustup` with the `x86_64-apple-darwin` target, `bindgen`, and a Python environment with `mako`, `pyyaml` and
`packaging`.

Stage 1, native: the two build-time tools the cross build needs.

```bash
meson setup nvk/build-host nvk/mesa -Dbuildtype=release \
  -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled \
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
  -Dbuild-tests=false -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled
ninja -C nvk/build-host
mkdir -p nvk/hosttools
ln -s "$PWD/nvk/build-host/src/compiler/clc/mesa_clc" nvk/hosttools/
ln -s "$PWD/nvk/build-host/src/compiler/spirv/vtn_bindgen2" nvk/hosttools/
```

Stage 2, cross: the driver itself, with `nvk/hosttools` on the `PATH`.

```bash
PATH="$PWD/nvk/hosttools:$PATH" meson setup nvk/build-x86 nvk/mesa \
  --cross-file nvk/cross-x86_64-darwin.ini -Dbuildtype=debugoptimized \
  -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms= -Degl=disabled -Dglx=disabled -Dglvnd=disabled \
  -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=system -Dvulkan-layers= -Dtools= -Dbuild-tests=false \
  -Dexpat=disabled -Dzstd=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled \
  -Dgallium-rusticl=false -Dspirv-tools=disabled
ninja -C nvk/build-x86
codesign -s - -f nvk/build-x86/src/nouveau/vulkan/libvulkan_nouveau.dylib
```

The result is `nvk/build-x86/src/nouveau/vulkan/libvulkan_nouveau.dylib`. Every dylib that goes to the test machine
needs at least an ad-hoc signature, or sandboxed processes refuse to load it.

`debugoptimized` is what is in use today. It keeps NIR validation compiled in, which the Metal plugin turns off at run
time with `NIR_DEBUG=novalidate`. A release build is on the to-do list.

## Tests

`tests/` has three small Vulkan programs: `vk_enum` (enumerate the device), `vk_compute` (a compute shader that fills a
buffer) and `vk_triangle` (the triangle in the top-level README). `../tools/run_maxwell_regression.sh` runs them on the
test machine together with the `libnvrm387` hardware test.
