/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once

#define NVRM_VRAM_ABI_VERSION 2u
#define NVRM_VRAM_FN_ALLOC    "nvAllocVram"
#define NVRM_VRAM_FN_FREE     "nvFreeVram"

#define NVRM_VRAM_FN_SCANOUT  "nvScanoutInfo"
/* porte Maxwell (0.7.5 / 0.4.6): VRAM sem mapa permanente na BAR1. Pedido com flags NVRM_VRAM_FLAG_NOMAP: o backend, se
 * atender, devolve o bit em flags, kva NULL e phys de uma reserva comum (so para a familia ter um endereco valido).
 * nvMapVram / nvUnmapVram (handle + allocationCookie) dao um mapa temporario para a leitura pela CPU. */
#define NVRM_VRAM_FLAG_NOMAP  1u
#define NVRM_VRAM_FN_MAP      "nvMapVram"
#define NVRM_VRAM_FN_UNMAP    "nvUnmapVram"

#define NVRM_VRAM_BAR1_BUDGET (192ull * 1024ull * 1024ull)

struct NVRMVramRequest {
    unsigned int       version;
    unsigned int       flags;
    unsigned long long size;
    unsigned long long actualSize;
    unsigned long long phys;
    void              *kva;
    void              *handle;
    unsigned long long mappedTotal;
    unsigned int width, height, pitch;
    unsigned long long allocationCookie;
};
