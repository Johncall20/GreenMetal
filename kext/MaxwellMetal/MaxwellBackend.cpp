/*
 * MaxwellBackend - backend de GPU no kernel para o futuro acelerador (porte do NVAccel), sobre a API NvRm*
 * exportada pelo NVDAResmanWeb (o mesmo caminho que o GeForceWeb usa: cliente do kernel).
 *
 * Etapa B1: VRAM com mapa de kernel, VA da GPU num espaco proprio, e a tela pelo canal base EVO (0x927C) do
 * head 0. Nada roda no boot: o RM so e aberto quando o cliente de teste ('MXBK', so root) abre, e tudo e
 * desfeito quando ele fecha (a camada base e desligada antes de soltar qualquer superficie).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include "MaxwellBackend.h"
#include "../../accel/nvrm_vram_abi.h"
#include "../../accel/nvrm_gpuva_abi.h"
#define NVRM_SS_KERNEL_ABI 1
#include "../../accel/nvrm_surfshare_abi.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

extern "C" {
#include "../../nvrm_r387.h"
}

#define BLOG(fmt, ...) IOLog("MaxwellMetal/backend: " fmt "\n", ##__VA_ARGS__)

#define CL_DEVICE      0x0080u
#define CL_SUBDEVICE   0x2080u
#define CL_VIRT_R387   0x0070u
#define CL_CTXDMA      0x0002u
#define CL_DISP_GM200  0x9570u
#define CL_BASE_GK110  0x927Cu

#define MEM_SYS_WC     0x00002010u
#define CTX_HASH_OFF   (1u << 29)
#define CTX_RO         1u
#define CTX_KIND_PITCH (2u << 16)
#define OS46_FIXED     (1u << 15)

#define VA_BASE        0x0000000200000000ull
#define VA_END         0x0000008000000000ull
#define VA_ALIGN       0x20000ull

#define MAXVRAM        32
#define PB_SIZE        0x1000u

struct MMVram {
    bool     used;
    NvHandle hMem, hIso;
    NvU64    vramOffset, size, gpuva;
    volatile uint32_t *kva;
};

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
} EvoChannelDmaAlloc;

static struct {
    IOLock  *lock;
    bool     ready;
    NvHandle hClient, hDevice, hSub;
    NvU32    nextHandle;
    bool     vaReady;
    NvHandle hVirtAll, hCtxAll;
    NvU64    vaNext;
    bool     dispReady;
    NvHandle hDisp, hPbMem, hPbDma, hBase;
    volatile uint32_t *pb, *ctl;
    uint32_t put;
    int      scanout;
    MMVram   v[MAXVRAM];
    bool     abi;
} g;

static NvHandle newHandle(void) { return 0xBEE10000u + (++g.nextHandle); }

static NvU32 vidmem(NvHandle h, NvU64 size, NvU32 type, NVOS32_ALLOC_R387 *out)
{
    NVOS32_ALLOC_R387 v;
    bzero(&v, sizeof(v));
    v.hRoot = g.hClient; v.hObjectParent = g.hDevice; v.function = NV387_NVOS32_FUNCTION_ALLOC_SIZE;
    v.owner = 0x6d6d626bu; v.hMemory = h; v.type = type; v.flags = 0x4000u | 0x0001u;
    v.attr = 0x4u | (2u << 8) | (1u << 23) | (2u << 27) | (2u << 29);
    v.size = (size + 0xFFFull) & ~0xFFFull; v.alignment = 0x1000;
    v.status = 0xFFFFFFFFu;
    NvU32 st = NvRmVidHeapControl((NVOS32_PARAMETERS_R387 *)&v);
    if (!st) st = v.status;
    if (!st && v.hMemory != h) { NvRmFree(g.hClient, g.hDevice, v.hMemory); st = 0x33; }
    if (!st && out) *out = v;
    return st;
}

static NvU32 openRm(void)
{
    if (g.ready) return 0;
    NvHandle c = 0;
    NvU32 st = NvRmAllocRoot(&c);
    if (st || !c) { BLOG("NvRmAllocRoot: 0x%x", st); return st ? st : 0x1; }
    g.hClient = c; g.hDevice = newHandle(); g.hSub = newHandle();
    NV0080_ALLOC_PARAMETERS_R387 dp;
    bzero(&dp, sizeof(dp));
    dp.hClientShare = c;
    st = NvRmAlloc(c, c, g.hDevice, CL_DEVICE, &dp);
    if (!st) { NvU32 sp = 0; st = NvRmAlloc(c, g.hDevice, g.hSub, CL_SUBDEVICE, &sp); }
    if (st) { BLOG("device/subdevice: 0x%x", st); NvRmFree(c, c, c); g.hClient = 0; return st; }
    g.ready = true;
    BLOG("RM do kernel aberto: cliente 0x%x device 0x%x subdevice 0x%x", c, g.hDevice, g.hSub);
    return 0;
}

static NvU32 openVa(void)
{
    if (g.vaReady) return 0;
    NvHandle hv = newHandle(), hc = newHandle();
    void *addr = NULL;
    NvU64 limit = VA_END - 1;
    NvU32 st = NvRmAllocMemory64(g.hClient, g.hDevice, hv, CL_VIRT_R387, MEM_SYS_WC, &addr, &limit);
    if (st) { BLOG("VA: virtual 0x70 -> 0x%x", st); return st; }
    st = NvRmAllocContextDma2(g.hClient, hc, CL_CTXDMA, CTX_HASH_OFF, hv, 0, limit);
    if (st) { BLOG("VA: ctxdma -> 0x%x", st); NvRmFree(g.hClient, g.hDevice, hv); return st; }
    g.hVirtAll = hv; g.hCtxAll = hc; g.vaNext = VA_BASE; g.vaReady = true;
    BLOG("VA: virtual 0x%x ctxdma 0x%x limite 0x%llx", hv, hc, limit);
    return 0;
}

static bool pbKick(void)
{
    __sync_synchronize();
    g.ctl[0] = g.put;
    for (int i = 0; i < 200 && g.ctl[1] != g.put; i++) IOSleep(1);
    return g.ctl[1] == g.put;
}

static void mthd(uint32_t m, uint32_t data)
{
    if (g.put + 8 > PB_SIZE - 8) {
        g.pb[g.put / 4] = 0x20000000u;
        g.put = 0;
    }
    g.pb[g.put / 4] = (1u << 18) | m;
    g.pb[g.put / 4 + 1] = data;
    g.put += 8;
}

/* ---- flip do canal base, pela referencia aberta (NVKMS 515.43.04, nvkms-evo2.c FlipBase90, classe 917C/927C) ----
 * O flip curto (0x84=0, ISO, imagem, UPDATE) passou a dar excecao INVALID_STATE no UPDATE (codigo 0x0d) depois da troca:
 * os metodos ficam na montagem e nunca armam. debug.mxflip escolhe o que entra na sequencia (bits):
 *   0x01 MIN_PRESENT_INTERVAL = 1 (sem tearing)      0x02 zera notifier, semaforo e timestamps
 *   0x04 CSC com OWNER_BASE e matriz identidade      0x08 BASE_LUT/OUTPUT_LUT = USE_CORE_LUT
 *   0x10 BEGIN_MODE IMMEDIATE (intervalo 0)          0x20 CSC com OWNER_CORE
 * Depois do flip o estado e lido pelo RM (EXEC_REG_OPS, so leitura): debug.mxflip_iso = ISO em uso (0 = nao armou),
 * debug.mxflip_exc / debug.mxflip_code = ultima excecao do canal base do head 0. */
