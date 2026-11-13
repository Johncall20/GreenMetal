# Credits and third-party notices

GreenMetal exists because of the projects below. If you like what it does, they are the ones to thank.

## Work this repository is built on

### NullMoth NVIDIA driver for macOS

<https://github.com/nullmoth/nvidia-macos-driver> · Copyright (c) 2026 NullMoth Systems ·
[PolyForm Noncommercial License 1.0.0](LICENSES/PolyForm-Noncommercial-1.0.0.md)

The design of this project (a Metal plugin over NVK, an IOAccelerator kext underneath) is NullMoth's. Base version:
**v2.7.2**, commit `18ba05f38b5eadea8b5ae05834bb51c61fb4c5c3`.

| Here | From | What changed |
|---|---|---|
| `metal/plugin-lib/` | `plugin/` | `NVMTLDevice.m`, `nvmtl_vk.c` and `nvmtl_vk.h`: wide-row display surfaces, reading the accelerator's scanout geometry, NIR validation off by default. Every other file is unmodified. |
| `accel/src/nvrm-accel.cpp`, `accel/src/nvaccel_stubs.h` | `kexts/NVRM/accel/` | Ported from NullMoth's kernel resource manager to a backend over NVIDIA's r387 `NvRm*` exports; surface sharing modes, wide-row scanout, page flip, page-off. |
| `accel/src/iofam/`, `accel/src/re/`, `accel/src/tahoe_fwd.h` | `kexts/NVRM/accel/` | Unmodified. |
| `accel/nvrm_*.h` | `kexts/NVRM/`, `kexts/NVRMFB/` | ABI headers; `nvrm_vram_abi.h` and `nvrm_surfshare_abi.h` changed, the rest unmodified. `accel/src/nvkms-kapi.h` is a stub written for this port. |
| `agdc/` | `kexts/NVRMAGDC/` | `nvrm-agdc.cpp` adapted; headers unmodified. |

Used but **not included** here, so take them from NullMoth's repository: the NVK macOS patch (`nvk/nvk-macos.patch`)
and the AIR→SPIR-V shader translator.

Files derived from NullMoth keep their original copyright headers and stay under NullMoth's licence.

### Mesa, NVK and nouveau

<https://mesa3d.org> · MIT

NVK is the Vulkan driver that drives the card, and NAK is the compiler that turns shaders into Maxwell machine code.
Both are the work of the Mesa community, built on years of reverse engineering by the nouveau project. Base revision:
Mesa `17ca6174`. Our changes ship as [`nvk/patches/0002-nvk-maxwell-r387.patch`](nvk/patches/0002-nvk-maxwell-r387.patch),
applied on top of NullMoth's patch.

That patch is **not** an upstream contribution and must not be submitted to Mesa as it is. See "How this was written"
below.

### NVIDIA

- [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) 615.78.08 (MIT): the resource manager
  headers NVK is built against, and the reference the r387 layouts were compared with.
- [open-gpu-doc](https://github.com/NVIDIA/open-gpu-doc) (MIT): class and method headers for the Maxwell engines. Three
  of them (`cla0c0.h`, `cla197.h`, `cla1c0.h`) are added to the Mesa tree by our patch, with NVIDIA's notice intact.
- The NVIDIA Web Driver itself is proprietary. It is required at run time and is **not** included or modified.

### metal2vulkan

<https://github.com/steelbrain/metal2vulkan> · Copyright (C) Anees Iqbal · LGPL-3.0-or-later

The base of NullMoth's shader translator. Not included here.

## Work that made the test machine possible

- **[Acidanthera](https://github.com/acidanthera)**: OpenCore, Lilu and WhateverGreen.
- **[OpenCore Legacy Patcher](https://github.com/dortania/OpenCore-Legacy-Patcher)** and **AMFIPass**, by Dortania and contributors.
- **LLVM**, used by the shader runtime.

## How this was written

Most of the code and analysis in this repository was written by the author working with AI coding assistants,
mainly Claude Code (Anthropic), and tested by hand on real hardware. Treat it accordingly: it has had one reviewer and
one test machine.

Mesa requires AI involvement to be disclosed in commits (`Assisted-by:` or `Generated-by:` trailers) and has its own
rules for such contributions. Anyone who wants to take part of the Mesa patch upstream needs to rewrite or review it to
that standard first.

## Trademarks

NVIDIA and GeForce are trademarks of NVIDIA Corporation. Apple, macOS and Metal are trademarks of Apple Inc. This
project is not affiliated with or endorsed by either, or by NullMoth Systems.
