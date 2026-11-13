/*
 * test_xlate - testes da traducao r615 -> r387 com transporte falso (sem GPU).
 * O transporte grava cada escape r387 e responde como o RM r387 respondeu no hackintosh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "class/cl0080.h"
#include "alloc/alloc_channel.h"
#include "nvrm_r387.h"
#include "nvrm_xnu.h"
#include "nvrm387.h"

/* ------------------------------ transporte falso ------------------------------ */
#define MAXLOG 256
struct rec { uint32_t esc, size; uint8_t p[0x100]; uint8_t ap[0x100]; uint32_t apValid; };
static struct rec g_log[MAXLOG];
static int g_n;
#define FAKE_CLIENT 0xC1D00042u
#define FAKE_CPU_PTR 0x00007F0010000000ull

static int fake(uint32_t esc, void *params, uint32_t size)
{
    struct rec *r = &g_log[g_n < MAXLOG ? g_n++ : MAXLOG - 1];
    memset(r, 0, sizeof(*r));
    r->esc = esc; r->size = size;
    memcpy(r->p, params, size < sizeof(r->p) ? size : sizeof(r->p));
    switch (esc) {
    case NV387_ESC_RM_ALLOC: {
        NVOS21_PARAMETERS_R387 *a = params;
        if (a->hClass == 0 && a->pAllocParms) *(NvU32 *)(uintptr_t)a->pAllocParms = FAKE_CLIENT;
        if (a->pAllocParms) { memcpy(r->ap, (void *)(uintptr_t)a->pAllocParms, sizeof(r->ap)); r->apValid = 1; }
        a->status = NV_OK;
        if (a->hClass == 0xB0B5) {
            const NvU32 *q = (const NvU32 *)(uintptr_t)a->pAllocParms;
            if (!q || q[0] != 0 || q[1] != 2) a->status = 0x40;     /* so a GRCE (instancia 2) */
        }
        break;
    }
    case NV387_ESC_RM_FREE: ((NVOS00_PARAMETERS_R387 *)params)->status = NV_OK; break;
    case NV387_ESC_RM_CONTROL: {
        NVOS54_PARAMETERS_R387 *k = params;
        void *p = (void *)(uintptr_t)k->params;
        if (k->cmd == 0x800201) {
            NvU32 *q = p;                          /* {numClasses, pad, classList} */
            NvU64 list; memcpy(&list, q + 2, 8);
            q[0] = 3;
            if (list) { NvU32 *l = (NvU32 *)(uintptr_t)list; l[0] = 0xB06F; l[1] = 0xB197; l[2] = 0xB1C0; }
        } else if (k->cmd == 0x202) {
            NvU32 *q = p; q[1] = 0x21; q[2] = 0; q[3] = 0;
        } else if (k->cmd == 0x20801201) {
            /* valores reais da GTX 960 (r387), por indice r387 */
            NvU32 *q = p; NvU64 list; memcpy(&list, q + 2, 8);
            NvU32 *l = (NvU32 *)(uintptr_t)list;
            static const NvU32 hw[0x1E] = { [0x09] = 0x502, [0x0a] = 64, [0x0b] = 32, [0x14] = 4,
                                            [0x1a] = 1024, [0x1d] = 1 };
            if (l[0] >= 0x1E) { k->status = NV_ERR_INVALID_ARGUMENT; break; }
            l[1] = hw[l[0]];
        } else if (k->cmd == 0x20801301) {
            NvU32 *q = p; NvU64 list; memcpy(&list, q + 2, 8);
            NvU32 *l = (NvU32 *)(uintptr_t)list;
            for (NvU32 i = 0; i < q[0]; i++) l[2 * i + 1] = 0x1000 + l[2 * i];
        } else if (k->cmd == 0x201) {
            NvU32 *q = p; q[0] = 0x100;
        }
        if (p && k->paramsSize <= sizeof(r->ap)) { memcpy(r->ap, p, k->paramsSize); r->apValid = k->paramsSize; }
        k->status = NV_OK;
        break;
    }
    case NV387_ESC_RM_ALLOC_MEMORY: {
        NVOS02_PARAMETERS_R387 *m = params;
        if (m->hClass == 0x70) m->limit = 0xFFFFFFFFFFull;
        m->status = NV_OK;
        break;
    }
    case NV387_ESC_RM_MAP_MEMORY: {
        NVOS33_PARAMETERS_R387 *m = params;
        m->pLinearAddress = FAKE_CPU_PTR;
        m->status = NV_OK;
        break;
    }
    case NV387_ESC_RM_MAP_MEMORY_DMA: {
        NVOS46_PARAMETERS_R387 *m = params;
        if (!(m->flags & (1u << 15))) m->dmaOffset = 0x100000000ull;
        m->status = NV_OK;
        break;
    }
    case NV387_ESC_RM_UNMAP_MEMORY_DMA: ((NVOS47_PARAMETERS_R387 *)params)->status = NV_OK; break;
    case NV387_ESC_RM_VID_HEAP_CONTROL: {
        NVOS32_ALLOC_R387 *v = params;
        if (!(v->flags & NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED)) v->hMemory = 0xCAF00000;
        if (!(v->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE)) v->offset = 0x100000000ull;
        v->size = (v->size + 0x1FFFF) & ~0x1FFFFull;
        v->limit = v->size - 1;
        v->status = NV_OK;
        break;
    }
    case NV387_ESC_RM_ALLOC_CONTEXT_DMA2: ((NVOS39_PARAMETERS_R387 *)params)->status = NV_OK; break;
    case NV387_ESC_RM_BIND_CONTEXT_DMA: ((NvU32 *)params)[3] = NV_OK; break;
    default: return 22;
    }
    return 0;
}