int gMxFlip = 0x4f, gMxFlipIso = 0, gMxFlipExc = 0, gMxFlipCode = 0;
/* 0.7.1: o core do head 0 (NVDA) esta em STORAGE 0x00007804 = BLOCKLINEAR, blocos de 16 GOBs, pitch 0x78 blocos; a nossa
 * base ia em 0x01001e00 = PITCH. No EVO2 a superficie do core e a da base andam juntas (NVKMS needToReprogramCoreSurface);
 * layouts diferentes = INVALID_STATE no UPDATE. debug.mxflip_storage != 0 manda esse valor em SURFACE_SET_STORAGE;
 * bit 0x40 de debug.mxflip copia o STORAGE em uso no core; bit 0x80 cria o ctxdma ISO sem PTE_KIND_PITCH. */
int gMxFlipStorage = 0, gMxFlipCoreStorage = 0;
/* 0.7.3: flip por quadro (superficie do WindowServer com a linha larga). Os tres primeiros flips de superficie e os da
 * home seguem com o diagnostico completo (espera de 60 ms e leitura dos registradores); os demais so empurram os metodos
 * e esperam o canal consumir. debug.mxflip_count = flips de superficie; debug.mxflip_stuck = flips que o canal nao
 * consumiu em 200 ms (o flip volta erro e o acelerador copia). */
int gMxFlipCount = 0, gMxFlipStuck = 0;
/* 0.7.4: VRAM das superficies mapeada na BAR1 agora, em KB (orcamento BAR1_BUDGET), e pedidos recusados por falta dela */
int gMxVramMappedKB = 0, gMxVramRefused = 0;
/* 0.7.5: VRAM de superficies sem mapa na BAR1 (linha larga), em KB, e mapas temporarios feitos para leitura pela CPU */
int gMxVramUnmappedKB = 0, gMxVramTempMaps = 0;

typedef struct { NvU32 hClientTarget, hChannelTarget, reserved00[3], regOpCount; NvU64 regOps; } MMRegOpsP;
typedef struct { uint8_t regOp, regType, regStatus, regQuad; NvU32 regGroupMask, regSubGroupMask, regOffset,
                 regValueHi, regValueLo, regAndNMaskHi, regAndNMaskLo; } MMRegOp;
static NvU32 dispRd32(NvU32 off)
{
    MMRegOp op; bzero(&op, sizeof(op));
    op.regStatus = 0xff; op.regOffset = off;
    MMRegOpsP p; bzero(&p, sizeof(p));
    p.regOpCount = 1; p.regOps = (NvU64)(uintptr_t)&op;
    NvU32 st = NvRmControl(g.hClient, g.hSub, 0x20800122, &p, sizeof(p));
    return (st || op.regStatus) ? 0xDEAD0000u | (st & 0xFFFF) : op.regValueLo;
}

static bool pushFlip(NvHandle hIso, uint32_t w, uint32_t h, uint32_t pitch, const char *what, bool quick = false)
{
    const int v = gMxFlip;
    if (v & 0x02) {
        mthd(0x00A4, 0); mthd(0x00A0, 0);                                   /* notifier */
        mthd(0x0094, 0); mthd(0x0088, 0); mthd(0x008C, 0); mthd(0x0090, 0); /* semaforo */
        mthd(0x0130, 0); mthd(0x0134, 0); mthd(0x0138, 0); mthd(0x013C, 0); /* timestamps */
    }
    uint32_t pc = 0;
    if (v & 0x10) pc = 1u << 8;            /* IMMEDIATE, intervalo 0 */
    else if (v & 0x01) pc = 1u << 4;       /* NON_TEARING, MIN_PRESENT_INTERVAL 1 */
    mthd(0x0084, pc);
    if (v & 0x04) {
        for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 4; x++) {
            uint32_t c = (x == y) ? 0x10000u : 0;
            if (!x && !y) c |= 1u << 31;   /* OWNER_BASE */
            mthd(0x0140 + 4 * (y * 4 + x), c);
        }
    } else if (v & 0x20) mthd(0x0140, 0);  /* OWNER_CORE */
    if (v & 0x08) { mthd(0x00E0, 1u << 30); mthd(0x00E8, 1u << 30); }
    mthd(0x0400, 0);
    mthd(0x00C0, hIso);
    mthd(0x0408, w | (h << 16));
    uint32_t storage = ((pitch >> 8) << 8) | (1u << 24);
    if (!quick || !gMxFlipCoreStorage || ((uint32_t)gMxFlipCoreStorage >> 16) == 0xDEAD) gMxFlipCoreStorage = (int)dispRd32(0x64046c);
    if (gMxFlipStorage) storage = (uint32_t)gMxFlipStorage;
    else if ((v & 0x40) && ((uint32_t)gMxFlipCoreStorage >> 16) != 0xDEAD && gMxFlipCoreStorage) storage = (uint32_t)gMxFlipCoreStorage;
    mthd(0x040C, storage);
    mthd(0x0410, 0xCFu << 8);
    mthd(0x0080, 0);
    const bool taken = pbKick();
    if (quick) {
        if (!taken) {
            if (gMxFlipStuck++ < 8) BLOG("flip %s: o canal base nao consumiu em 200 ms (PUT 0x%x GET 0x%x, excecao 0x%x codigo 0x%x)",
                                         what, g.ctl[0], g.ctl[1], dispRd32(0x6101fc), dispRd32(0x610204));
            return false;
        }
        if ((gMxFlipCount & 0x3FF) == 0) {
            gMxFlipIso = (int)dispRd32(0x6410c0); gMxFlipExc = (int)dispRd32(0x6101fc); gMxFlipCode = (int)dispRd32(0x610204);
            BLOG("flip %s #%d (rapido): iso 0x%x em uso 0x%x storage 0x%x excecao 0x%x codigo 0x%x", what, gMxFlipCount, hIso,
                 gMxFlipIso, storage, gMxFlipExc, gMxFlipCode);
        }
        return true;
    }
    IOSleep(60);   /* o UPDATE arma no vblank */
    gMxFlipIso = (int)dispRd32(0x6410c0); gMxFlipExc = (int)dispRd32(0x6101fc); gMxFlipCode = (int)dispRd32(0x610204);
    BLOG("flip %s (mxflip 0x%x, iso 0x%x, storage 0x%x, core 0x%x): PUT 0x%x GET 0x%x | ISO em uso 0x%x tamanho em uso 0x%x | excecao 0x%x codigo 0x%x -> %s",
         what, v, hIso, storage, gMxFlipCoreStorage, g.ctl[0], g.ctl[1], gMxFlipIso, dispRd32(0x641408), gMxFlipExc, gMxFlipCode,
         ((NvU32)gMxFlipIso == hIso && dispRd32(0x64140c) == storage) ? "ARMOU" : "nao armou (ISO ou STORAGE em uso diferentes)");
    return true;
}

