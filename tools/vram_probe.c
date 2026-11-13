/*
 * vram_probe - VRAM fisica na r387 (NVOS32 ALLOC_SIZE sem VIRTUAL, como o AllocPageLevel do
 * GeForceWeb), mapa na CPU pela BAR1, e virtual com page kind no campo certo (format@0x44).
 * So aloca e mapeia; nada executa na GPU.
 *
 * Uso: sudo ./vram_probe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "../nvrm_r387.h"

static io_connect_t gConn;
static int esc(NvU32 e, void *p, NvU32 size)
{
    uint8_t buf[1024];
    NV387_NVDAUSER_HEADER h = { NV387_ESC_BASE | e, size };
    memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), p, size);
    size_t in = sizeof(h) + size, out = in;
    if (IOConnectCallStructMethod(gConn, NV387_NVDAUSER_SEL_RM_API, buf, in, buf, &out) || out != in) return -1;
    memcpy(p, buf + sizeof(h), size);
    return 0;
}
static NvU32 alloc(NvHandle c, NvHandle par, NvHandle h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS_R387 a = { .hRoot = c, .hObjectParent = par, .hObjectNew = h, .hClass = cls,
                                 .pAllocParms = (NvU64)(uintptr_t)p, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_ALLOC, &a, sizeof(a)) ? 0xEEEEEEEE : a.status;
}

#define ATTR_WC        (2u << 29)
#define ATTR_CONTIG    (2u << 27)
#define ATTR_PAGE_4K   (1u << 23)
#define ATTR_PAGE_BIG  (2u << 23)
#define ATTR_BL        (2u << 16)
#define FL_HANDLE      0x4000u
#define FL_NO_SCANOUT  0x1000u
#define FL_IGNORE_BANK 0x0001u
#define FL_VIRTUAL     0x80000u
#define FL_MAP_NOT_REQ 0x8000u

static NvU32 heap(NvHandle c, NvHandle hDev, NvHandle h, NvU32 flags, NvU32 attr, NvU32 format, NvU64 size,
                  NvU64 align, NVOS32_ALLOC_R387 *out, const char *nome)
{
    NVOS32_ALLOC_R387 v;
    memset(&v, 0, sizeof(v));
    v.hRoot = c; v.hObjectParent = hDev; v.function = 2; v.status = 0xFFFFFFFF;
    v.owner = 0x6e766b20; v.hMemory = h; v.type = 0; v.flags = flags;
    v.attr = attr; v.format = format; v.size = size; v.alignment = align;
    if (esc(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v))) return 0xEEEEEEEE;
    printf("  %-34s status 0x%x h 0x%x offset 0x%llx limit 0x%llx size 0x%llx attr 0x%08x->0x%08x format 0x%x->0x%x\n",
           nome, v.status, v.hMemory, (unsigned long long)v.offset, (unsigned long long)v.limit,
           (unsigned long long)v.size, attr, v.attr, format, v.format);
    if (out) *out = v;
    return v.status;
}
static NvU32 map_cpu(NvHandle c, NvHandle hDev, NvHandle hMem, NvU64 len, volatile uint32_t **p)
{
    NVOS33_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hMemory = hMem, .offset = 0, .length = len,
                                 .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY, &m, sizeof(m))) return 0xEEEEEEEE;
    *p = (volatile uint32_t *)(uintptr_t)m.pLinearAddress;
    return m.status;
}
static NvU32 map_dma(NvHandle c, NvHandle hDev, NvHandle hDma, NvHandle hMem, NvU32 flags, NvU64 len, NvU64 va)
{
    NVOS46_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hDma = hDma, .hMemory = hMem, .offset = 0,
                                 .length = len, .flags = flags, .dmaOffset = va, .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY_DMA, &m, sizeof(m))) return 0xEEEEEEEE;
    printf("  MAP_MEMORY_DMA mem 0x%x -> virtual 0x%x flags 0x%x VA 0x%llx: status 0x%x dmaOffset 0x%llx\n",
           hMem, hDma, flags, (unsigned long long)va, m.status, (unsigned long long)m.dmaOffset);
    return m.status;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    int okVram = 0, okCpu = 0, okKind = 0, okMapV = 0, okMapS = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }

    printf("[1] VRAM fisica 64 KiB (NVOS32 ALLOC_SIZE, LOCATION_VIDMEM)\n");
    const NvHandle hV = 0xCAFE0040, hV2 = 0xCAFE0041;
    NVOS32_ALLOC_R387 vr;
    NvU32 st = heap(hc, hDev, hV, FL_HANDLE | FL_NO_SCANOUT | FL_IGNORE_BANK, ATTR_WC | ATTR_PAGE_4K, 0,
                    0x10000, 0x10000, &vr, "VRAM WC pagina 4K");
    if (st) st = heap(hc, hDev, hV, FL_HANDLE | FL_NO_SCANOUT | FL_IGNORE_BANK, ATTR_WC | ATTR_CONTIG | ATTR_PAGE_4K, 0,
                      0x10000, 0x10000, &vr, "VRAM WC contigua 4K (como o GeForceWeb)");
    okVram = st == 0 && vr.hMemory == hV;
    if (!okVram) goto out;
    NVOS32_ALLOC_R387 vr2;
    heap(hc, hDev, hV2, FL_HANDLE | FL_NO_SCANOUT | FL_IGNORE_BANK, ATTR_WC | ATTR_PAGE_BIG, 0, 0x20000, 0x20000,
         &vr2, "VRAM WC pagina grande 128 KiB");

    printf("[2] mapa da VRAM na CPU (BAR1)\n");
    volatile uint32_t *cpu = NULL;
    st = map_cpu(hc, hDev, hV, 0x10000, &cpu);
    printf("  MAP_MEMORY: status 0x%x ptr %p\n", st, (void *)cpu);
    if (!st && cpu && (uintptr_t)cpu < 0x00007FFFFFFFF000ull) {
        for (int i = 0; i < 0x4000; i++) cpu[i] = 0xA5000000u | (uint32_t)i;
        __sync_synchronize();
        int bad = 0;
        for (int i = 0; i < 0x4000; i++) if (cpu[i] != (0xA5000000u | (uint32_t)i)) bad++;
        printf("  escrita/leitura de 16384 palavras pela BAR1: %d erradas\n", bad);
        okCpu = bad == 0;
    }

    printf("[3] virtual com kind 0xFE no campo certo (attr@0x40 BLOCK_LINEAR, format@0x44)\n");
    const NvHandle hK = 0xCAFE50FE;
    NVOS32_ALLOC_R387 kv;
    st = heap(hc, hDev, hK, FL_HANDLE | FL_VIRTUAL | FL_MAP_NOT_REQ | FL_NO_SCANOUT | FL_IGNORE_BANK, ATTR_BL, 0xFE,
              0x20000, 0x20000, &kv, "virtual kind 0xFE BL");
    okKind = st == 0;
    if (okKind) {
        printf("[4] mapas no virtual com PAGE_KIND_VIRTUAL + offset fixo\n");
        okMapV = map_dma(hc, hDev, hK, hV, NV387_NVOS46_FLAGS_PAGE_KIND_VIRTUAL | NV387_NVOS46_FLAGS_DMA_OFFSET_FIXED,
                         0x10000, kv.offset) == 0;
        NVOS02_PARAMETERS_R387 m = { .hRoot = hc, .hObjectParent = hDev, .hObjectNew = 0xCAFE003E, .hClass = 0x3E,
                                     .flags = 0x2010, .limit = 0x10000 - 1, .status = 0xFFFFFFFF };
        esc(NV387_ESC_RM_ALLOC_MEMORY, &m, sizeof(m));
        if (!m.status)
            okMapS = map_dma(hc, hDev, hK, 0xCAFE003E,
                             NV387_NVOS46_FLAGS_PAGE_KIND_VIRTUAL | NV387_NVOS46_FLAGS_DMA_OFFSET_FIXED,
                             0x10000, kv.offset + 0x10000) == 0;
    }

out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
    printf("\nFREE cliente: status 0x%x\n", f.status);
    printf("RESUMO: VRAM %s | BAR1 %s | virtual kind %s | mapa VRAM kind %s | mapa sistema kind %s\n",
           okVram ? "ok" : "FALHOU", okCpu ? "ok" : "FALHOU", okKind ? "ok" : "FALHOU",
           okMapV ? "ok" : "FALHOU", okMapS ? "ok" : "FALHOU");
    IOServiceClose(gConn);
    return 0;
}