/* ------------------------------ mini framework ------------------------------ */
static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("  FALHOU %s:%d: ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("   [%s]\n", #cond); } } while (0)
#define TEST(name) printf("- %s\n", name)
static struct rec *last(uint32_t esc)
{
    for (int i = g_n - 1; i >= 0; i--) if (g_log[i].esc == esc) return &g_log[i];
    return NULL;
}
static int count(uint32_t esc) { int n = 0; for (int i = 0; i < g_n; i++) if (g_log[i].esc == esc) n++; return n; }

/* chamadas no formato do nvRmApi.c do NullMoth (structs r615) */
static int g_fd;
static NvU32 api_alloc(NvHandle c, NvHandle parent, NvHandle *h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS a = { .hRoot = c, .hObjectParent = parent, .hObjectNew = *h, .hClass = cls, .pAllocParms = p };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_ALLOC, &a, sizeof(a))) return 0xEEEE;
    *h = a.hObjectNew;
    return a.status;
}
static NvU32 api_free(NvHandle c, NvHandle h)
{
    NVOS00_PARAMETERS f = { .hRoot = c, .hObjectOld = h };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_FREE, &f, sizeof(f))) return 0xEEEE;
    return f.status;
}
static NvU32 api_ctrl(NvHandle c, NvHandle o, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS k = { .hClient = c, .hObject = o, .cmd = cmd, .params = p, .paramsSize = size };
    if (nvrm_xnu_ioctl(g_fd, NV_ESC_RM_CONTROL, &k, sizeof(k))) return 0xEEEE;
    return k.status;
}