static NvU32 openDisplay(void)
{
    if (g.dispReady) return 0;
    NvU32 ap[4] = { 0, 0, 0, 0 };
    NvHandle hd = newHandle(), hpm = newHandle(), hpd = newHandle(), hb = newHandle();
    NvU32 st = NvRmAlloc(g.hClient, g.hDevice, hd, CL_DISP_GM200, ap);
    if (st) { BLOG("display 0x9570: 0x%x", st); return st; }
    NVOS32_ALLOC_R387 pv;
    st = vidmem(hpm, PB_SIZE, 0, &pv);
    void *pbk = NULL;
    if (!st) st = NvRmMapMemory(g.hClient, g.hDevice, hpm, 0, PB_SIZE, &pbk, 0);
    if (!st && !pbk) st = 0x2;
    if (!st) st = NvRmAllocContextDma2(g.hClient, hpd, CL_CTXDMA, CTX_HASH_OFF | CTX_RO, hpm, 0, PB_SIZE - 1);
    if (st) { BLOG("pushbuffer do display: 0x%x", st); NvRmFree(g.hClient, g.hDevice, hd); return st; }
    g.pb = (volatile uint32_t *)pbk;
    for (unsigned i = 0; i < PB_SIZE / 4; i++) g.pb[i] = 0;
    EvoChannelDmaAlloc ca;
    bzero(&ca, sizeof(ca));
    ca.channelInstance = 0; ca.hObjectBuffer = hpd; ca.subDeviceId = 1;
    st = NvRmAlloc(g.hClient, hd, hb, CL_BASE_GK110, &ca);
    if (st) { BLOG("canal base 0x927C head 0: 0x%x", st); NvRmFree(g.hClient, g.hDevice, hd); return st; }
    void *ctl = NULL;
    st = NvRmMapMemory(g.hClient, g.hSub, hb, 0, 0x1000, &ctl, 0);
    if (st || !ctl) {
        BLOG("controle do canal base: 0x%x", st);
        NvRmFree(g.hClient, hd, hb); NvRmFree(g.hClient, g.hDevice, hd);
        return st ? st : 0x2;
    }
    g.hDisp = hd; g.hPbMem = hpm; g.hPbDma = hpd; g.hBase = hb;
    g.ctl = (volatile uint32_t *)ctl;
    g.put = g.ctl[0];
    g.scanout = -1;
    g.dispReady = true;
    BLOG("canal base aberto: display 0x%x canal 0x%x PUT 0x%x GET 0x%x", hd, hb, g.ctl[0], g.ctl[1]);
    return 0;
}

static void displayOff(void)
{
    if (!g.dispReady) return;
    mthd(0x00C0, 0);
    mthd(0x0080, 0);
    pbKick();
    g.scanout = -1;
}

static void freeVram(int id)
{
    MMVram *m = &g.v[id];
    if (!m->used) return;
    if (g.scanout == id) displayOff();
    if (m->gpuva) NvRmUnmapMemoryDma(g.hClient, g.hDevice, g.hCtxAll, m->hMem, 0, m->gpuva);
    if (m->hIso) NvRmFree(g.hClient, g.hClient, m->hIso);
    if (m->kva) NvRmUnmapMemory(g.hClient, g.hDevice, m->hMem, (void *)m->kva, 0);
    NvRmFree(g.hClient, g.hDevice, m->hMem);
    bzero(m, sizeof(*m));
}

static void abiTeardownLocked(void);

void mmBackendInit(void)
{
    if (!g.lock) g.lock = IOLockAlloc();
}

static void teardownLocked(void)
{
    if (!g.ready) return;
    if (g.dispReady) displayOff();
    for (int i = 0; i < MAXVRAM; i++) freeVram(i);
    abiTeardownLocked();
    NvRmFree(g.hClient, g.hClient, g.hClient);
    IOLock *l = g.lock;
    bzero(&g, sizeof(g));
    g.lock = l;
    BLOG("RM do kernel fechado");
}

void mmBackendShutdown(void)
{
    if (!g.lock) return;
    IOLockLock(g.lock);
    teardownLocked();
    IOLockUnlock(g.lock);
    IOLockFree(g.lock);
    g.lock = NULL;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Cliente de teste                                                                                        */
/* ------------------------------------------------------------------------------------------------------ */
class IOAccelMaxwellBackendTest : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelMaxwellBackendTest)
public:
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientClose(void) override;
    IOReturn clientDied(void) override;
};
OSDefineMetaClassAndStructors(IOAccelMaxwellBackendTest, IOUserClient)

static OSObject *gOwner;
static bool gSelfTest;
static uint64_t abiSelfTestStep(uint64_t step, uint64_t color, uint64_t *out);

