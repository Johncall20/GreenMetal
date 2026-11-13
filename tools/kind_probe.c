/*
 * kind_probe - testa na r387 a alocacao virtual NATIVA com page kind (NVOS32 ALLOC_SIZE + VIRTUAL,
 * como o GeForceWeb faz em nvMemoryManagerDevice::AllocVirtualAddressRange), um CTXDMA sobre ela
 * e o mapa da memoria com PAGE_KIND_VIRTUAL. Testa o virtual devolvido como hDma e CTXDMAs
 * com faixa absoluta/relativa. Em cada modo tenta o endereco fixo pedido pelo NVK e, se falhar,
 * o endereco escolhido pelo RM. So aloca e mapeia; nada executa na GPU.
 *
 * Uso: sudo ./kind_probe
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

static NvU32 virt_alloc(NvHandle c, NvHandle hDev, NvU32 kind, NvU32 attr, NvU64 size,
                        NvHandle *hVirt, NvU64 *va, NvU64 *limit)
{
    NVOS32_ALLOC_R387 v;
    memset(&v, 0, sizeof(v));
    v.hRoot = c; v.hObjectParent = hDev; v.function = 2; v.status = 0xFFFFFFFF;
    v.owner = 0x6d6d7661; v.hMemory = 0; v.type = 0;
    v.flags = 0x80000 | 0x8000 | 0x1;             /* VIRTUAL | MAP_NOT_REQUIRED | IGNORE_BANK_PLACEMENT */
    v.attr = attr; v.format = kind; v.size = size; v.alignment = 0x20000;
    if (esc(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v))) return 0xEEEEEEEE;
    *hVirt = v.hMemory; *va = v.offset; *limit = v.limit;
    printf("    NVOS32 VIRTUAL kind 0x%02x attr 0x%x: status 0x%x hMemory 0x%x VA 0x%llx limit 0x%llx format-> 0x%x attr-> 0x%x\n",
           kind, attr, v.status, v.hMemory, (unsigned long long)v.offset, (unsigned long long)v.limit, v.format, v.attr);
    return v.status;
}
static NvU32 ctx_dma(NvHandle c, NvHandle h, NvHandle hMem, NvU64 offset, NvU64 limit)
{
    NVOS39_PARAMETERS_R387 d = { .hObjectParent = c, .hObjectNew = h, .hClass = 0x2,
                                 .flags = 0x20000000, .hMemory = hMem, .offset = offset,
                                 .limit = limit, .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_ALLOC_CONTEXT_DMA2, &d, sizeof(d))) return 0xEEEEEEEE;
    printf("    CTXDMA 0x%x offset 0x%llx limit 0x%llx: status 0x%x\n", h,
           (unsigned long long)offset, (unsigned long long)limit, d.status);
    return d.status;
}
static NvU32 map_dma(NvHandle c, NvHandle hDev, NvHandle hDma, NvHandle hMem, NvU32 flags, NvU64 len,
                     NvU64 requested, NvU64 *actual)
{
    NVOS46_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hDma = hDma, .hMemory = hMem, .offset = 0,
                                 .length = len, .flags = flags, .dmaOffset = requested, .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY_DMA, &m, sizeof(m))) return 0xEEEEEEEE;
    *actual = m.dmaOffset;
    return m.status;
}
static NvU32 map_both(NvHandle c, NvHandle hDev, NvHandle hDma, NvHandle hMem, NvU32 flags,
                      NvU64 va, const char *mode, int *fixedOk)
{
    NvU64 actual = 0;
    NvU32 st = map_dma(c, hDev, hDma, hMem, flags | 0x8000, 0x10000, va, &actual);
    printf("    %s FIXO flags 0x%x VA 0x%llx: status 0x%x dmaOffset 0x%llx%s\n", mode,
           flags | 0x8000, (unsigned long long)va, st, (unsigned long long)actual,
           st == 0 && actual == va ? "  (= VA do virtual)" : "");
    *fixedOk = st == 0 && actual == va;
    if (st) {
        actual = 0;
        st = map_dma(c, hDev, hDma, hMem, flags, 0x10000, 0, &actual);
        printf("    %s RM ESCOLHE flags 0x%x: status 0x%x dmaOffset 0x%llx%s\n", mode, flags, st,
               (unsigned long long)actual, st == 0 && actual == va ? "  (= VA do virtual)" : "");
    }
    return st;
}

int main(void)
{
    int targetFixedOk = 0;
    if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hMem = 0xCAFE003E;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    NVOS02_PARAMETERS_R387 m = { .hRoot = hc, .hObjectParent = hDev, .hObjectNew = hMem, .hClass = 0x3E,
                                 .flags = 0x2010, .limit = 0x10000 - 1, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_ALLOC_MEMORY, &m, sizeof(m));
    printf("memoria de sistema 64 KiB WC: status 0x%x\n\n", m.status);
    if (m.status) goto out;

    static const struct { const char *nome; NvU32 kind, attr, mflags; } T[] = {
        { "A kind 0xFE + BLOCK_LINEAR, map PAGE_KIND_VIRTUAL", 0xFE, 0x20000, 0x8 },
        { "B kind 0xFE sem attr,       map PAGE_KIND_VIRTUAL", 0xFE, 0x0,     0x8 },
        { "C kind 0 (pitch),           map PAGE_KIND_VIRTUAL", 0x00, 0x0,     0x8 },
        { "D kind 0xFE + BLOCK_LINEAR, map PAGE_KIND_PHYSICAL", 0xFE, 0x20000, 0x0 },
        { "E kind 0xFE + BL, map VIRTUAL + CACHE_SNOOP",        0xFE, 0x20000, 0x18 },
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        NvHandle hv = 0, hCtx = 0xCAFE6000 + i;
        NvU64 va = 0, limit = 0;
        int fixedOk = 0;
        printf("[%c] %s\n", 'A' + i, T[i].nome);
        if (virt_alloc(hc, hDev, T[i].kind, T[i].attr, 0x10000, &hv, &va, &limit) || !hv) continue;
        NvU32 st = map_both(hc, hDev, hv, hMem, T[i].mflags, va, "DIRETO", &fixedOk);
        if (i == 0 && fixedOk) targetFixedOk = 1;
        if (!st) continue;
        if (ctx_dma(hc, hCtx, hv, va, va + limit)) continue;
        st = map_both(hc, hDev, hCtx, hMem, T[i].mflags, va, "CTX ABS", &fixedOk);
        if (i == 0 && fixedOk) targetFixedOk = 1;
        if (!st) continue;
        hCtx = 0xCAFE7000 + i;
        if (ctx_dma(hc, hCtx, hv, 0, va + limit)) continue;
        st = map_both(hc, hDev, hCtx, hMem, T[i].mflags, va, "CTX 0..FIM", &fixedOk);
        if (i == 0 && fixedOk) targetFixedOk = 1;
    }

out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
    printf("\nFREE cliente: status 0x%x\n", f.status);
    printf("RESULTADO: receita A com mapa fixo %s\n", targetFixedOk ? "OK" : "FALHOU");
    IOServiceClose(gConn);
    return targetFixedOk ? 0 : 1;
}