int main(void)
{
    nvrm387_reset_state();
    nvrm387_set_transport(fake);
    g_fd = nvrm_xnu_open("/dev/nvidiactl");
    CHECK(g_fd >= 0, "open");

    TEST("ALLOC cliente raiz: NVOS21 r615 -> r387 (status em 0x18), handle volta em hObjectNew");
    NvHandle hc = 0;
    NvU32 st = api_alloc(0, 0, &hc, 0x41, NULL);
    struct rec *r = last(NV387_ESC_RM_ALLOC);
    CHECK(st == NV_OK && hc == FAKE_CLIENT, "st 0x%x hc 0x%x", st, hc);
    CHECK(r && r->size == 0x20 && ((NVOS21_PARAMETERS_R387 *)r->p)->hClass == 0, "r387 root com classe 0");
    CHECK(offsetof(NVOS21_PARAMETERS, status) == 0x1C && offsetof(NVOS21_PARAMETERS_R387, status) == 0x18,
          "offsets do status");

    TEST("ALLOC device: so deviceId/hClientShare passam; vaMode/flags r615 ficam fora");
    NV0080_ALLOC_PARAMETERS dp = { .deviceId = 0, .hClientShare = hc, .flags = 0x8, .vaMode = 2 };
    NvHandle hDev = 0xD0000080;
    st = api_alloc(hc, hc, &hDev, 0x80, &dp);
    r = last(NV387_ESC_RM_ALLOC);
    NV0080_ALLOC_PARAMETERS_R387 *dr = (void *)r->ap;
    CHECK(st == NV_OK && r->apValid, "st 0x%x", st);
    CHECK(dr->hClientShare == hc && dr->flags == 0 && dr->vaMode == 0, "share 0x%x flags 0x%x vaMode %u",
          dr->hClientShare, dr->flags, dr->vaMode);

    TEST("ALLOC subdevice");
    NvU32 sdp = 0; NvHandle hSub = 0xD0002080;
    st = api_alloc(hc, hDev, &hSub, 0x2080, &sdp);
    CHECK(st == NV_OK, "st 0x%x", st);

    TEST("CONTROL CLASSLIST_V2 emulado por CLASSLIST (contagem + lista)");
    struct { NvU32 n; NvU32 l[200]; } cl;
    memset(&cl, 0, sizeof(cl));
    int before = count(NV387_ESC_RM_CONTROL);
    st = api_ctrl(hc, hDev, 0x800292, &cl, sizeof(cl));
    CHECK(st == NV_OK && cl.n == 3 && cl.l[0] == 0xB06F && cl.l[2] == 0xB1C0, "st 0x%x n %u", st, cl.n);
    CHECK(count(NV387_ESC_RM_CONTROL) - before == 2, "duas chamadas r387");
    CHECK(((NVOS54_PARAMETERS_R387 *)last(NV387_ESC_RM_CONTROL)->p)->cmd == 0x800201, "cmd r387");

    TEST("CONTROL GPU_GET_ID_INFO_V2 emulado por GET_ID_INFO (szName NULL)");
    struct { NvU32 gpuId, gpuFlags, devInst, subInst, sli, board, gi; NvS32 numa; } ii = { .gpuId = 0x100, .devInst = 99 };
    st = api_ctrl(hc, hc, 0x205, &ii, sizeof(ii));
    r = last(NV387_ESC_RM_CONTROL);
    CHECK(st == NV_OK && ii.devInst == 0 && ii.gpuFlags == 0x21, "st 0x%x dev %u", st, ii.devInst);
    CHECK(((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize == 0x28 && r->apValid == 0x28, "params r387 de 0x28");

    TEST("CONTROL NAME_STRING: r615 (68 bytes) -> r387 (0x84), so 64 bytes voltam");
    NvU8 nmb[68 + 32];
    memset(nmb, 0xA5, sizeof(nmb)); memset(nmb, 0, 68);
    st = api_ctrl(hc, hSub, 0x20800110, nmb, 68);
    r = last(NV387_ESC_RM_CONTROL);
    CHECK(st == NV_OK && ((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize == 0x84, "size r387 %u",
          ((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize);
    int intact = 1; for (int i = 68; i < (int)sizeof(nmb); i++) if (nmb[i] != 0xA5) intact = 0;
    CHECK(intact, "nada escrito alem de 68 bytes");
    CHECK(api_ctrl(hc, hSub, 0x20800110, nmb, 0x84) == NV_ERR_INVALID_PARAM_STRUCT, "tamanho r387 direto recusa");

    TEST("CONTROL FB_GET_INFO_V2 -> FB_GET_INFO por ponteiro (preenche in place)");
    struct { NvU32 n; struct { NvU32 index, data; } l[128]; } fb;
    memset(&fb, 0, sizeof(fb));
    fb.n = 2; fb.l[0].index = 7; fb.l[1].index = 5;
    st = api_ctrl(hc, hSub, 0x20801303, &fb, sizeof(fb));
    r = last(NV387_ESC_RM_CONTROL);
    CHECK(st == NV_OK && fb.l[0].data == 0x1007 && fb.l[1].data == 0x1005, "dados 0x%x 0x%x", fb.l[0].data, fb.l[1].data);
    CHECK(((NVOS54_PARAMETERS_R387 *)r->p)->cmd == 0x20801301 && ((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize == 0x10,
          "cmd/size r387");
    fb.n = 200;
    CHECK(api_ctrl(hc, hSub, 0x20801303, &fb, sizeof(fb)) == NV_ERR_INVALID_ARGUMENT, "lista grande demais recusa");

    TEST("CONTROL GR_GET_INFO_V2 (device) -> GR_GET_INFO 0x20801201 (subdevice), indice - 3");
    struct { NvU32 n; struct { NvU32 index, data; } l[0x3F]; NvU32 pad[5]; } gr;
    memset(&gr, 0, sizeof(gr));
    gr.n = 4; gr.l[0].index = 0x0C; gr.l[1].index = 0x0D; gr.l[2].index = 0x20; gr.l[3].index = 0x22 /* RT cores */;
    st = api_ctrl(hc, hDev, 0x801110, &gr, sizeof(gr));
    CHECK(st == NV_OK && gr.l[0].data == 0x502 && gr.l[1].data == 64 && gr.l[2].data == 1 && gr.l[3].data == 0,
          "SM 0x%x warps %u sm/tpc %u rt %u", gr.l[0].data, gr.l[1].data, gr.l[2].data, gr.l[3].data);
    r = last(NV387_ESC_RM_CONTROL);
    CHECK(((NVOS54_PARAMETERS_R387 *)r->p)->hObject == hSub && ((NVOS54_PARAMETERS_R387 *)r->p)->cmd == 0x20801201,
          "vai pelo subdevice");

    TEST("CONTROL GPC/TPC mask derivados: 1024 cores / 128 / 1 SM por TPC / 4 TPC por GPC = 2 GPCs");
    NvU32 gm[6] = { 0 };
    st = api_ctrl(hc, hSub, 0x2080122A, gm, 24);
    CHECK(st == NV_OK && gm[4] == 0x3, "gpcMask 0x%x", gm[4]);
    NvU32 tpm[6] = { 0 }; tpm[4] = 1;
    st = api_ctrl(hc, hSub, 0x2080122B, tpm, 24);
    CHECK(st == NV_OK && tpm[5] == 0xF, "tpcMask gpc1 0x%x", tpm[5]);
    NvU32 tm2[6] = { 0 }; tm2[4] = 2;
    CHECK(api_ctrl(hc, hSub, 0x2080122B, tm2, 24) == NV_OK && tm2[5] == 0, "gpc inexistente 0x%x", tm2[5]);

    TEST("CONTROL MC_GET_ARCH_INFO / TIMER passam direto");
    NvU32 arch[4] = { 0 };
    CHECK(api_ctrl(hc, hSub, 0x20801701, arch, 16) == NV_OK, "arch");
    CHECK(((NVOS54_PARAMETERS_R387 *)last(NV387_ESC_RM_CONTROL)->p)->cmd == 0x20801701, "cmd");
    NvU64 tm = 0;
    CHECK(api_ctrl(hc, hSub, 0x20800403, &tm, 8) == NV_OK, "timer");

    TEST("CONTROL desconhecido: NOT_SUPPORTED sem tocar o RM");
    before = g_n;
    NvU32 dummy[4] = { 0 };
    st = api_ctrl(hc, hSub, 0x20801301, dummy, sizeof(dummy));
    CHECK(st == NV_ERR_NOT_SUPPORTED && g_n == before, "st 0x%x escapes %d", st, g_n - before);

    TEST("FERMI_VASPACE_A: emulado, nada vai ao RM");
    before = g_n;
    NV_VASPACE_ALLOCATION_PARAMETERS vs = { .vaBase = 0x200000000ull };
    NvHandle hVas = 0xD00090F1;
    st = api_alloc(hc, hDev, &hVas, 0x90F1, &vs);
    CHECK(st == NV_OK && g_n == before, "st 0x%x", st);

    TEST("NV01_MEMORY_SYSTEM por ALLOC -> ALLOC_MEMORY 0x3E write-combine");
    NV_MEMORY_ALLOCATION_PARAMS mp = { .size = 0x10000 };
    NvHandle hMem = 0xD000003E;
    st = api_alloc(hc, hDev, &hMem, 0x3E, &mp);
    r = last(NV387_ESC_RM_ALLOC_MEMORY);
    NVOS02_PARAMETERS_R387 *mr = (void *)r->p;
    CHECK(st == NV_OK && mr->hClass == 0x3E && mr->flags == 0x2010 && mr->limit == 0xFFFF, "flags 0x%x limit 0x%llx",
          mr->flags, (unsigned long long)mr->limit);
    CHECK(mp.limit == 0xFFFF && mp.size == 0x10000, "params r615 preenchidos");

    TEST("NV01_MEMORY_LOCAL_USER -> VRAM fisica (NVOS32 sem VIRTUAL, VIDMEM, WC, handle fornecido)");
    NV_MEMORY_ALLOCATION_PARAMS lp = { .size = 0x30000, .alignment = 0x20000, .flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
                                       .attr = (3u << 27) | (2u << 23) | (1u << 25) /* noncontig, big, PCI */ };
    NvHandle hLoc = 0xD0000040;
    st = api_alloc(hc, hDev, &hLoc, 0x40, &lp);
    r = last(NV387_ESC_RM_VID_HEAP_CONTROL);
    NVOS32_ALLOC_R387 *lr = (void *)r->p;
    CHECK(st == NV_OK && lr->hMemory == hLoc && lr->hObjectParent == hDev, "st 0x%x", st);
    CHECK(!(lr->flags & NVOS32_ALLOC_FLAGS_VIRTUAL) && (lr->flags & NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED) &&
          (lr->flags & NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE), "flags 0x%x", lr->flags);
    CHECK(((lr->attr >> 25) & 3) == 0 && (lr->attr >> 29) == 2 && ((lr->attr >> 23) & 3) == 1 && ((lr->attr >> 27) & 3) == 3,
          "attr 0x%08x (VIDMEM, WC, pagina 4 KiB forcada, fisicalidade preservada)", lr->attr);
    CHECK(lr->size == 0x30000 && lr->alignment == 0x20000, "size/align");

    TEST("MAP_MEMORY com fd: o ponteiro do RM volta pelo mmap(fd)");
    int memFd = nvrm_xnu_open("/dev/nvidiactl");
    nv_ioctl_nvos33_parameters_with_fd mm = { .params = { .hClient = hc, .hDevice = hDev, .hMemory = hMem,
                                                         .length = 0x10000 }, .fd = memFd };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY, &mm, sizeof(mm)) == 0 && mm.params.status == NV_OK, "map");
    void *cpu = nvrm_xnu_mmap(memFd, 0x10000);
    CHECK(cpu == (void *)(uintptr_t)FAKE_CPU_PTR, "mmap %p", cpu);
    CHECK(nvrm_xnu_mmap(memFd, 0x20000) == MAP_FAILED, "mmap maior que o mapa recusa");
    nvrm_xnu_close(memFd);

    TEST("NV50_MEMORY_VIRTUAL: cria 0x70 + ctxdma uma vez e reserva VA da lib");
    int a70 = count(NV387_ESC_RM_ALLOC_MEMORY), actx = count(NV387_ESC_RM_ALLOC_CONTEXT_DMA2);
    NV_MEMORY_ALLOCATION_PARAMS vp = { .flags = NVOS32_ALLOC_FLAGS_VIRTUAL, .size = 0x10000, .hVASpace = hVas };
    NvHandle hVirt = 0xD00050A0;
    st = api_alloc(hc, hDev, &hVirt, 0x50A0, &vp);
    CHECK(st == NV_OK && vp.offset == NVRM387_VA_DEFAULT_BASE, "st 0x%x VA 0x%llx", st, (unsigned long long)vp.offset);
    CHECK(count(NV387_ESC_RM_ALLOC_MEMORY) - a70 == 1 && count(NV387_ESC_RM_ALLOC_CONTEXT_DMA2) - actx == 1,
          "0x70 + ctxdma criados");
    NVOS39_PARAMETERS_R387 *ctx = (void *)last(NV387_ESC_RM_ALLOC_CONTEXT_DMA2)->p;
    const NvHandle hCtxAll = ctx->hObjectNew;
    CHECK(ctx->limit == 0xFFFFFFFFFFull && ctx->hObjectParent == hc, "ctxdma cobre o VA, pai = cliente");
    NV_MEMORY_ALLOCATION_PARAMS vp2 = { .flags = NVOS32_ALLOC_FLAGS_VIRTUAL, .size = 0x1000, .hVASpace = hVas };
    NvHandle hVirt2 = 0xD00050A1;
    st = api_alloc(hc, hDev, &hVirt2, 0x50A0, &vp2);
    CHECK(st == NV_OK && vp2.offset == NVRM387_VA_DEFAULT_BASE + 0x20000, "2o VA alinhado em 128 KiB 0x%llx", (unsigned long long)vp2.offset);
    CHECK(count(NV387_ESC_RM_ALLOC_CONTEXT_DMA2) - actx == 1, "ctxdma nao repete");

    TEST("MAP_MEMORY_DMA no virtual -> ctxdma global, offset fixo absoluto");
    NVOS46_PARAMETERS md = { .hClient = hc, .hDevice = hDev, .hDma = hVirt, .hMemory = hMem, .length = 0x10000,
                             .flags = 0x10 | (1u << 31) /* snoop + bit que nao passa */, .dmaOffset = 0 };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &md, sizeof(md)) == 0 && md.status == NV_OK, "map dma");
    NVOS46_PARAMETERS_R387 *dmr = (void *)last(NV387_ESC_RM_MAP_MEMORY_DMA)->p;
    CHECK(dmr->hDma == hCtxAll && dmr->flags == (0x10u | (1u << 15)) && dmr->dmaOffset == NVRM387_VA_DEFAULT_BASE,
          "hDma 0x%x flags 0x%x off 0x%llx", dmr->hDma, dmr->flags, (unsigned long long)dmr->dmaOffset);
    CHECK(md.dmaOffset == 0, "offset relativo volta 0x%llx", (unsigned long long)md.dmaOffset);
    NVOS46_PARAMETERS md2 = md; md2.length = 0x20000;
    nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &md2, sizeof(md2));
    CHECK(md2.status == NV_ERR_INVALID_LIMIT, "mapa maior que o virtual recusa");

    TEST("UNMAP_MEMORY_DMA: NVOS47 r615 (0x30) -> r387 (0x28), VA absoluto");
    NVOS47_PARAMETERS um = { .hClient = hc, .hDevice = hDev, .hDma = hVirt, .hMemory = hMem, .flags = 0x10, .dmaOffset = 0 };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_UNMAP_MEMORY_DMA, &um, sizeof(um)) == 0 && um.status == NV_OK, "unmap");
    r = last(NV387_ESC_RM_UNMAP_MEMORY_DMA);
    NVOS47_PARAMETERS_R387 *umr = (void *)r->p;
    CHECK(r->size == 0x28 && umr->dmaOffset == NVRM387_VA_DEFAULT_BASE && umr->hDma == hCtxAll, "layout r387");

    TEST("canal 0xB06F: prefixo r615, hObjectBuffer = ctxdma do VA, sem PRIVILEGED, BIND automatico");
    NV_CHANNEL_ALLOC_PARAMS ch;
    memset(&ch, 0, sizeof(ch));
    ch.hObjectError = 0xD0000002; ch.gpFifoOffset = vp.offset; ch.gpFifoEntries = 0x200; ch.flags = 0xFFFF;
    ch.hVASpace = hVas; ch.engineType = 1;
    NvHandle hChan = 0xD000B06F;
    int nbind = count(NV387_ESC_RM_BIND_CONTEXT_DMA);
    st = api_alloc(hc, hDev, &hChan, 0xB06F, &ch);
    struct rec *ra = NULL;
    for (int i = g_n - 1; i >= 0; i--)
        if (g_log[i].esc == NV387_ESC_RM_ALLOC && ((NVOS21_PARAMETERS_R387 *)g_log[i].p)->hClass == 0xB06F) { ra = &g_log[i]; break; }
    CHECK(st == NV_OK && ra && ra->apValid, "st 0x%x", st);
    NvU32 *cp = (NvU32 *)ra->ap; NvU64 gpf; memcpy(&gpf, &cp[2], 8);
    CHECK(cp[0] == 0xD0000002 && cp[1] == hCtxAll && gpf == vp.offset && cp[4] == 0x200 && cp[5] == 0 && cp[6] == 0 && cp[7] == 0,
          "err 0x%x buf 0x%x gpfifo 0x%llx n 0x%x flags 0x%x", cp[0], cp[1], (unsigned long long)gpf, cp[4], cp[5]);
    CHECK(count(NV387_ESC_RM_BIND_CONTEXT_DMA) - nbind == 1, "BIND_CONTEXT_DMA");
    NvU32 *bp = (NvU32 *)last(NV387_ESC_RM_BIND_CONTEXT_DMA)->p;
    CHECK(bp[0] == hc && bp[1] == hChan && bp[2] == hCtxAll, "bind args");

    TEST("canal com hContextShare (TSG): NOT_SUPPORTED");
    NV_CHANNEL_ALLOC_PARAMS ch2 = ch; ch2.hContextShare = 0xD0009067;
    NvHandle hChan2 = 0xD000B070;
    CHECK(api_alloc(hc, hDev, &hChan2, 0xB06F, &ch2) == NV_ERR_NOT_SUPPORTED, "recusa");
    NvHandle hTsg = 0xD000A06C;
    CHECK(api_alloc(hc, hDev, &hTsg, 0xA06C, &ch2) == NV_ERR_NOT_SUPPORTED, "TSG recusa");

    TEST("GPFIFO_SCHEDULE r615 (3 bytes) -> r387 (1 byte bEnable)");
    NvU8 sch[3] = { 1, 0, 0 };
    st = api_ctrl(hc, hChan, 0xA06F0103, sch, 3);
    r = last(NV387_ESC_RM_CONTROL);
    CHECK(st == NV_OK && ((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize == 1 && r->ap[0] == 1, "size %u",
          ((NVOS54_PARAMETERS_R387 *)r->p)->paramsSize);
    CHECK(api_ctrl(hc, hChan, 0xA06F0104, sch, 4) == NV_OK, "A06F BIND vira no-op");
    CHECK(api_ctrl(hc, hChan, 0xC36F0108, sch, 4) == NV_ERR_NOT_SUPPORTED, "work submit token recusado");

    TEST("engine MAXWELL_COMPUTE_B: params r615 ignorados, vai NULL");
    NvU32 junk[4] = { 7, 7, 7, 7 };
    NvHandle hEng = 0xD000B1C0;
    st = api_alloc(hc, hChan, &hEng, 0xB1C0, junk);
    r = last(NV387_ESC_RM_ALLOC);
    CHECK(st == NV_OK && ((NVOS21_PARAMETERS_R387 *)r->p)->pAllocParms == 0, "NULL");

    TEST("copy engine 0xB0B5 no canal de graficos: NULL da 0x40, lib acha a GRCE por VERSION_0");
    NvHandle hCe = 0xD000B0B5;
    st = api_alloc(hc, hChan, &hCe, 0xB0B5, NULL);
    r = last(NV387_ESC_RM_ALLOC);
    CHECK(st == NV_OK && r->apValid && ((NvU32 *)r->ap)[0] == 0 && ((NvU32 *)r->ap)[1] == 2, "st 0x%x", st);
    NvU32 v1[2] = { 1, 0x09 + 2 };                              /* VERSION_1 COPY2 */
    NvHandle hCe2 = 0xD000B0B6;
    int nb = count(NV387_ESC_RM_ALLOC);
    st = api_alloc(hc, hChan, &hCe2, 0xB0B5, v1);
    CHECK(st == NV_OK && count(NV387_ESC_RM_ALLOC) - nb == 2, "VERSION_1 COPY2 vira instancia 2 na 1a tentativa (%d)",
          count(NV387_ESC_RM_ALLOC) - nb);

    TEST("FREE de virtual emulado: so devolve o VA (reuso na proxima alocacao)");
    before = g_n;
    CHECK(api_free(hc, hVirt) == NV_OK && g_n == before, "sem RM");
    NV_MEMORY_ALLOCATION_PARAMS vp3 = { .flags = NVOS32_ALLOC_FLAGS_VIRTUAL, .size = 0x8000, .hVASpace = hVas };
    NvHandle hVirt3 = 0xD00050A2;
    st = api_alloc(hc, hDev, &hVirt3, 0x50A0, &vp3);
    CHECK(st == NV_OK && vp3.offset == NVRM387_VA_DEFAULT_BASE, "VA reusado 0x%llx", (unsigned long long)vp3.offset);

    TEST("FREE de virtual emulado com mapa vivo: desfaz o mapa no RM antes de devolver o VA");
    NVOS46_PARAMETERS md3 = { .hClient = hc, .hDevice = hDev, .hDma = hVirt3, .hMemory = hMem, .length = 0x8000 };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &md3, sizeof(md3)) == 0 && md3.status == NV_OK, "map");
    int nun = count(NV387_ESC_RM_UNMAP_MEMORY_DMA);
    CHECK(api_free(hc, hVirt3) == NV_OK && count(NV387_ESC_RM_UNMAP_MEMORY_DMA) - nun == 1, "um UNMAP no FREE");
    NVOS47_PARAMETERS_R387 *fu = (void *)last(NV387_ESC_RM_UNMAP_MEMORY_DMA)->p;
    CHECK(fu->hDma == hCtxAll && fu->hMemory == hMem && fu->dmaOffset == NVRM387_VA_DEFAULT_BASE,
          "hDma 0x%x mem 0x%x VA 0x%llx", fu->hDma, fu->hMemory, (unsigned long long)fu->dmaOffset);
    NvHandle hVirt4 = 0xD00050A3;
    NV_MEMORY_ALLOCATION_PARAMS vp4 = vp3;
    st = api_alloc(hc, hDev, &hVirt4, 0x50A0, &vp4);
    CHECK(st == NV_OK && vp4.offset == NVRM387_VA_DEFAULT_BASE, "VA reusado 0x%llx", (unsigned long long)vp4.offset);
    CHECK(api_free(hc, hVirt4) == NV_OK && count(NV387_ESC_RM_UNMAP_MEMORY_DMA) - nun == 1, "virtual sem mapa: nenhum UNMAP");

    TEST("NVOS32 VIRTUAL kind 0xFE: handle fornecido, BLOCK_LINEAR e VA nativo");
    NV_MEMORY_ALLOCATION_PARAMS vk = { .flags = NVOS32_ALLOC_FLAGS_VIRTUAL, .size = 0x10000,
                                      .attr = 0x20000, .format = 0xFE, .hVASpace = hVas };
    NvHandle hKind = 0xD00050FE;
    st = api_alloc(hc, hDev, &hKind, 0x50A0, &vk);
    r = last(NV387_ESC_RM_VID_HEAP_CONTROL);
    NVOS32_ALLOC_R387 *vr = (void *)r->p;
    CHECK(st == NV_OK && vk.offset == 0x100000000ull && vk.limit == 0x10001FFFFull,
          "st 0x%x VA 0x%llx limit 0x%llx", st, (unsigned long long)vk.offset, (unsigned long long)vk.limit);
    CHECK(vr->hMemory == hKind && vr->format == 0xFE && vr->attr == 0x20000, "handle/kind/attr");
    CHECK((vr->flags & (NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED)) ==
          (NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED), "flags virtual+provided 0x%x", vr->flags);

    TEST("MAP/UNMAP no virtual nativo: hDma direto, PAGE_KIND_VIRTUAL e VA absoluto");
    NVOS46_PARAMETERS mk = { .hClient = hc, .hDevice = hDev, .hDma = hKind, .hMemory = hMem,
                             .length = 0x10000, .flags = 0x18, .dmaOffset = 0 };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_MAP_MEMORY_DMA, &mk, sizeof(mk)) == 0 && mk.status == NV_OK, "map kind");
    dmr = (void *)last(NV387_ESC_RM_MAP_MEMORY_DMA)->p;
    CHECK(dmr->hDma == hKind && dmr->flags == (0x18 | (1u << 15)) && dmr->dmaOffset == 0x100000000ull,
          "hDma 0x%x flags 0x%x off 0x%llx", dmr->hDma, dmr->flags, (unsigned long long)dmr->dmaOffset);
    CHECK(mk.dmaOffset == 0, "offset relativo volta 0x%llx", (unsigned long long)mk.dmaOffset);
    NVOS47_PARAMETERS uk = { .hClient = hc, .hDevice = hDev, .hDma = hKind, .hMemory = hMem,
                             .flags = 0x18, .dmaOffset = 0 };
    CHECK(nvrm_xnu_ioctl(g_fd, NV_ESC_RM_UNMAP_MEMORY_DMA, &uk, sizeof(uk)) == 0 && uk.status == NV_OK, "unmap kind");
    umr = (void *)last(NV387_ESC_RM_UNMAP_MEMORY_DMA)->p;
    CHECK(umr->hDma == hKind && umr->dmaOffset == 0x100000000ull && umr->flags == 0x18, "unmap nativo");
    before = count(NV387_ESC_RM_FREE);
    CHECK(api_free(hc, hKind) == NV_OK && count(NV387_ESC_RM_FREE) - before == 1, "free nativo vai ao RM");

    TEST("FREE do canal: pai (device) vem da tabela; filhos saem junto");
    int nobj = nvrm387_object_count();
    CHECK(api_free(hc, hChan) == NV_OK, "free");
    NVOS00_PARAMETERS_R387 *fr = (void *)last(NV387_ESC_RM_FREE)->p;
    CHECK(fr->hObjectParent == hDev && fr->hObjectOld == hChan, "pai 0x%x", fr->hObjectParent);
    CHECK(nvrm387_object_count() == nobj - 4, "canal + engine + 2 CE removidos (%d -> %d)", nobj, nvrm387_object_count());

    TEST("FREE do cliente: RM libera em cascata, tabela zera");
    CHECK(api_free(hc, hc) == NV_OK, "free cliente");
    fr = (void *)last(NV387_ESC_RM_FREE)->p;
    CHECK(fr->hRoot == hc && fr->hObjectParent == hc && fr->hObjectOld == hc, "r387 (c,c,c)");
    CHECK(nvrm387_object_count() == 0, "tabela vazia (%d)", nvrm387_object_count());

    TEST("escape desconhecido: EINVAL");
    NvU32 z[8] = { 0 };
    CHECK(nvrm_xnu_ioctl(g_fd, 0x99, z, sizeof(z)) != 0, "einval");

    nvrm_xnu_close(g_fd);
    printf("\n%d verificacoes ok, %d falharam\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
