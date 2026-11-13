/*
 * test_hw - integracao da libnvrm387 na GTX 960 (rodar com sudo no hackintosh).
 *
 * Faz o caminho do NVK do NullMoth (structs r615, mesmas chamadas do nvRmApi.c) ate a GPU
 * escrever um semaforo:
 *   cliente -> ID_INFO_V2 -> device -> subdevice -> nome -> CLASSLIST_V2 -> FERMI_VASPACE_A
 *   -> NV01_MEMORY_SYSTEM (ALLOC) -> MAP_MEMORY+mmap -> NV50_MEMORY_VIRTUAL -> MAP_MEMORY_DMA
 *   (offset fixo no VA da lib) -> error notifier (NV01_CONTEXT_DMA por ALLOC) -> canal 0xB06F
 *   (NV_CHANNEL_ALLOC_PARAMS r615) -> USERD -> MAXWELL_COMPUTE_B -> GPFIFO_SCHEDULE (r615)
 *   -> pushbuffer SEMAPHORE RELEASE -> GPPut -> espera.
 * Ao sair libera o cliente (o RM desmonta tudo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0080.h"
#include "alloc/alloc_channel.h"
#include "nvrm_xnu.h"

static int g_fd;
static NvHandle g_client;

static NvU32 api_alloc(NvHandle parent, NvHandle *h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS a = { .hRoot = g_client, .hObjectParent = parent, .hObjectNew = *h, .hClass = cls, .pAllocParms = p };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_ALLOC, &a, sizeof(a))) return 0xEEEEEEEE;
    *h = a.hObjectNew;
    return a.status;
}
static NvU32 api_ctrl(NvHandle o, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS k = { .hClient = g_client, .hObject = o, .cmd = cmd, .params = p, .paramsSize = size };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_CONTROL, &k, sizeof(k))) return 0xEEEEEEEE;
    return k.status;
}
/* como nvRmApiMapMemory: fd proprio, MAP_MEMORY com fd, mmap(fd) */
static void *api_map(NvHandle hDevice, NvHandle hMem, NvU64 len, NvU32 *st)
{
    int memFd = nvrm_xnu_open("/dev/nvidiactl");
    nv_ioctl_nvos33_parameters_with_fd p = { .params = { .hClient = g_client, .hDevice = hDevice, .hMemory = hMem,
                                                        .length = len }, .fd = memFd };
    void *ptr = NULL;
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY, &p, sizeof(p))) { *st = 0xEEEEEEEE; goto out; }
    *st = p.params.status;
    if (*st == NV_OK) { ptr = nvrm_xnu_mmap(memFd, len); if (ptr == MAP_FAILED) ptr = NULL; }
out:
    nvrm_xnu_close(memFd);
    return ptr;
}

#define STEP(fmt, ...) printf("[hw] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do { printf("[hw] FALHOU: " fmt "\n", ##__VA_ARGS__); rc = 1; goto done; } while (0)

