/*
 * zkind_probe - quais page kinds de depth/stencil da Maxwell a r387 aceita na alocacao virtual nativa
 * (NVOS32 ALLOC_SIZE + VIRTUAL, format = kind) e se a VRAM mapeia nela (PAGE_KIND_VIRTUAL + offset fixo).
 * Variantes: so BLOCK_LINEAR (tipo IMAGE) / BL + atributos de Z (tipo DEPTH ou STENCIL) / sem atributo.
 * So aloca e mapeia; nada executa na GPU. Roda sem root com o MaxwellMetal carregado.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
static NvU32 heap(NvHandle c, NvHandle hDev, NvHandle h, NvU32 type, NvU32 flags, NvU32 attr, NvU32 format,
                  NvU64 size, NVOS32_ALLOC_R387 *out)
{
    NVOS32_ALLOC_R387 v;
    memset(&v, 0, sizeof(v));
    v.hRoot = c; v.hObjectParent = hDev; v.function = 2; v.status = 0xFFFFFFFF;
    v.owner = 0x7a6b6e64; v.hMemory = h; v.type = type; v.flags = flags;
    v.attr = attr; v.format = format; v.size = size; v.alignment = 0x20000;
    if (esc(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v))) return 0xEEEEEEEE;
    if (out) *out = v;
    return v.status;
}
static NvU32 map_dma(NvHandle c, NvHandle hDev, NvHandle hDma, NvHandle hMem, NvU32 flags, NvU64 len, NvU64 va)
{
    NVOS46_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hDma = hDma, .hMemory = hMem, .offset = 0,
                                 .length = len, .flags = flags, .dmaOffset = va, .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY_DMA, &m, sizeof(m))) return 0xEEEEEEEE;
    return m.status ? m.status : (m.dmaOffset == va ? 0 : 0xBADADD);
}
static void unmap_free(NvHandle c, NvHandle hDev, NvHandle hv, NvHandle hMem, NvU64 va, int mapped)
{
    if (mapped) {
        NVOS47_PARAMETERS_R387 u = { .hClient = c, .hDevice = hDev, .hDma = hv, .hMemory = hMem, .dmaOffset = va,
                                     .status = 0xFFFFFFFF };
        esc(NV387_ESC_RM_UNMAP_MEMORY_DMA, &u, sizeof(u));
    }
    NVOS00_PARAMETERS_R387 f = { .hRoot = c, .hObjectParent = hDev, .hObjectOld = hv, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
}

#define BL        (2u << 16)
#define WC        (2u << 29)
#define PAGE_BIG  (2u << 23)
#define FL_HANDLE 0x4000u
#define FL_NOSCAN 0x1000u
#define FL_IGNB   0x0001u
#define FL_VIRT   0x80000u
#define FL_NOMAP  0x8000u
#define ZA(depth, ztype, pack) (((depth) & 7u) | ((ztype) << 18) | ((pack) << 19))

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hV = 0xCAFE0040;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0, st;
    int okAll = 1;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    st = heap(hc, hDev, hV, 0, FL_HANDLE | FL_NOSCAN | FL_IGNB, WC | PAGE_BIG, 0, 0x20000, NULL);
    printf("VRAM fisica 128 KiB: status 0x%x\n", st);
    if (st) goto out;

    /* kind, nome, tipo Z, atributos Z (DEPTH, Z_TYPE, ZS_PACKING) */
    static const struct { NvU32 kind; const char *nome; NvU32 ztype; NvU32 zattr; } K[] = {
        { 0x01, "Z16",        1,  ZA(2, 0, 7) },
        { 0x11, "S8Z24",      1,  ZA(4, 0, 1) },
        { 0x46, "Z24S8",      1,  ZA(4, 0, 0) },
        { 0x2a, "S8 (STENCIL, DEPTH_8)", 16, ZA(1, 0, 0) },
        { 0x2a, "S8 (DEPTH, DEPTH_8)",   1,  ZA(1, 0, 0) },
        { 0x2a, "S8 (STENCIL, DEPTH_8, sem BL)", 16, ZA(1, 0, 0) | 0x80000000u },
        { 0x2a, "S8 (DEPTH, DEPTH_8, S8)", 1, ZA(1, 0, 0) | 0x40000000u },
        { 0x7b, "ZF32",       1,  ZA(4, 1, 2) },
        { 0xc3, "ZF32_X24S8", 1,  ZA(5, 1, 5) },
        { 0xfe, "GENERIC_16BX2 (controle)", 0, 0 },
    };
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        struct { const char *v; NvU32 type, attr; } V[3] = {
            { "BL tipo IMAGE  ", 0, BL }, { "BL + Z attr    ", K[i].ztype, (K[i].zattr & 0x80000000u) ? (K[i].zattr & 0x3fffffffu) : (BL | (K[i].zattr & 0x3fffffffu)) }, { "sem attr       ", 0, 0 } };
        int any = 0;
        printf("kind 0x%02x %s\n", K[i].kind, K[i].nome);
        for (int j = 0; j < 3; j++) {
            NVOS32_ALLOC_R387 vo;
            NvHandle hv = 0xCAFE5000 + i * 4 + j;
            st = heap(hc, hDev, hv, V[j].type, FL_HANDLE | FL_VIRT | FL_NOMAP | FL_NOSCAN | FL_IGNB, V[j].attr,
                      K[i].kind, 0x20000, &vo);
            if (st) { printf("  %s alloc 0x%x\n", V[j].v, st); continue; }
            NvU32 ms = map_dma(hc, hDev, hv, hV, NV387_NVOS46_FLAGS_PAGE_KIND_VIRTUAL | NV387_NVOS46_FLAGS_DMA_OFFSET_FIXED,
                               0x20000, vo.offset);
            printf("  %s alloc ok VA 0x%llx format->0x%02x attr 0x%08x->0x%08x | mapa VRAM %s (0x%x)\n", V[j].v,
                   (unsigned long long)vo.offset, vo.format, V[j].attr, vo.attr, ms ? "FALHOU" : "ok", ms);
            if (!ms && vo.format == K[i].kind) any = 1;
            unmap_free(hc, hDev, hv, hV, vo.offset, !ms);
        }
        printf("  => %s\n", any ? "ACEITO" : "nenhuma variante");
        okAll &= any;
    }
out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
    printf("\nFREE cliente: status 0x%x\nRESULTADO: %s\n", f.status, okAll ? "todos os kinds aceitos" : "algum kind recusado");
    IOServiceClose(gConn);
    return !okAll;
}