IOUserClient *mmBackendNewTestClient(IOService *provider, task_t task, void *securityID, UInt32 type)
{
    if (IOUserClient::clientHasPrivilege(task, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) return NULL;
    if (!g.lock) return NULL;
    IOAccelMaxwellBackendTest *uc = new IOAccelMaxwellBackendTest;
    if (!uc) return NULL;
    if (!uc->initWithTask(task, securityID, type) || !uc->attach(provider) || !uc->start(provider)) {
        uc->detach(provider);
        uc->release();
        return NULL;
    }
    return uc;
}

IOReturn IOAccelMaxwellBackendTest::externalMethod(uint32_t sel, IOExternalMethodArguments *a,
                                                   IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!a || a->scalarOutputCount < 4 || a->scalarInputCount > 8) return kIOReturnBadArgument;
    const uint64_t *in = a->scalarInput;
    uint64_t *out = a->scalarOutput;
    for (unsigned i = 0; i < a->scalarOutputCount; i++) out[i] = 0;
    IOLockLock(g.lock);
    if ((g.abi && sel != 7) || (gOwner && gOwner != this)) { IOLockUnlock(g.lock); return kIOReturnExclusiveAccess; }
    gOwner = this;
    NvU32 st = openRm();
    IOReturn ret = kIOReturnSuccess;
    if (st) { out[0] = st; ret = kIOReturnError; goto done; }
    switch (sel) {
    case 0:
        out[0] = g.hClient; out[1] = g.hDevice; out[2] = g.hSub; out[3] = g.dispReady;
        break;
    case 1: {
        if (a->scalarInputCount < 3) { ret = kIOReturnBadArgument; break; }
        uint64_t size = in[0];
        if (!size || size > (64ull << 20)) { ret = kIOReturnBadArgument; break; }
        int id = -1;
        for (int i = 0; i < MAXVRAM; i++) if (!g.v[i].used) { id = i; break; }
        if (id < 0) { ret = kIOReturnNoResources; break; }
        MMVram *m = &g.v[id];
        NVOS32_ALLOC_R387 vo;
        NvHandle h = newHandle();
        st = vidmem(h, size, (uint32_t)in[2], &vo);
        if (st) { out[0] = st; ret = kIOReturnNoMemory; break; }
        void *kva = NULL;
        st = NvRmMapMemory(g.hClient, g.hDevice, h, 0, vo.size, &kva, 0);
        if (st || !kva) { NvRmFree(g.hClient, g.hDevice, h); out[0] = st ? st : 2; ret = kIOReturnNoMemory; break; }
        m->used = true; m->hMem = h; m->vramOffset = vo.offset; m->size = vo.size;
        m->kva = (volatile uint32_t *)kva;
        uint32_t color = (uint32_t)in[1];
        for (uint64_t i = 0; i < vo.size / 4; i++) m->kva[i] = color;
        __sync_synchronize();
        out[0] = 0; out[1] = (uint64_t)id; out[2] = vo.offset; out[3] = vo.size;
        BLOG("VRAM %d: h 0x%x offset 0x%llx tamanho 0x%llx, pintada pelo mapa de kernel", id, h, vo.offset, vo.size);
        break;
    }
    case 2: {
        int id = a->scalarInputCount ? (int)in[0] : -1;
        if (id < 0 || id >= MAXVRAM || !g.v[id].used) { ret = kIOReturnBadArgument; break; }
        MMVram *m = &g.v[id];
        if (!m->gpuva) {
            st = openVa();
            if (st) { out[0] = st; ret = kIOReturnError; break; }
            NvU64 va = (g.vaNext + VA_ALIGN - 1) & ~(VA_ALIGN - 1);
            if (va + m->size > VA_END) { ret = kIOReturnNoSpace; break; }
            NvU64 dma = va;
            st = NvRmMapMemoryDma(g.hClient, g.hDevice, g.hCtxAll, m->hMem, 0, m->size, OS46_FIXED, &dma);
            if (st) { out[0] = st; ret = kIOReturnError; BLOG("MAP_MEMORY_DMA: 0x%x", st); break; }
            m->gpuva = dma;
            g.vaNext = va + m->size;
            BLOG("VRAM %d no VA da GPU 0x%llx", id, dma);
        }
        out[0] = 0; out[1] = m->gpuva;
        break;
    }
    case 3: {
        if (a->scalarInputCount < 4) { ret = kIOReturnBadArgument; break; }
        int id = (int)in[0];
        uint32_t w = (uint32_t)in[1], h = (uint32_t)in[2], pitch = (uint32_t)in[3];
        if (id < 0 || id >= MAXVRAM || !g.v[id].used || !w || !h || w > 8192 || h > 8192 || (pitch & 0xFF) ||
            (uint64_t)pitch * h > g.v[id].size) { ret = kIOReturnBadArgument; break; }
        st = openDisplay();
        if (st) { out[0] = st; ret = kIOReturnError; break; }
        MMVram *m = &g.v[id];
        if (!m->hIso) {
            NvHandle hi = newHandle();
            st = NvRmAllocContextDma2(g.hClient, hi, CL_CTXDMA, CTX_HASH_OFF | CTX_RO | ((gMxFlip & 0x80) ? 0 : CTX_KIND_PITCH), m->hMem, 0,
                                      m->size - 1);
            if (!st) st = NvRmBindContextDma(g.hClient, g.hBase, hi);
            if (st) { NvRmFree(g.hClient, g.hClient, hi); out[0] = st; ret = kIOReturnError; BLOG("ISO: 0x%x", st); break; }
            m->hIso = hi;
        }
        pushFlip(m->hIso, w, h, pitch, "teste");
        g.scanout = id;
        out[0] = 0; out[1] = g.ctl[0]; out[2] = g.ctl[1]; out[3] = g.ctl[0] == g.ctl[1];
        BLOG("flip para a VRAM %d (%ux%u pitch %u): PUT 0x%x GET 0x%x", id, w, h, pitch, g.ctl[0], g.ctl[1]);
        break;
    }
    case 4:
        displayOff();
        out[0] = 0; out[1] = g.dispReady ? g.ctl[0] : 0; out[2] = g.dispReady ? g.ctl[1] : 0;
        break;
    case 5: {
        int id = a->scalarInputCount ? (int)in[0] : -1;
        if (id < 0 || id >= MAXVRAM || !g.v[id].used) { ret = kIOReturnBadArgument; break; }
        freeVram(id);
        break;
    }
    case 6: {
        if (a->scalarInputCount < 2) { ret = kIOReturnBadArgument; break; }
        int id = (int)in[0];
        uint64_t idx = in[1];
        if (id < 0 || id >= MAXVRAM || !g.v[id].used || idx >= g.v[id].size / 4) { ret = kIOReturnBadArgument; break; }
        out[0] = 0; out[1] = g.v[id].kva[idx];
        break;
    }
    case 7:
        IOLockUnlock(g.lock);
        gSelfTest = true;
        out[0] = abiSelfTestStep(a->scalarInputCount ? in[0] : 99, a->scalarInputCount > 1 ? in[1] : 0, out);
        gSelfTest = false;
        return kIOReturnSuccess;
    default:
        ret = kIOReturnUnsupported;
    }
done:
    IOLockUnlock(g.lock);
    return ret;
}

IOReturn IOAccelMaxwellBackendTest::clientClose(void)
{
    IOLockLock(g.lock);
    if (gOwner == this) {
        /* com a ABI em uso (MaxwellFB/acelerador) a home e a memoria do framebuffer: o teste so libera o que e dele.
         * WAS: teardownLocked() sempre -- liberava o cliente RM e a home, e o MaxwellFB ficava apontando para VRAM solta. */
        if (g.abi) { for (int i = 0; i < MAXVRAM; i++) freeVram(i); }
        else teardownLocked();
        gOwner = NULL;
    }
    IOLockUnlock(g.lock);
    terminate();
    return kIOReturnSuccess;
}

IOReturn IOAccelMaxwellBackendTest::clientDied(void)
{
    return clientClose();
}

/* ------------------------------------------------------------------------------------------------------ */
/* ABI do acelerador portado (nvrm_vram_abi.h / nvrm_gpuva_abi.h): MaxwellAccel -> MaxwellMetal            */
/* ------------------------------------------------------------------------------------------------------ */
#define SURF_MAGIC 0x4D585346u
#define BAR1_BUDGET (160ull << 20)