int main(void)
{
    int rc = 0;
    if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    g_fd = nvrm_xnu_open("/dev/nvidiactl");
    if (g_fd < 0) { perror("nvrm_xnu_open"); return 1; }

    nv_ioctl_card_info_t ci[4];
    memset(ci, 0, sizeof(ci));
    nvrm_xnu_ioctl(g_fd, NV_ESC_CARD_INFO, ci, sizeof(ci));
    STEP("CARD_INFO: valid %u gpu_id 0x%x pci %04x:%04x", ci[0].valid, ci[0].gpu_id,
         ci[0].pci_info.vendor_id, ci[0].pci_info.device_id);

    NvHandle hc = 0;
    NvU32 st = api_alloc(0, &hc, 0x41, NULL);
    if (st) FAIL("cliente 0x%x", st);
    g_client = hc;
    STEP("cliente 0x%x", hc);

    struct { NvU32 gpuId, gpuFlags, devInst, subInst, sli, board, gi; NvS32 numa; } ii = { .gpuId = ci[0].gpu_id };
    st = api_ctrl(hc, 0x205, &ii, sizeof(ii));
    STEP("GPU_GET_ID_INFO_V2: status 0x%x deviceInstance %u", st, ii.devInst);
    if (st) FAIL("ID_INFO_V2");

    NV0080_ALLOC_PARAMETERS dp = { .deviceId = ii.devInst, .hClientShare = hc,
                                   .vaMode = NV_DEVICE_ALLOCATION_VAMODE_MULTIPLE_VASPACES };
    NvHandle hDev = 0xCAFE0080;
    if ((st = api_alloc(hc, &hDev, 0x80, &dp))) FAIL("device 0x%x", st);
    NvU32 sdp = 0; NvHandle hSub = 0xCAFE2080;
    if ((st = api_alloc(hDev, &hSub, 0x2080, &sdp))) FAIL("subdevice 0x%x", st);
    STEP("device/subdevice ok");

    struct { NvU32 n; NvU32 l[200]; } cl;
    memset(&cl, 0, sizeof(cl));
    if ((st = api_ctrl(hDev, 0x800292, &cl, sizeof(cl)))) FAIL("CLASSLIST_V2 0x%x", st);
    int hasB06F = 0;
    for (NvU32 i = 0; i < cl.n; i++) if (cl.l[i] == 0xB06F) hasB06F = 1;
    STEP("CLASSLIST_V2: %u classes, 0xB06F %s", cl.n, hasB06F ? "presente" : "AUSENTE");

    /* sequencia de inicializacao do nvkmd_nvrm_pdev.c (structs r615, pela lib) */
    { NvU32 arch[4] = { 0 };
      st = api_ctrl(hSub, 0x20801701, arch, 16);
      STEP("MC_GET_ARCH_INFO: status 0x%x chipset 0x%x", st, arch[0] | arch[1]);
      if (st) FAIL("MC_GET_ARCH_INFO");
      struct { NvU32 flags; char s[64]; } nm2 = { 0 };
      st = api_ctrl(hSub, 0x20800110, &nm2, sizeof(nm2));
      STEP("NAME_STRING (68 bytes r615): status 0x%x \"%s\"", st, nm2.s);
      if (st) FAIL("NAME_STRING");
      char sh[64] = { 0 };
      st = api_ctrl(hSub, 0x20800111, sh, sizeof(sh));
      STEP("SHORT_NAME_STRING: status 0x%x \"%s\"", st, sh);
      if (st) FAIL("SHORT_NAME_STRING");
      struct { NvU32 n; struct { NvU32 index, data; } l[0x3F]; NvU32 pad[5]; } gr;
      memset(&gr, 0, sizeof(gr));
      gr.n = 3; gr.l[0].index = 0x0C; gr.l[1].index = 0x0D; gr.l[2].index = 0x20;
      st = api_ctrl(hDev, 0x801110, &gr, sizeof(gr));
      STEP("GR_GET_INFO_V2: status 0x%x SM_VERSION 0x%x MAX_WARPS_PER_SM %u SM_PER_TPC %u", st,
           gr.l[0].data, gr.l[1].data, gr.l[2].data);
      if (st || gr.l[0].data != 0x502) FAIL("GR_GET_INFO_V2");
      NvU32 gm[6] = { 0 }, gpcCount = 0, tpcCount = 0;
      st = api_ctrl(hSub, 0x2080122A, gm, 24);
      if (st) FAIL("GPC_MASK 0x%x", st);
      for (NvU32 g = 0; g < 32; g++) {
          if (!(gm[4] & (1u << g))) continue;
          gpcCount++;
          NvU32 tm[6] = { 0 }; tm[4] = g;
          api_ctrl(hSub, 0x2080122B, tm, 24);
          tpcCount += (NvU32)__builtin_popcount(tm[5]);
      }
      STEP("GPC/TPC (derivados): gpcMask 0x%x -> %u GPCs, %u TPCs", gm[4], gpcCount, tpcCount);
      struct { NvU32 n; struct { NvU32 index, data; } l[128]; } fb;
      memset(&fb, 0, sizeof(fb));
      fb.n = 4; fb.l[0].index = 7; fb.l[1].index = 5; fb.l[2].index = 8; fb.l[3].index = 0x16;
      st = api_ctrl(hSub, 0x20801303, &fb, sizeof(fb));
      STEP("FB_GET_INFO_V2: status 0x%x VRAM %u MiB, BAR1 %u MiB, total %u MiB, livre %u MiB", st,
           fb.l[0].data / 1024, fb.l[1].data / 1024, fb.l[2].data / 1024, fb.l[3].data / 1024);
      if (st) FAIL("FB_GET_INFO_V2");
      NvU64 t = 0;
      st = api_ctrl(hSub, 0x20800403, &t, 8);
      STEP("TIMER_GET_TIME: status 0x%x %llu ns", st, (unsigned long long)t);
      if (st) FAIL("TIMER_GET_TIME");
    }

    NV_VASPACE_ALLOCATION_PARAMETERS vs = { .flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
                                            .vaBase = 0x200000000ull };
    NvHandle hVas = 0xCAFE90F1;
    if ((st = api_alloc(hDev, &hVas, 0x90F1, &vs))) FAIL("VASPACE 0x%x", st);

    /* memoria: 64 KiB (GPFIFO @0, pushbuffer @0x1000, semaforo @0x2000) + 4 KiB notifier */
    NV_MEMORY_ALLOCATION_PARAMS mp = { .owner = hc, .type = NVOS32_TYPE_IMAGE, .size = 0x10000 };
    NvHandle hMem = 0xCAFE003E;
    if ((st = api_alloc(hDev, &hMem, 0x3E, &mp))) FAIL("NV01_MEMORY_SYSTEM 0x%x", st);
    NV_MEMORY_ALLOCATION_PARAMS np = { .owner = hc, .type = NVOS32_TYPE_IMAGE, .size = 0x1000 };
    NvHandle hNotif = 0xCAFE003F;
    if ((st = api_alloc(hDev, &hNotif, 0x3E, &np))) FAIL("notifier 0x%x", st);
    volatile NvU32 *mem = api_map(hDev, hMem, 0x10000, &st);
    if (!mem) FAIL("MAP_MEMORY memoria 0x%x", st);
    volatile NvU32 *notif = api_map(hDev, hNotif, 0x1000, &st);
    if (!notif) FAIL("MAP_MEMORY notifier 0x%x", st);
    for (int i = 0; i < 0x10000 / 4; i++) mem[i] = 0;
    for (int i = 0; i < 0x1000 / 4; i++) notif[i] = 0;
    mem[0x2000 / 4] = 0x12345678;
    STEP("memoria de sistema mapeada em %p, leitura 0x%08x", (void *)mem, mem[0x2000 / 4]);
    if (mem[0x2000 / 4] != 0x12345678) FAIL("leitura pela CPU");

    /* VA estilo NVK: NV50_MEMORY_VIRTUAL + MAP_MEMORY_DMA com dmaOffset dentro do virtual */
    NV_MEMORY_ALLOCATION_PARAMS vp = { .owner = hc, .type = NVOS32_TYPE_IMAGE, .flags = NVOS32_ALLOC_FLAGS_VIRTUAL,
                                       .size = 0x10000, .hVASpace = hVas };
    NvHandle hVirt = 0xCAFE50A0;
    if ((st = api_alloc(hDev, &hVirt, 0x50A0, &vp))) FAIL("NV50_MEMORY_VIRTUAL 0x%x", st);
    NVOS46_PARAMETERS md = { .hClient = hc, .hDevice = hDev, .hDma = hVirt, .hMemory = hMem, .offset = 0,
                             .length = 0x10000, .flags = DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE), .dmaOffset = 0 };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &md, sizeof(md)) || md.status)
        FAIL("MAP_MEMORY_DMA com offset fixo no VA da lib (VA 0x%llx): status 0x%x",
             (unsigned long long)vp.offset, md.status);
    const NvU64 va = vp.offset + md.dmaOffset;
    STEP("GPU VA 0x%llx (virtual da lib + offset fixo)", (unsigned long long)va);

    NV_MEMORY_ALLOCATION_PARAMS kp = { .owner = hc, .type = NVOS32_TYPE_IMAGE,
                                       .flags = NVOS32_ALLOC_FLAGS_VIRTUAL,
                                       .attr = DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR),
                                       .format = 0xFE, .size = 0x10000, .hVASpace = hVas };
    NvHandle hKind = 0xCAFE50FE;
    if ((st = api_alloc(hDev, &hKind, 0x50A0, &kp))) FAIL("NVOS32 VIRTUAL kind 0xFE: 0x%x", st);
    NVOS46_PARAMETERS km = { .hClient = hc, .hDevice = hDev, .hDma = hKind, .hMemory = hMem,
                             .length = 0x10000,
                             .flags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL) |
                                      DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE),
                             .dmaOffset = 0 };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &km, sizeof(km)) || km.status)
        FAIL("MAP kind 0xFE (VA 0x%llx): status 0x%x", (unsigned long long)kp.offset, km.status);
    if (kp.format != 0xFE || km.dmaOffset != 0) FAIL("kind/format ou offset relativo incorreto");
    STEP("NVOS32 VIRTUAL kind 0xFE: VA 0x%llx, mapa direto com PAGE_KIND_VIRTUAL ok",
         (unsigned long long)(kp.offset + km.dmaOffset));
    NVOS47_PARAMETERS ku = { .hClient = hc, .hDevice = hDev, .hDma = hKind, .hMemory = hMem,
                             .flags = km.flags, .dmaOffset = km.dmaOffset };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_UNMAP_MEMORY_DMA, &ku, sizeof(ku)) || ku.status)
        FAIL("UNMAP kind 0xFE: status 0x%x", ku.status);
    NVOS00_PARAMETERS kf = { .hRoot = hc, .hObjectOld = hKind };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_FREE, &kf, sizeof(kf)) || kf.status)
        FAIL("FREE kind 0xFE: status 0x%x", kf.status);

    NV_CONTEXT_DMA_ALLOCATION_PARAMS cd = { .flags = 0x20000000, .hMemory = hNotif, .offset = 0, .limit = 0xFFF };
    NvHandle hCtxErr = 0xCAFE0002;
    if ((st = api_alloc(hDev, &hCtxErr, 0x2, &cd))) FAIL("NV01_CONTEXT_DMA 0x%x", st);

    NV_CHANNEL_ALLOC_PARAMS ch;
    memset(&ch, 0, sizeof(ch));
    ch.hObjectError = hCtxErr; ch.gpFifoOffset = va; ch.gpFifoEntries = 0x200; ch.hVASpace = hVas;
    ch.engineType = 1; /* NV2080_ENGINE_TYPE_GRAPHICS */
    NvHandle hChan = 0xCAFEB06F;
    if ((st = api_alloc(hDev, &hChan, 0xB06F, &ch))) FAIL("canal 0x%x", st);
    volatile NvU32 *userd = api_map(hDev, hChan, 0x200, &st);
    if (!userd) FAIL("USERD 0x%x", st);
    STEP("canal 0xB06F, USERD em %p: GPGet 0x%x GPPut 0x%x", (void *)userd, userd[0x88 / 4], userd[0x8C / 4]);

    NvHandle hEng = 0xCAFEB1C0;
    if ((st = api_alloc(hChan, &hEng, 0xB1C0, NULL))) FAIL("MAXWELL_COMPUTE_B 0x%x", st);
    NvU8 sch[3] = { 1, 0, 0 };                     /* NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS r615 */
    if ((st = api_ctrl(hChan, 0xA06F0103, sch, sizeof(sch)))) FAIL("GPFIFO_SCHEDULE 0x%x", st);
    STEP("engine + schedule ok");

    const NvU64 pbVA = va + 0x1000, semVA = va + 0x2000;
    volatile NvU32 *pb = mem + 0x1000 / 4;
    pb[0] = (1u << 29) | (4u << 16) | (0x10u >> 2);
    pb[1] = (NvU32)(semVA >> 32) & 0xFF;
    pb[2] = (NvU32)semVA & ~3u;
    pb[3] = 0xCAFEF00D;
    pb[4] = 0x2u | (1u << 24);
    mem[0] = (NvU32)pbVA & ~3u;
    mem[1] = ((NvU32)(pbVA >> 32) & 0xFF) | (5u << 10);
    mem[0x2000 / 4] = 0;
    __sync_synchronize();
    __asm__ volatile("sfence" ::: "memory");
    userd[0x8C / 4] = 1;
    NvU32 v = 0; int ms;
    for (ms = 0; ms < 2000; ms++) { v = mem[0x2000 / 4]; if (v == 0xCAFEF00D) break; usleep(1000); }
    STEP("semaforo 0x%08x apos %d ms | GPGet 0x%x GPPut 0x%x | notifier erro 0x%08x/0x%04x",
         v, ms, userd[0x88 / 4], userd[0x8C / 4], notif[2], notif[3] >> 16);
    if (v != 0xCAFEF00D) FAIL("a GPU nao escreveu o semaforo");
    STEP("=> A GPU EXECUTOU O PUSHBUFFER PELA LIBNVRM387 (caminho do NVK)");

done:
    if (g_client) {
        NVOS00_PARAMETERS f = { .hRoot = g_client, .hObjectOld = g_client };
        nvrm_xnu_ioctl(g_fd, NV_ESC_RM_FREE, &f, sizeof(f));
        STEP("FREE cliente: status 0x%x", f.status);
    }
    nvrm_xnu_close(g_fd);
    printf("RESULTADO: %s\n", rc ? "falhou" : "ok");
    return rc;
}
