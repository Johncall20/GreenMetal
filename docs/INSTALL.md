# Installing GreenMetal

🇧🇷 [Em português](INSTALL.pt-BR.md)

> [!CAUTION]
> There is no installer. This page describes what the author does on one machine (GTX 960, macOS 26.7.2), using lab
> scripts written for that machine. Every step that needs `sudo` replaces part of what your Mac boots from. Read each
> script before you run it. If you are not ready to recover from a black screen over `ssh`, stop here.

## Before anything

1. **Run the preflight.** It is read-only and needs no root:

   ```bash
   tools/greenmetal-check.sh
   ```

   Fix everything marked `[!!]` first.
2. **Have three ways back:** a tested USB stick with your working EFI, a bootable clone of the system volume, and
   Remote Login (`ssh`) enabled with a second computer to use it from.
3. **Know your starting point.** The scripts assume the state the author started from: macOS 26 with OpenCore Legacy
   Patcher's non-Metal patches and the NVIDIA Web Driver 387.10.10.10.40.140 installed and driving the screen
   (`NVDAStartupWeb`, `NVDAResmanWeb`, `NVDAGM100HalWeb` and `GeForceWeb` in the auxiliary kernel collection), with
   `ngfxgl=1` in boot-args. Getting to that state is not covered here.

## What gets installed

| Piece | Where it goes |
|---|---|
| `MaxwellMetal.kext`, `MaxwellAccel.kext`, `MaxwellAGDC.kext`, `MaxwellFB.kext` | `/Library/Extensions`, then the auxiliary kernel collection |
| `NVMTLDriver.bundle` (the Metal plugin, with the shader runtime inside) | `/Library/GPUBundles` |
| `libvulkan_nouveau.dylib` (NVK) and `libnvmtl_translate.dylib` (shader translator) | `/Library/GPUBundles/nvmtl` |
| `nvmtl-allow.txt` (which processes get the Metal device) | `/Library/GPUBundles` |

`/Library/GPUBundles` is used because sandboxed apps are allowed to read it.

## 1. Build

On the build machine:

- **Kexts:** run `build.sh` in `kext/MaxwellMetal`, `accel`, `agdc` and `fb`. Each leaves its kext under its own
  `build/` directory.