struct MMSurf {
    MMSurf  *next;
    uint32_t magic;
    NvHandle hMem, hIso;
    NvU64    vramOffset, size, phys, cookie;
    void    *kva;
    bool     nomap;      /* sem mapa permanente na BAR1 (kva NULL; phys = o da reserva comum) */
    void    *tmpKva;     /* mapa temporario (nvMapVram) */
};
/* 0.7.6: 10 superficies de 1920x1080 com linha larga (34 MB cada). Eram 640 MB: com video o WindowServer criou ~30
 * superficies de tela de uma vez, a VRAM encheu e ele abortou (2026-11-12). Passou do teto: recusa, e a superficie fica
 * com imagem privada e copia. */
#define NOMAP_BUDGET (352ull << 20)
struct MMVirt { NvU32 hVirt; NvU64 va, size; };
#define MAXVIRT 4096

static struct {
    MMSurf *list;
    MMSurf *home;
    MMSurf *front;
    MMSurf *dummy;       /* reserva mapeada cujo endereco fisico representa as superficies sem mapa */
    NvU64   mapped, unmapped, nextCookie;
    uint32_t modeW, modeH, modePitch;
    IORangeAllocator *va;
    MMVirt  virt[MAXVIRT];
    NvU32   nextVirt;
} a;

static MMSurf *surfFind(void *h, NvU64 cookie)
{
    for (MMSurf *s = a.list; s; s = s->next)
        if (s == h && s->magic == SURF_MAGIC && (!cookie || s->cookie == cookie)) return s;
    return NULL;
}

static NvU64 physOf(void *kva, NvU64 size)
{
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddress(kva, (IOByteCount)size, kIODirectionInOut);
    if (!md) { BLOG("phys: withAddress falhou"); return 0; }
    IOByteCount len = 0;
    addr64_t base = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    NvU64 off = 0;
    while (base && off < size) {
        IOByteCount l = 0;
        addr64_t ph = md->getPhysicalSegment((IOByteCount)off, &l, kIOMemoryMapperNone);
        if (!ph || !l || ph != base + off) break;
        off += l;
    }
    md->release();
    static int logged;
    if (logged < 12 || off < size) { logged++; BLOG("phys: 0x%llx, contiguo por 0x%llx de 0x%llx", (unsigned long long)base,
                                                   (unsigned long long)off, (unsigned long long)size); }
    return (base && off >= size) ? (NvU64)base : 0;
}

static MMSurf *surfAlloc(NvU64 size, bool nomap = false)
{
    /* +1 MiB: uma varredura block-linear de blocos de 64 linhas le ate a linha 1088 de uma tela de 1080 */
    size = ((size + 0xFFFFull) & ~0xFFFFull) + 0x100000ull;
    if (nomap) {
        /* a reserva comum e do tamanho do primeiro pedido; pedido maior que ela nao pode usar o endereco dela */
        if (!a.dummy) { a.dummy = surfAlloc(size - 0x100000ull); if (a.dummy) BLOG("reserva comum das superficies sem mapa: %llu B", a.dummy->size); }
        if (!a.dummy || size > a.dummy->size || a.unmapped + size > NOMAP_BUDGET) { gMxVramRefused++; return NULL; }
        MMSurf *s = (MMSurf *)IOMalloc(sizeof(MMSurf));
        if (!s) return NULL;
        bzero(s, sizeof(*s));
        NVOS32_ALLOC_R387 vo;
        NvHandle h = newHandle();
        NvU32 st = vidmem(h, size, 0, &vo);
        if (st) { BLOG("nvAllocVram(%llu, sem mapa): status 0x%x -- recusado", size, st); IOFree(s, sizeof(*s)); return NULL; }
        s->magic = SURF_MAGIC; s->hMem = h; s->vramOffset = vo.offset; s->size = vo.size; s->nomap = true;
        s->phys = a.dummy->phys;
        s->cookie = ++a.nextCookie;
        s->next = a.list; a.list = s;
        a.unmapped += vo.size; gMxVramUnmappedKB = (int)(a.unmapped >> 10);
        return s;
    }
    if (!size || a.mapped + size > BAR1_BUDGET) { if (size) gMxVramRefused++; return NULL; }
    MMSurf *s = (MMSurf *)IOMalloc(sizeof(MMSurf));
    if (!s) return NULL;
    bzero(s, sizeof(*s));
    NVOS32_ALLOC_R387 vo;
    NvHandle h = newHandle();
    NvU32 st = vidmem(h, size, 0, &vo);
    void *kva = NULL;
    if (!st) st = NvRmMapMemory(g.hClient, g.hDevice, h, 0, vo.size, &kva, 0);
    NvU64 ph = (!st && kva) ? physOf(kva, vo.size) : 0;
    if (st || !kva || !ph) {
        BLOG("nvAllocVram(%llu): status 0x%x kva %p phys 0x%llx -- recusado", size, st, kva, ph);
        if (kva) NvRmUnmapMemory(g.hClient, g.hDevice, h, kva, 0);
        if (!st || kva) NvRmFree(g.hClient, g.hDevice, h);
        IOFree(s, sizeof(*s));
        return NULL;
    }
    s->magic = SURF_MAGIC; s->hMem = h; s->vramOffset = vo.offset; s->size = vo.size; s->kva = kva; s->phys = ph;
    s->cookie = ++a.nextCookie;
    s->next = a.list; a.list = s;
    a.mapped += vo.size; gMxVramMappedKB = (int)(a.mapped >> 10);
    return s;
}

