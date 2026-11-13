/*
 * evo_base - canal base EVO do display (o que o GeForceWeb usa para o flip) aberto por user space no RM r387.
 * Classes e metodos: open-gpu-doc (cl927c.h canal base GK110/GM20x, cl5070.h objeto de display).
 *
 *   ./evo_base probe
 *       Aloca o objeto de display e o canal base do head 0 (pushbuffer de 4 KiB em VRAM), mapeia o controle
 *       PUT/GET e libera tudo. Nao manda metodo nenhum. So funciona com o canal base livre (boot sem o GeForceWeb).
 *
 *   ./evo_base show [cor 0xAARRGGBB] [segundos]
 *       Igual ao probe e mais: superficie nossa 1920x1080 A8R8G8B8 pitch-linear em VRAM, ctxdma ISO ligado ao canal,
 *       metodos SURFACE_SET_* + UPDATE (sem interlock com o core). Segura N s e libera (o canal liberado sai da tela).
 *
 * Abre o RM pela ponte do MaxwellMetal (sem root); sem ela, NVDAUser direto (root).
 * Tamanho do modo: NDRV_W / NDRV_H (padrao 1920x1080).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include "../nvrm_r387.h"

#define MM_BRIDGE_TYPE 0x4D58524Du
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
static NvU32 rmfree(NvHandle c, NvHandle par, NvHandle h)
{
    NVOS00_PARAMETERS_R387 f = { .hRoot = c, .hObjectParent = par, .hObjectOld = h, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_FREE, &f, sizeof(f)) ? 0xEEEEEEEE : f.status;
}
static NvU32 vram(NvHandle c, NvHandle hDev, NvHandle h, NvU64 size, NVOS32_ALLOC_R387 *v)
{
    memset(v, 0, sizeof(*v));
    v->hRoot = c; v->hObjectParent = hDev; v->function = NV387_NVOS32_FUNCTION_ALLOC_SIZE; v->status = 0xFFFFFFFF;
    v->owner = 0x65766f62; v->hMemory = h; v->type = 0; v->flags = 0x4000u | 0x0001u;
    v->attr = 0x4u | (2u << 8) | (1u << 23) | (2u << 27) | (2u << 29);
    v->size = size; v->alignment = 0x1000;
    if (esc(NV387_ESC_RM_VID_HEAP_CONTROL, v, sizeof(*v))) return 0xEEEEEEEE;
    return v->status;
}
static NvU32 ctxdma(NvHandle c, NvHandle hDev, NvHandle h, NvU32 flags, NvHandle hMem, NvU64 size)
{
    NVOS39_PARAMETERS_R387 d;
    memset(&d, 0, sizeof(d));
    (void)hDev; d.hObjectParent = c; d.hObjectNew = h; d.hClass = 0x2; d.flags = flags; d.hMemory = hMem;
    d.offset = 0; d.limit = size - 1; d.status = 0xFFFFFFFF;
    if (esc(NV387_ESC_RM_ALLOC_CONTEXT_DMA2, &d, sizeof(d))) return 0xEEEEEEEE;
    return d.status;
}
static NvU32 bind(NvHandle c, NvHandle hChan, NvHandle hDma)
{
    struct { NvHandle hClient, hChannel, hCtxDma; NvV32 status; } b = { c, hChan, hDma, 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_BIND_CONTEXT_DMA, &b, sizeof(b))) return 0xEEEEEEEE;
    return b.status;
}
static NvU32 map_cpu(NvHandle c, NvHandle hDev, NvHandle hMem, NvU64 len, volatile uint32_t **p)
{
    NVOS33_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hMemory = hMem, .offset = 0, .length = len,
                                 .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY, &m, sizeof(m))) return 0xEEEEEEEE;
    *p = (volatile uint32_t *)(uintptr_t)m.pLinearAddress;
    return m.status;
}
static int open_rm(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaxwellMetal"));
    if (s) {
        kern_return_t kr = IOServiceOpen(s, mach_task_self(), MM_BRIDGE_TYPE, &gConn);
        IOObjectRelease(s);
        if (!kr) return 0;
    }
    s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!s) return -1;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn);
    IOObjectRelease(s);
    return kr ? -1 : 0;
}
static void stamp(const char *what)
{
    time_t t = time(NULL);
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
    printf("  %s %s\n", b, what);
}

typedef struct {
    NvU32    channelInstance;
    NvHandle hObjectBuffer;
    NvHandle hObjectNotify;
    NvU32    offset;
    NvU64    pControl;
    NvU32    flags;
    NvU32    channelPBSize;
    NvU32    subDeviceId;
    NvU32    _pad;
} EVO_CHANNEL_DMA_ALLOC;

#define HASH_OFF   (1u << 29)
#define KIND_PITCH (2u << 16)
#define ACC_RO     1u

static volatile uint32_t *gPb, *gCtl;
static uint32_t gPut;
static void mthd(uint32_t m, uint32_t data)
{
    gPb[gPut / 4] = (1u << 18) | m;
    gPb[gPut / 4 + 1] = data;
    gPut += 8;
}

static NvU32 gHead = 0;   /* EVO_HEAD=n: o head do canal base (0 = o de sempre) */
int main(int argc, char **argv)
{
    if (getenv("EVO_HEAD")) gHead = (NvU32)strtoul(getenv("EVO_HEAD"), NULL, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    int show = argc > 1 && !strcmp(argv[1], "show");
    if (argc < 2 || (!show && strcmp(argv[1], "probe") && strcmp(argv[1], "off"))) {
        fprintf(stderr, "uso: %s probe | show [cor 0xAARRGGBB] [segundos]\n", argv[0]);
        return 2;
    }
    uint32_t color = show && argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0xFFFF00FFu;
    int secs = show && argc > 3 ? atoi(argv[3]) : 15;
    uint32_t w = getenv("NDRV_W") ? (uint32_t)strtoul(getenv("NDRV_W"), NULL, 0) : 1920;
    uint32_t h = getenv("NDRV_H") ? (uint32_t)strtoul(getenv("NDRV_H"), NULL, 0) : 1080;
    if (open_rm()) { fprintf(stderr, "nao abriu o RM\n"); return 1; }

    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente RM\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hDisp = 0xCAFE5070, hPbMem = 0xCAFE0B00,
                   hPbDma = 0xCAFE0B01, hBase = 0xCAFE927C, hSurf = 0xCAFE0D00, hIso = 0xCAFE0D01;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0, st;
    int rc = 1, haveDisp = 0, haveBase = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    printf("cliente RM 0x%x\n", hc);

    static const NvU32 dispCls[] = { 0x9570, 0x9470, 0x9270, 0x9170, 0x5070 };
    NvU32 dcls = 0;
    for (unsigned i = 0; i < sizeof dispCls / sizeof dispCls[0] && !haveDisp; i++) {
        uint32_t ap[4] = { 0, 0, 0, 0 };
        st = alloc(hc, hDev, hDisp, dispCls[i], ap);
        printf("[disp] classe 0x%04x: status 0x%x\n", dispCls[i], st);
        if (!st) { haveDisp = 1; dcls = dispCls[i]; }
    }
    if (!haveDisp) goto out;

    NVOS32_ALLOC_R387 pv;
    st = vram(hc, hDev, hPbMem, 0x1000, &pv);
    printf("[pb] VRAM 4 KiB: status 0x%x offset 0x%llx\n", st, (unsigned long long)pv.offset);
    if (st) goto out;
    st = map_cpu(hc, hDev, hPbMem, 0x1000, &gPb);
    printf("[pb] mapa BAR1: status 0x%x %p\n", st, (void *)gPb);
    if (st || !gPb) goto out;
    memset((void *)gPb, 0, 0x1000);
    st = ctxdma(hc, hDev, hPbDma, HASH_OFF | ACC_RO, hPbMem, 0x1000);
    printf("[pb] ctxdma: status 0x%x\n", st);
    if (st) goto out;

    static const NvU32 baseCls[] = { 0x927C, 0x917C, 0x907C };
    NvU32 bcls = 0;
    for (unsigned i = 0; i < sizeof baseCls / sizeof baseCls[0] && !haveBase; i++) {
        EVO_CHANNEL_DMA_ALLOC ca;
        memset(&ca, 0, sizeof(ca));
        ca.channelInstance = gHead; ca.hObjectBuffer = hPbDma; ca.subDeviceId = 1;
        st = alloc(hc, hDisp, hBase, baseCls[i], &ca);
        printf("[base] classe 0x%04x head %u: status 0x%x pControl 0x%llx\n", baseCls[i], gHead, st,
               (unsigned long long)ca.pControl);
        if (!st) { haveBase = 1; bcls = baseCls[i]; }
    }
    if (!haveBase) goto out;

    st = map_cpu(hc, hSub, hBase, 0x1000, &gCtl);
    printf("[ctl] mapa do controle (subdevice): status 0x%x %p\n", st, (void *)gCtl);
    if (st || !gCtl) {
        st = map_cpu(hc, hDev, hBase, 0x1000, &gCtl);
        printf("[ctl] mapa do controle (device): status 0x%x %p\n", st, (void *)gCtl);
    }
    if (st || !gCtl) goto out;
    printf("[ctl] PUT 0x%x GET 0x%x\n", gCtl[0], gCtl[1]);
    gPut = gCtl[0];

    if (!strcmp(argv[1], "off")) {
        mthd(0x00C0, 0);
        mthd(0x0080, 0);
        __sync_synchronize();
        stamp("PUT (camada base desligada)");
        gCtl[0] = gPut;
        for (int i = 0; i < 200 && gCtl[1] != gPut; i++) usleep(5000);
        printf("[ctl] PUT 0x%x GET 0x%x\n", gCtl[0], gCtl[1]);
        sleep(1);
        rc = 0;
        goto out;
    }
    if (!show) { rc = 0; printf("RESULTADO: probe ok (disp 0x%04x, base 0x%04x)\n", dcls, bcls); goto out; }

    uint32_t pitch = (w * 4 + 255) & ~255u;
    uint64_t ssz = ((uint64_t)pitch * h + 0xFFFFF) & ~0xFFFFFull;
    NVOS32_ALLOC_R387 sv;
    st = vram(hc, hDev, hSurf, ssz, &sv);
    printf("[surf] %ux%u pitch %u, %llu KiB: status 0x%x offset 0x%llx kind 0x%x\n", w, h, pitch,
           (unsigned long long)(ssz >> 10), st, (unsigned long long)sv.offset, sv.format);
    if (st) goto out;
    volatile uint32_t *px = NULL;
    st = map_cpu(hc, hDev, hSurf, sv.size, &px);
    if (st || !px) { printf("[surf] mapa BAR1 falhou 0x%x\n", st); goto out; }
    for (uint64_t i = 0; i < sv.size / 4; i++) px[i] = color;
    __sync_synchronize();
    st = ctxdma(hc, hDev, hIso, HASH_OFF | ACC_RO | KIND_PITCH, hSurf, sv.size);
    printf("[iso] ctxdma (PTE_KIND_PITCH): status 0x%x\n", st);
    if (st) {
        st = ctxdma(hc, hDev, hIso, HASH_OFF | ACC_RO, hSurf, sv.size);
        printf("[iso] ctxdma: status 0x%x\n", st);
    }
    if (st) goto out;
    st = bind(hc, hBase, hIso);
    printf("[iso] bind no canal base: status 0x%x\n", st);
    if (st) goto out;

    mthd(0x0084, 0);
    mthd(0x00C0, hIso);
    mthd(0x0400, 0);
    mthd(0x0408, w | (h << 16));
    mthd(0x040C, ((pitch >> 8) << 8) | (1u << 24));
    mthd(0x0410, 0xCFu << 8);
    mthd(0x0080, 0);
    __sync_synchronize();
    stamp("PUT (flip para a superficie nossa)");
    gCtl[0] = gPut;
    for (int i = 0; i < 200 && gCtl[1] != gPut; i++) usleep(5000);
    printf("[ctl] PUT 0x%x GET 0x%x %s\n", gCtl[0], gCtl[1], gCtl[1] == gPut ? "(consumido)" : "(PARADO)");
    printf("segurando %d s...\n", secs);
    sleep((unsigned)secs);
    stamp("liberando o canal");
    rc = 0;
    printf("RESULTADO: metodos consumidos = %s\n", gCtl[1] == gPut ? "sim" : "nao");
out:
    if (haveBase) printf("[base] free: 0x%x\n", rmfree(hc, hDisp, hBase));
    if (haveDisp) printf("[disp] free: 0x%x\n", rmfree(hc, hDev, hDisp));
    rmfree(hc, hc, hc);
    IOServiceClose(gConn);
    return rc;
}