- **NVK:** follow [`nvk/README.md`](../nvk/README.md).
- **Metal plugin:** `metal/build_plugin.sh` (needs the Mesa tree from the NVK step for the Vulkan headers).
- **Shader translator and its runtime:** these are NullMoth's, unmodified. Build `libnvmtl_translate.dylib` and the
  `air-runtime` (an LLVM build) from [NullMoth's repository](https://github.com/nullmoth/nvidia-macos-driver) at
  v2.7.2, following its build scripts.

Everything must be x86_64, and every dylib and bundle needs at least an ad-hoc signature (`codesign -s - -f`).
Sandboxed processes refuse unsigned code.

## 2. Stage

The scripts look for the binaries next to themselves. Lay them out like this:

```
kext/deploy/MaxwellMetal.kext
kext/troca/MaxwellAccel.kext
kext/troca/MaxwellAGDC.kext
kext/troca/MaxwellFB.kext                                      (optional: the framebuffer)
kext/gpubundles/NVMTLDriver.bundle/Contents/Info.plist
kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib
kext/gpubundles/nvmtl/libvulkan_nouveau.dylib
kext/gpubundles/nvmtl/libnvmtl_translate.dylib
```

Then copy the tree to **`~/maxwell_metal`** on the target machine. Several scripts print or use that path.

## 3. Stage one: Metal for apps

This keeps your current desktop exactly as it is and adds a Metal device that apps can use.

```bash
sudo mkdir -p /Library/GPUBundles
```

```bash
sudo ~/maxwell_metal/kext/deploy/install_maxwellmetal.sh
```

What the script does:

- refuses to run unless kext signing is disabled in SIP and `NVDAResmanWeb` is in the current collection;
- builds a **new** auxiliary kernel collection in a temporary file, containing every kext of the current one plus
  `MaxwellMetal`, and stops if any existing kext would be lost;
- keeps two copies of the old collection next to the live one: `.maxwell-backup` (from before the first install) and
  `.maxwell-prev` (from before this run);
- installs the user-space stack in `/Library/GPUBundles`;
- only then publishes the new collection.

Install the allow list (`sudo ~/maxwell_metal/kext/deploy/allowlist_install.sh`), reboot, and check:

```bash
~/maxwell_metal/kext/deploy/pos_boot.sh
```

It confirms the kext loaded, arms the Metal plugin (`sudo sysctl -w debug.maxwell_metal=1`, which lasts until reboot)
and runs a small Metal probe as your normal user.

**If the machine does not come up:** boot with `-mmeoff` in boot-args (the kext loads and does nothing), or run
`sudo ~/maxwell_metal/kext/deploy/uninstall_maxwellmetal.sh` over `ssh`, or restore the collection from Recovery as
described in `kext/deploy/RECUPERACAO.txt`.

## 4. Stage two: the switch

This is the step that can leave you without a picture. It removes the non-Metal patches and NVIDIA's accelerator, and
boots with `MaxwellAccel` in their place. Do it only after stage one works.

```bash
sudo ~/maxwell_metal/kext/troca/1_preparar.sh
```

Safe: it changes nothing the next boot uses. It mounts Apple's original sealed snapshot read-only, builds the new
collection against it (Web Driver without `GeForceWeb`, plus `MaxwellMetal` and `MaxwellAccel`) into a side file, and
saves the current collection and the NVIDIA kexts for the way back.

```bash
sudo ~/maxwell_metal/kext/troca/2_trocar.sh
```

Not safe: it asks you to type `TROCAR`, then disables OpenCore Legacy Patcher's auto-patch, clears the CoreDisplay
overrides, installs the allow list for this setup, blesses the last sealed snapshot (which removes every system patch)
and publishes the new collection.

Before you reboot, add to boot-args in your OpenCore `config.plist`:

```
nvaccel=1 nvaccelarm=1
```

`nvaccel=1` lets the accelerator start. `nvaccelarm=1` arms it at boot. Without the second one you arm it by hand with
`sudo sysctl -w debug.nvaccelfb=1`.

After the reboot, also over `ssh` if the screen is black:

```bash
~/maxwell_metal/kext/troca/3_conferir.sh
```

**The way back** works over `ssh` with a black screen, and reapplies the patcher's system patches:

```bash
sudo ~/maxwell_metal/kext/troca/9_voltar.sh
```

## 5. After each boot: the Metal desktop

The machine comes up on a CPU-composited desktop. To move WindowServer to Metal with zero-copy flips:

```bash
sudo SURFSHARE=3 FLIP=1 PERFIL=1 ~/maxwell_metal/kext/troca/10_metal_total.sh
```

It restarts WindowServer, so you land on the login screen. It measures for about two and a half minutes, goes back by
itself if the flip counters look wrong, and leaves a guard that returns WindowServer to the CPU path if it dies.

To go back by hand:

```bash
sudo ~/maxwell_metal/kext/troca/10_metal_total.sh volta
```

## 6. Updating

- **Kexts changed:** restage them and run `sudo ~/maxwell_metal/kext/troca/atualizar_accel.sh`, then reboot. Check the
  date of `/Library/KernelCollections/AuxiliaryKernelExtensions.kc` before you reboot: if it did not change, the
  collection was not rebuilt.
- **Only the plugin or NVK changed:** `sudo ~/maxwell_metal/kext/gpubundles/install_gpubundles.sh`. No reboot. Apps
  opened afterwards use the new stack.

## What to report

Whatever happens, a [card report](../../../issues/new/choose) with the output of `tools/greenmetal-check.sh` helps.