static void surfFree(MMSurf *s)
{
    if (a.front == s && a.home && a.home != s) {
        /* a superficie na tela vai embora: volta para a home antes */
        pushFlip(a.home->hIso, a.modeW, a.modeH, a.modePitch, "home (volta)");
        a.front = a.home;
    } else if (a.front == s) { displayOff(); a.front = NULL; }
    for (MMSurf **pp = &a.list; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    if (s->hIso) NvRmFree(g.hClient, g.hClient, s->hIso);
    if (s->tmpKva) NvRmUnmapMemory(g.hClient, g.hDevice, s->hMem, s->tmpKva, 0);
    if (s->kva) NvRmUnmapMemory(g.hClient, g.hDevice, s->hMem, s->kva, 0);
    NvRmFree(g.hClient, g.hDevice, s->hMem);
    if (s->nomap) { a.unmapped -= s->size; gMxVramUnmappedKB = (int)(a.unmapped >> 10); }
    else { a.mapped -= s->size; gMxVramMappedKB = (int)(a.mapped >> 10); }
    s->magic = 0;
    IOFree(s, sizeof(*s));
}

static NvU32 flipTo(MMSurf *s, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (!w || !h || w > 8192 || h > 8192 || (pitch & 0xFF) || (NvU64)w * 4 > pitch || (NvU64)pitch * h > s->size)
        return 0x1F;
    NvU32 st = openDisplay();
    if (st) return st;
    if (!s->hIso) {
        NvHandle hi = newHandle();
        st = NvRmAllocContextDma2(g.hClient, hi, CL_CTXDMA, CTX_HASH_OFF | CTX_RO | ((gMxFlip & 0x80) ? 0 : CTX_KIND_PITCH), s->hMem, 0, s->size - 1);
        if (!st) st = NvRmBindContextDma(g.hClient, g.hBase, hi);
        if (st) { NvRmFree(g.hClient, g.hClient, hi); return st; }
        s->hIso = hi;
    }
    const bool quick = gMxFlipCount >= 3;   /* 0.7.5: a home tambem (a volta para a home antes de um quadro copiado custava 60 ms) */
    const bool ok = pushFlip(s->hIso, w, h, pitch, s == a.home ? "home" : "superficie", quick);
    if (s != a.home) gMxFlipCount++;
    if (!ok) return 0x65;
    a.front = s;
    return 0;
}

static void modeOf(void *fbp)
{
    if (a.modeW) return;
    uint32_t w = 1920, h = 1080;
    IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, (OSObject *)fbp);
    if (fb) {
        IODisplayModeID mode = 0; IOIndex depth = 0;
        IOPixelInformation pi;
        bzero(&pi, sizeof(pi));
        if (fb->getCurrentDisplayMode(&mode, &depth) == kIOReturnSuccess &&
            fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pi) == kIOReturnSuccess &&
            pi.activeWidth >= 640 && pi.activeHeight >= 480) { w = pi.activeWidth; h = pi.activeHeight; }
        else { BLOG("modo do framebuffer %s: %ux%u, ignorado", fb->getName(), (unsigned)pi.activeWidth, (unsigned)pi.activeHeight); return; }
    }
    a.modeW = w; a.modeH = h; a.modePitch = (w * 4 + 255) & ~255u;
    BLOG("modo da tela: %ux%u pitch %u%s", w, h, a.modePitch, fb ? "" : " (padrao: sem framebuffer)");
}

static void modeDefault(void)
{
    if (a.modeW) return;
    a.modeW = 1920; a.modeH = 1080; a.modePitch = 7680;
    BLOG("modo da tela: 1920x1080 (padrao: o framebuffer ainda nao respondeu)");
}

static NvU32 ensureHome(void)
{
    if (a.home) return 0;
    modeDefault();
    /* 0.7.2: o storage da base tem que ser IGUAL ao do core (senao INVALID_STATE 0x12), e com o do NVDA (0x7804) o head
     * le a nossa superficie linear com (campo PITCH) * 256 B por linha = 30720, 4x os 7680 de 1920 px (foto de
     * 2026-11-11: imagem inteira no quarto de cima). Em vez de brigar com o core, a home ganha esse stride real: uma
     * linha da tela = uma linha da imagem. O MaxwellFB informa esse bytesPerRow ao WindowServer. */
    MMSurf *s = NULL;
    {
        const uint32_t cs = dispRd32(0x64046c);
        gMxFlipCoreStorage = (int)cs;
        if ((cs >> 16) != 0xDEAD && cs && !(cs & (1u << 24))) {
            const uint32_t stride = ((cs >> 8) & 0x1FFFu) * 256u;
            if (stride > a.modePitch && stride <= a.modePitch * 8u) {
                s = surfAlloc((NvU64)stride * a.modeH);
                if (s) { BLOG("home com stride %u B (core STORAGE 0x%x; era %u)", stride, cs, a.modePitch); a.modePitch = stride; }
                else BLOG("home com stride %u B nao coube: fica em %u (a imagem sai espremida)", stride, a.modePitch);
            }
        } else BLOG("core STORAGE 0x%x: home com o pitch normal %u", cs, a.modePitch);
    }
    if (!s) s = surfAlloc((NvU64)a.modePitch * a.modeH);
    if (!s) return 0x51;
    for (NvU64 i = 0; i < s->size / 4; i++) ((volatile uint32_t *)s->kva)[i] = 0xFF000000u;
    __sync_synchronize();
    a.home = s;
    NvU32 st = flipTo(s, a.modeW, a.modeH, a.modePitch);
    BLOG("superficie home %ux%u em VRAM offset 0x%llx, na tela: 0x%x", a.modeW, a.modeH, s->vramOffset, st);
    return 0;
}

static void abiTeardownLocked(void)
{
    if (!g.abi) return;
    if (g.dispReady) displayOff();
    a.front = NULL;
    while (a.list) { MMSurf *s = a.list; a.list = s->next; s->magic = 0; IOFree(s, sizeof(*s)); }
    if (a.va) { a.va->release(); a.va = NULL; }
    bzero(&a, sizeof(a));
    g.abi = false;
}

static NvU32 vaReady(void)
{
    NvU32 st = openVa();
    if (st) return st;
    if (!a.va) {
        a.va = IORangeAllocator::withRange(0, VA_ALIGN, 0, IORangeAllocator::kLocking);
        if (!a.va) return 0x51;
        a.va->deallocate(VA_BASE + (64ull << 30), VA_END - VA_BASE - (64ull << 30));
    }
    return 0;
}

static MMVirt *virtFind(NvU32 h)
{
    if (!h) return NULL;
    for (int i = 0; i < MAXVIRT; i++) if (a.virt[i].hVirt == h) return &a.virt[i];
    return NULL;
}

IOReturn mmBackendPlatformFunction(const OSSymbol *fn, void *p1, void *p2)
{
    if (!fn || !g.lock) return kIOReturnUnsupported;
    bool vAlloc = fn->isEqualTo(NVRM_VRAM_FN_ALLOC), vFree = fn->isEqualTo(NVRM_VRAM_FN_FREE),
         scan = fn->isEqualTo(NVRM_VRAM_FN_SCANOUT), hand = fn->isEqualTo(NVRM_GPUVA_FN_HANDLES),
         flip = fn->isEqualTo(NVRM_FLIP_FN) || fn->isEqualTo("nvFlipToSurfacePure"),
         dptr = fn->isEqualTo(NVRM_DISPLAY_PTRS_FN), gAl = fn->isEqualTo(NVRM_GPUVA_FN_ALLOC),
         gMap = fn->isEqualTo(NVRM_GPUVA_FN_MAP), gFr = fn->isEqualTo(NVRM_GPUVA_FN_FREE),
         ssRm = fn->isEqualTo(NVRM_SS_FN_RM), ssPo = fn->isEqualTo(NVRM_SS_FN_PAGEOFF),
         vMap = fn->isEqualTo(NVRM_VRAM_FN_MAP), vUnmap = fn->isEqualTo(NVRM_VRAM_FN_UNMAP);
    if (!(vAlloc || vFree || scan || hand || flip || dptr || gAl || gMap || gFr || ssRm || ssPo || vMap || vUnmap)) return kIOReturnUnsupported;
    if (!p1) return kIOReturnBadArgument;
    if (scan) modeOf(p2);
    IOReturn ret = kIOReturnSuccess;
    IOLockLock(g.lock);
    if (gOwner && !gSelfTest) { IOLockUnlock(g.lock); return kIOReturnExclusiveAccess; }
    NvU32 st = openRm();
    if (st) { IOLockUnlock(g.lock); return kIOReturnNotReady; }
    g.abi = true;
    if (vMap || vUnmap) {
        /* mapa temporario de uma superficie sem mapa, para a leitura pela CPU (page-off de emergencia) */
        struct NVRMVramRequest *r = (struct NVRMVramRequest *)p1;
        if (r->version != NVRM_VRAM_ABI_VERSION) { ret = kIOReturnUnsupported; goto done; }
        MMSurf *s = surfFind(r->handle, r->allocationCookie);
        if (!s || !s->nomap) { ret = kIOReturnBadArgument; goto done; }
        if (vMap) {
            if (!s->tmpKva) {
                void *kva = NULL;
                st = NvRmMapMemory(g.hClient, g.hDevice, s->hMem, 0, s->size, &kva, 0);
                if (st || !kva) { r->kva = NULL; ret = kIOReturnNoMemory; goto done; }
                s->tmpKva = kva; gMxVramTempMaps++;
            }
            r->kva = s->tmpKva; r->actualSize = s->size;
        } else if (s->tmpKva) {
            NvRmUnmapMemory(g.hClient, g.hDevice, s->hMem, s->tmpKva, 0);
            s->tmpKva = NULL; r->kva = NULL;
        }
    } else if (vAlloc || vFree || scan) {
        struct NVRMVramRequest *r = (struct NVRMVramRequest *)p1;
        if (r->version != NVRM_VRAM_ABI_VERSION) { ret = kIOReturnUnsupported; goto done; }
        if (vAlloc) {
            const bool nomap = (r->flags & NVRM_VRAM_FLAG_NOMAP) != 0;
            r->flags &= ~NVRM_VRAM_FLAG_NOMAP;
            MMSurf *s = nomap ? surfAlloc(r->size, true) : surfAlloc(r->size);
            if (s && nomap) r->flags |= NVRM_VRAM_FLAG_NOMAP;   /* sem mapa recusado nao cai para a BAR1: fica sem VRAM */
            if (!s) { r->handle = NULL; r->kva = NULL; r->phys = 0; ret = kIOReturnNoMemory; goto done; }
            r->handle = s; r->kva = s->kva; r->phys = s->phys; r->actualSize = s->size; r->mappedTotal = a.mapped;
            r->allocationCookie = s->cookie;
        } else if (vFree) {
            MMSurf *s = surfFind(r->handle, r->allocationCookie);
            if (!s || s == a.home || s == a.dummy) { ret = kIOReturnBadArgument; goto done; }
            surfFree(s);
            r->handle = NULL; r->kva = NULL; r->phys = 0; r->actualSize = 0; r->allocationCookie = 0;
        } else {
            st = ensureHome();
            if (st || !a.home) { ret = kIOReturnNotReady; goto done; }
            r->phys = a.home->phys; r->kva = a.home->kva; r->handle = a.home;
            r->actualSize = (unsigned long long)a.modePitch * a.modeH;
            r->width = a.modeW; r->height = a.modeH; r->pitch = a.modePitch;
            r->allocationCookie = a.home->cookie;
        }
    } else if (hand) {
        struct NVRMRmHandles *h = (struct NVRMRmHandles *)p1;
        if (h->version != NVRM_GPUVA_ABI_VERSION) { ret = kIOReturnBadArgument; goto done; }
        h->kapiDevice = &g; h->hClient = g.hClient; h->hDevice = g.hDevice; h->hSubDevice = g.hSub;
    } else if (dptr) {
        struct NVRMDisplayPointers *d = (struct NVRMDisplayPointers *)p1;
        d->kapi = NULL; d->physForVa = NULL;
    } else if (flip) {
        struct NVRMFlipRequest *f = (struct NVRMFlipRequest *)p1;
        if (f->version != NVRM_FLIP_ABI_VERSION) { ret = kIOReturnBadArgument; goto done; }
        f->flipResult = -1;
        MMSurf *s;
        uint32_t w = f->width, h = f->height, pitch = f->pitch;
        if (f->flags & 1u) {
            st = ensureHome();
            s = a.home; w = a.modeW; h = a.modeH; pitch = a.modePitch;
        } else s = surfFind(f->kapiMemory, f->allocationCookie);
        if (!s) { ret = kIOReturnBadArgument; goto done; }
        modeDefault();
        if (w != a.modeW || h != a.modeH) { ret = kIOReturnIOError; goto done; }
        st = flipTo(s, w, h, pitch);
        f->flipResult = st ? -1 : 0;
        ret = st ? kIOReturnIOError : kIOReturnSuccess;
    } else if (gAl || gMap || gFr) {
        struct NVRMGpuVaRequest *q = (struct NVRMGpuVaRequest *)p1;
        if (q->version != NVRM_GPUVA_ABI_VERSION) { ret = kIOReturnBadArgument; goto done; }
        st = vaReady();
        if (st) { q->status = (int)st; ret = kIOReturnNotReady; goto done; }
        if (gAl) {
            NvU64 size = (q->size + 0xFFFull) & ~0xFFFull, align = q->align > VA_ALIGN ? q->align : VA_ALIGN, va = 0;
            MMVirt *v = virtFind(0);
            for (int i = 0; i < MAXVIRT && !v; i++) if (!a.virt[i].hVirt) v = &a.virt[i];
            if (!v || !size || !a.va->allocate(size, &va, align)) { q->status = 0x51; ret = kIOReturnNoSpace; goto done; }
            v->hVirt = ++a.nextVirt ? a.nextVirt : ++a.nextVirt; v->va = va; v->size = size;
            q->hVirt = v->hVirt; q->gpuva = va; q->status = 0;
        } else if (gMap) {
            MMVirt *v = virtFind(q->hVirt);
            MMSurf *s = surfFind(q->kapiMemory, 0);
            if (!v || !s) { q->status = 0x1F; ret = kIOReturnBadArgument; goto done; }
            NvU64 len = q->size ? q->size : s->size;
            if (len > v->size || len > s->size) { q->status = 0x1F; ret = kIOReturnBadArgument; goto done; }
            NvU64 dma = v->va;
            st = NvRmMapMemoryDma(g.hClient, g.hDevice, g.hCtxAll, s->hMem, 0, len, OS46_FIXED, &dma);
            q->status = (int)st;
            if (st) { ret = kIOReturnError; goto done; }
            q->hMemory = s->hMem; q->gpuva = dma;
        } else {
            MMVirt *v = virtFind(q->hVirt);
            if (!v) { q->status = 0x1F; ret = kIOReturnBadArgument; goto done; }
            if (q->hMemory) NvRmUnmapMemoryDma(g.hClient, g.hDevice, g.hCtxAll, q->hMemory, 0, v->va);
            if (!(q->flags & NVRM_GPUVA_FLAG_UNMAP_ONLY)) { a.va->deallocate(v->va, v->size); bzero(v, sizeof(*v)); }
            q->status = 0;
        }
    } else if (ssRm) {
        /* IOSurface do acelerador -> cliente RM do NVK. A r387 nao tem politica de compartilhamento (NV04_SHARE):
         * o kernel faz o dup direto no cliente do NVK. Entrada (porte): hClient = device do alvo, hObject = handle
         * novo escolhido pela libnvrm387; saida: hClient = cliente alvo, hObject = handle novo. */
        struct NVRMSurfShareRm *q = (struct NVRMSurfShareRm *)p1;
        if (q->version != NVRM_SS_RM_VERSION || q->op != NVRM_SS_RM_SHARE_KAPI) { q->status = NVRM_SS_BAD_VERSION; goto done; }
        MMSurf *sf = surfFind(q->kapiMemory, 0);
        if (!sf) { q->status = NVRM_SS_NO_VRAM; goto done; }
        if (!q->target || !q->hClient || !q->hObject) { q->status = NVRM_SS_OFF; goto done; }
        st = NvRmDupObject(q->target, q->hClient, q->hObject, g.hClient, sf->hMem, 0);
        static int ssLogged;
        if (ssLogged < 16 || st) { ssLogged++; BLOG("surfshare: VRAM 0x%x -> cliente 0x%x device 0x%x handle 0x%x: 0x%x",
                                                   sf->hMem, q->target, q->hClient, q->hObject, st); }
        q->status = st;
        if (!st) q->hClient = q->target;
    } else {
        /* nvSurfPageoff (copia pelo copy engine para paginas de sistema): nao portado */
        ret = kIOReturnUnsupported;
        ((struct NVRMSurfPageoff *)p1)->status = NVRM_SS_OFF;
    }
done:
    IOLockUnlock(g.lock);
    return ret;
}

/* Autoteste da ABI pelo cliente de teste (seletor 7): o mesmo caminho que o acelerador usa, passo a passo. */
static struct NVRMVramRequest gStV;
static uint64_t abiSelfTestStep(uint64_t step, uint64_t color, uint64_t *out)
{
    const OSSymbol *sym = NULL;
    IOReturn r = kIOReturnUnsupported;
    switch (step) {
    case 0: {
        struct NVRMVramRequest q; bzero(&q, sizeof(q)); q.version = NVRM_VRAM_ABI_VERSION;
        sym = OSSymbol::withCString(NVRM_VRAM_FN_SCANOUT);
        OSDictionary *m = IOService::serviceMatching("NVDA");
        OSIterator *it = m ? IOService::getMatchingServices(m) : NULL;
        if (m) m->release();
        while (it && !a.modeW) {
            IOService *fb = OSDynamicCast(IOService, it->getNextObject());
            if (!fb) break;
            modeOf(fb);
        }
        if (it) it->release();
        r = mmBackendPlatformFunction(sym, &q, NULL);
        out[1] = q.phys; out[2] = ((uint64_t)q.width << 32) | q.height; out[3] = q.pitch;
        break;
    }
    case 1: {
        bzero(&gStV, sizeof(gStV)); gStV.version = NVRM_VRAM_ABI_VERSION; gStV.size = (uint64_t)a.modePitch * a.modeH;
        sym = OSSymbol::withCString(NVRM_VRAM_FN_ALLOC);
        r = mmBackendPlatformFunction(sym, &gStV, NULL);
        if (!r && gStV.kva) { for (uint64_t i = 0; i < gStV.actualSize / 4; i++) ((volatile uint32_t *)gStV.kva)[i] = (uint32_t)color; }
        out[1] = gStV.phys; out[2] = gStV.actualSize; out[3] = gStV.mappedTotal;
        break;
    }
    case 2: case 3: {
        struct NVRMFlipRequest f; bzero(&f, sizeof(f)); f.version = NVRM_FLIP_ABI_VERSION;
        if (step == 3) f.flags = 1u;
        else { f.kapiMemory = gStV.handle; f.allocationCookie = gStV.allocationCookie; f.width = a.modeW; f.height = a.modeH; f.pitch = a.modePitch; }
        sym = OSSymbol::withCString(step == 3 ? "nvFlipToSurfacePure" : NVRM_FLIP_FN);
        r = mmBackendPlatformFunction(sym, &f, NULL);
        out[1] = (uint64_t)(int64_t)f.flipResult;
        break;
    }
    case 4: {
        struct NVRMGpuVaRequest q; bzero(&q, sizeof(q)); q.version = NVRM_GPUVA_ABI_VERSION;
        q.size = gStV.actualSize; q.align = 0x20000;
        sym = OSSymbol::withCString(NVRM_GPUVA_FN_ALLOC);
        r = mmBackendPlatformFunction(sym, &q, NULL); sym->release(); sym = NULL;
        out[1] = q.gpuva;
        if (r) break;
        q.kapiMemory = gStV.handle; q.isVidmem = 1;
        sym = OSSymbol::withCString(NVRM_GPUVA_FN_MAP);
        r = mmBackendPlatformFunction(sym, &q, NULL); sym->release(); sym = NULL;
        out[2] = q.gpuva; out[3] = (uint64_t)(uint32_t)q.status;
        if (r) break;
        sym = OSSymbol::withCString(NVRM_GPUVA_FN_FREE);
        r = mmBackendPlatformFunction(sym, &q, NULL);
        break;
    }
    case 6: {
        struct NVRMSurfShareRm q; bzero(&q, sizeof(q));
        q.version = NVRM_SS_RM_VERSION; q.op = NVRM_SS_RM_SHARE_KAPI; q.kapiDevice = &g; q.kapiMemory = gStV.handle;
        q.target = (unsigned)(color >> 32); q.hClient = (unsigned)color; q.hObject = 0xCAFE5500u;
        sym = OSSymbol::withCString(NVRM_SS_FN_RM);
        r = mmBackendPlatformFunction(sym, &q, NULL);
        out[1] = q.status; out[2] = q.hClient; out[3] = q.hObject;
        if (!r && q.status) r = kIOReturnError;
        break;
    }
    case 5:
        sym = OSSymbol::withCString(NVRM_VRAM_FN_FREE);
        r = mmBackendPlatformFunction(sym, &gStV, NULL);
        break;
    }
    if (sym) sym->release();
    BLOG("autoteste da ABI, passo %llu: 0x%x", step, r);
    return (uint64_t)(uint32_t)r;
}
