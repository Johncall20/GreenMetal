/*
 * libnvrm387 - nvrm_xnu.h (backend nvkmd/nvrm do NVK do NullMoth) sobre o RM r387 do
 * NVIDIA Web Driver 10.3.3, pelo user client NVDAUser do NVDAResmanWeb.
 *
 * Entrada: structs da r615 (open-gpu-kernel-modules 615.78.08, MIT).
 * Saida:   escapes r387 (ver ../../nvrm_r387.h e ../../docs/ANALISE_RM.md).
 *
 * Decisoes (detalhes no README):
 *  - Um unico user client NVDAUser por processo; os fds devolvidos sao /dev/null, so numeros.
 *  - MAP_MEMORY: o RM r387 ja mapeia na task e devolve o ponteiro; ele fica guardado no fd
 *    passado no escape, e nvrm_xnu_mmap(fd) so o devolve. munmap e no-op (UNMAP_MEMORY desfaz).
 *  - FERMI_VASPACE_A e NV50_MEMORY_VIRTUAL sem kind sao emulados: a r387 usa o VA padrao
 *    do device; a lib reserva faixas num alocador proprio e mapeia num context DMA global.
 *    Com kind 0xFE, usa NVOS32 ALLOC_SIZE+VIRTUAL e MAP_MEMORY_DMA direto com PAGE_KIND_VIRTUAL.
 *  - NV01_MEMORY_SYSTEM vira ALLOC_MEMORY 0x3E write-combine (cached sem snoop nao e
 *    coerente; ver ANALISE_RM.md 7d). NV01_MEMORY_LOCAL_USER (VRAM) ainda cai em memoria de
 *    sistema (com aviso), ate a alocacao de VRAM da r387 ser mapeada.
 *  - Controles: lista branca. Desconhecido => NV_ERR_NOT_SUPPORTED sem tocar o RM
 *    (NVRM387_CTRL_PASSTHROUGH=1 manda direto, por conta e risco).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#ifndef NVRM387_MOCK
#include <IOKit/IOKitLib.h>
#endif

/* ---------------------------------------------------------------------------------------- */
/* Classes e controles                                                                       */
/* ---------------------------------------------------------------------------------------- */
#define CL_ROOT            0x0000u
#define CL_ROOT_CLIENT     0x0041u
#define CL_CONTEXT_DMA     0x0002u
#define CL_EVENT           0x0005u
#define CL_EVENT_OS_EVENT  0x0079u
#define CL_MEM_SYSTEM      0x003Eu
#define CL_MEM_LOCAL_USER  0x0040u
#define CL_MEM_VIRTUAL_R387 0x0070u
#define CL_DEVICE          0x0080u
#define CL_SUBDEVICE       0x2080u
#define CL_VASPACE         0x90F1u
#define CL_MEM_VIRTUAL     0x50A0u
#define CL_CTXSHARE        0x9067u
#define CL_TSG             0xA06Cu
#define CL_CHANNEL_B06F    0xB06Fu

#define CTRL_ATTACHED_IDS      0x00000201u
#define CTRL_ID_INFO           0x00000202u
#define CTRL_ID_INFO_V2        0x00000205u
#define CTRL_CLASSLIST         0x00800201u
#define CTRL_CLASSLIST_V2      0x00800292u
#define CTRL_NAME_STRING       0x20800110u
#define CTRL_SHORT_NAME        0x20800111u
#define CTRL_MC_ARCH_INFO      0x20801701u
#define CTRL_TIMER_GET_TIME    0x20800403u
#define CTRL_FIFO_ENG_CTX_PROP 0x00801707u
#define CTRL_ENGINE_PARTNERS   0x20800147u
#define CTRL_FB_GET_INFO       0x20801301u
#define CTRL_FB_GET_INFO_V2    0x20801303u
#define CTRL_GR_GET_INFO_V2    0x00801110u   /* r615, no device */
#define CTRL_GR_GET_INFO_2080  0x20801201u   /* r387: no subdevice, {n, ponteiro} */
#define CTRL_GR_GPC_MASK       0x2080122Au   /* r615; nao existe na r387 */
#define CTRL_GR_TPC_MASK       0x2080122Bu   /* r615; nao existe na r387 */
#define CTRL_A06F_SCHEDULE     0xA06F0103u
#define CTRL_A06F_BIND         0xA06F0104u
#define CTRL_C36F_GET_TOKEN    0xC36F0108u
#define CTRL_C36F_TOKEN_NOTIF  0xC36F010Au

#define MEM_FLAGS_SYS_WC       0x00002010u  /* nao contigua, PCI, write-combine */
#define MEM_FLAGS_SYS_CACHED   0x00001010u  /* nao contigua, PCI, cached */
#define CTXDMA_FLAGS_DEFAULT   0x20000000u  /* o que o GeForceWeb usa */
#define OS46_FLAGS_KEEP        0x00000013u  /* ACCESS 1:0, CACHE_SNOOP 4:4 */
#define OS46_FLAG_KIND         (1u << 3)
#define OS46_FLAG_FIXED        (1u << 15)
#define ATTR_PAGE_SIZE_MASK (3u << 23)   /* NVOS32_ATTR_PAGE_SIZE 24:23 */
#define ATTR_PAGE_SIZE_4KB  (1u << 23)
#define MAXWELL_BIG_PAGE       0x20000ull     /* pagina grande do VA no Maxwell: 128 KiB */

/* ---------------------------------------------------------------------------------------- */
/* Log                                                                                       */
/* ---------------------------------------------------------------------------------------- */
static int g_log = -1;
static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && e[0] && e[0] != '0';
}
#define LOG(...)  do { if (g_log < 0) g_log = env_on("NVRM387_LOG"); \
                       if (g_log) fprintf(stderr, "nvrm387: " __VA_ARGS__); } while (0)
#define WARN(...) fprintf(stderr, "nvrm387: " __VA_ARGS__)

/* ---------------------------------------------------------------------------------------- */
/* Transporte NVDAUser                                                                       */
/* ---------------------------------------------------------------------------------------- */
#define NVDAUSER_BUF_MAX 1024u

#ifndef NVRM387_MOCK
static io_connect_t g_conn;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

/* ponte do MaxwellMetal.kext: um IOAccelerator, que a App Sandbox deixa abrir; repassa ao NVDAUser no kernel */
#define MM_BRIDGE_CLASS "MaxwellMetal"
#define MM_BRIDGE_TYPE  0x4D58524Du   /* 'MXRM' */
static int g_via_bridge;

/* Ordem: o acelerador (MaxwellAccel, classe NVAccel: cliente RM tipo 0x4E56xxxx, a ponte depois da troca e aceita pela
 * App Sandbox), a ponte para sandbox do sistema antigo (MaxwellMetalBridge) e o servico MaxwellMetal. */
static kern_return_t bridge_try(const char *cls, uint32_t type)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(cls));
    if (!svc) return kIOReturnNotFound;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), type, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { g_conn = 0; LOG("ponte %s recusou (0x%08x)\n", cls, kr); }
    else LOG("RM pela ponte %s\n", cls);
    return kr;
}
static kern_return_t bridge_connect(void)
{
    if (env_on("NVRM387_NO_BRIDGE")) return kIOReturnNotFound;
    if (!env_on("NVRM387_NO_ACCEL") && bridge_try("NVAccel", 0x4E560000u) == KERN_SUCCESS) return KERN_SUCCESS;
    if (bridge_try("MaxwellMetalBridge", MM_BRIDGE_TYPE) == KERN_SUCCESS) return KERN_SUCCESS;
    kern_return_t kr = bridge_try(MM_BRIDGE_CLASS, MM_BRIDGE_TYPE);
    if (kr != KERN_SUCCESS) WARN("nenhuma ponte (NVAccel, MaxwellMetalBridge, MaxwellMetal): tentando o NVDAUser direto\n");
    return kr;
}

static kern_return_t nvdauser_connect(void)
{
    kern_return_t kr = KERN_SUCCESS;
    pthread_mutex_lock(&g_conn_lock);
    if (!g_conn && bridge_connect() == KERN_SUCCESS) {
        g_via_bridge = 1;
        LOG("RM pela ponte MaxwellMetal (funciona dentro da App Sandbox)\n");
    }
    if (!g_conn) {
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
        if (!svc) {
            kr = kIOReturnNotFound;
        } else {
            kr = IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &g_conn);
            IOObjectRelease(svc);
            if (kr != KERN_SUCCESS) {
                g_conn = 0;
                WARN("IOServiceOpen(NVDA, 0x%08x) -> 0x%08x%s\n", NV387_NVDAUSER_OPEN_TYPE, kr,
                     kr == kIOReturnNotPrivileged ? " (precisa de root ou boot-arg nvrm=0x8)" : "");
            }
        }
    }
    pthread_mutex_unlock(&g_conn_lock);
    return kr;
}

static int nvdauser_escape(uint32_t esc, void *params, uint32_t size)
{
    uint8_t buf[NVDAUSER_BUF_MAX];
    const uint32_t hdr = (uint32_t)sizeof(NV387_NVDAUSER_HEADER);
    if (size > sizeof(buf) - hdr) return EINVAL;
    if (!g_conn && nvdauser_connect() != KERN_SUCCESS) return ENODEV;
    NV387_NVDAUSER_HEADER h = { NV387_ESC_BASE | esc, size };
    memcpy(buf, &h, hdr);
    memcpy(buf + hdr, params, size);
    size_t in = hdr + size, out = in;
    kern_return_t kr = IOConnectCallStructMethod(g_conn, NV387_NVDAUSER_SEL_RM_API, buf, in, buf, &out);
    if (kr != KERN_SUCCESS) { WARN("esc 0x%02x: IOConnectCallStructMethod 0x%08x\n", esc, kr); return EIO; }
    if (out != in) { WARN("esc 0x%02x: outSize %zu != %zu\n", esc, out, in); return EIO; }
    memcpy(params, buf + hdr, size);
    return 0;
}
static nvrm387_transport_fn g_xport = nvdauser_escape;
#else
static int no_transport(uint32_t esc, void *p, uint32_t size) { (void)esc; (void)p; (void)size; return ENODEV; }
static nvrm387_transport_fn g_xport = no_transport;
#endif

void nvrm387_set_transport(nvrm387_transport_fn fn)
{
#ifndef NVRM387_MOCK
    g_xport = fn ? fn : nvdauser_escape;
#else
    g_xport = fn ? fn : no_transport;
#endif
}

/* ---------------------------------------------------------------------------------------- */
/* Estado: objetos, fds, VA emulado                                                          */
/* ---------------------------------------------------------------------------------------- */
enum kind { K_FREE = 0, K_CLIENT, K_DEVICE, K_SUBDEV, K_VASPACE, K_VIRT, K_MEM, K_CTXDMA,
            K_CHANNEL, K_ENGINE, K_OTHER, K_TOMB };

#define VIRT_MAPS 4
struct obj {
    NvHandle hClient, h, parent;
    NvU32 cls;
    uint8_t kind;
    uint8_t emulated;      /* existe so na lib (VASPACE/VIRT) */
    NvU64 va, size;        /* VIRT: faixa reservada */
    NvHandle hDevice;      /* VIRT/CHANNEL: device dono do VA */
    uint8_t nmap;          /* VIRT emulado: mapas feitos nele, desfeitos no FREE (a r615 faz isso sozinha) */
    NvHandle mapMem[VIRT_MAPS];
    NvU64 mapVa[VIRT_MAPS];
};

#define MAXOBJ 65536u      /* potencia de 2 */
static struct obj g_obj[MAXOBJ];
static int g_nobj;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hkey(NvHandle c, NvHandle h)
{
    uint32_t x = c * 0x9E3779B1u ^ h * 0x85EBCA77u;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
    return x & (MAXOBJ - 1);
}
static struct obj *obj_find(NvHandle c, NvHandle h)
{
    for (uint32_t i = hkey(c, h), n = 0; n < MAXOBJ; i = (i + 1) & (MAXOBJ - 1), n++) {
        struct obj *o = &g_obj[i];
        if (o->kind == K_FREE) return NULL;
        if (o->kind != K_TOMB && o->hClient == c && o->h == h) return o;
    }
    return NULL;
}
static struct obj *obj_add(NvHandle c, NvHandle h, NvHandle parent, NvU32 cls, enum kind k)
{
    struct obj *o = obj_find(c, h);
    if (!o) {
        for (uint32_t i = hkey(c, h), n = 0; n < MAXOBJ; i = (i + 1) & (MAXOBJ - 1), n++) {
            if (g_obj[i].kind == K_FREE || g_obj[i].kind == K_TOMB) { o = &g_obj[i]; g_nobj++; break; }
        }
        if (!o) { WARN("tabela de objetos cheia\n"); return NULL; }
    }
    memset(o, 0, sizeof(*o));
    o->hClient = c; o->h = h; o->parent = parent; o->cls = cls; o->kind = (uint8_t)k;
    return o;
}
/* Um TOMB so precisa existir enquanto a posicao seguinte estiver ocupada: no fim de uma sequencia ele volta a FREE (e
 * os TOMBs logo antes dele tambem). Sem isto, depois de ~65 mil objetos criados e soltos (36 min de video com o
 * WindowServer reimportando a superficie a cada quadro) nao sobra FREE e toda busca que falha varre a tabela inteira. */
static void obj_del(struct obj *o)
{
    if (!o || o->kind == K_FREE || o->kind == K_TOMB) return;
    o->kind = K_TOMB; g_nobj--;
    uint32_t i = (uint32_t)(o - g_obj);
    if (g_obj[(i + 1) & (MAXOBJ - 1)].kind != K_FREE) return;
    for (uint32_t n = 0; n < MAXOBJ && g_obj[i].kind == K_TOMB; i = (i - 1) & (MAXOBJ - 1), n++) g_obj[i].kind = K_FREE;
}

int nvrm387_object_count(void) { return g_nobj; }

/* fds: o ponteiro do ultimo MAP_MEMORY feito com aquele fd */
#define MAXFD 32768
static struct { void *ptr; size_t len; } g_fd[MAXFD];

/* VA emulado por device: NV01_MEMORY_VIRTUAL (0x70) cobrindo o VA + ctxdma sobre ele */
#define MAXDEV 16
#define MAXRANGE 8192
struct range { NvU64 base, end; };          /* livre: [base, end) */
struct devva {
    NvHandle hClient, hDevice;
    NvHandle hVirtAll, hCtxAll;
    int ready;
    struct range fr[MAXRANGE];
    int nfr;
};
static struct devva g_dev[MAXDEV];

static NvU32 g_internal_seq;
/* 24 bits de sequencia (eram 16: com uma importacao por quadro a faixa dava a volta em ~35 min e o handle novo caia em
 * cima de um objeto vivo). Faixas 0xD4xxxxxx (internos) e 0xD5xxxxxx (dups pedidos com handle 0). */
static NvHandle internal_handle(void) { g_internal_seq = (g_internal_seq + 1) & 0xFFFFFFu; if (!g_internal_seq) g_internal_seq = 1; return 0xD4000000u | g_internal_seq; }
static NvU32 g_auto_seq;
static NvHandle auto_handle(void) { g_auto_seq = (g_auto_seq + 1) & 0xFFFFFFu; if (!g_auto_seq) g_auto_seq = 1; return 0xD5000000u | g_auto_seq; }

void nvrm387_reset_state(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_obj, 0, sizeof(g_obj));
    memset(g_fd, 0, sizeof(g_fd));
    memset(g_dev, 0, sizeof(g_dev));
    g_nobj = 0; g_internal_seq = 0; g_auto_seq = 0;
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------------------------------- */
/* Escapes r387 basicos                                                                      */
/* ---------------------------------------------------------------------------------------- */
static NvU32 x_status(int err, NvU32 st) { return err ? NV_ERR_GENERIC : st; }

static NvU32 r387_alloc(NvHandle c, NvHandle parent, NvHandle h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS_R387 a = { .hRoot = c, .hObjectParent = parent, .hObjectNew = h, .hClass = cls,
                                 .pAllocParms = (NvU64)(uintptr_t)p, .status = NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_ALLOC, &a, sizeof(a));
    return x_status(e, a.status);
}
static NvU32 r387_free(NvHandle c, NvHandle parent, NvHandle h)
{
    NVOS00_PARAMETERS_R387 f = { .hRoot = c, .hObjectParent = parent, .hObjectOld = h, .status = NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_FREE, &f, sizeof(f));
    return x_status(e, f.status);
}
static NvU32 r387_control(NvHandle c, NvHandle o, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS_R387 k = { .hClient = c, .hObject = o, .cmd = cmd, .params = (NvU64)(uintptr_t)p,
                                 .paramsSize = size, .status = NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_CONTROL, &k, sizeof(k));
    return x_status(e, k.status);
}
static NvU32 r387_alloc_memory(NvHandle c, NvHandle parent, NvHandle h, NvU32 cls, NvU32 flags,
                               NvU64 size, NvU64 *limitOut)
{
    NVOS02_PARAMETERS_R387 m = { .hRoot = c, .hObjectParent = parent, .hObjectNew = h, .hClass = cls,
                                 .flags = flags, .pMemory = 0, .limit = size - 1, .status = NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_ALLOC_MEMORY, &m, sizeof(m));
    if (!e && limitOut) *limitOut = m.limit;
    return x_status(e, m.status);
}
static NvU32 r387_alloc_virtual_kind(NvHandle c, NvHandle hDevice, NvHandle h,
                                     NV_MEMORY_ALLOCATION_PARAMS *p)
{
    NVOS32_ALLOC_R387 v;
    memset(&v, 0, sizeof(v));
    v.hRoot = c; v.hObjectParent = hDevice; v.function = NVOS32_FUNCTION_ALLOC_SIZE;
    v.owner = 0x6d6d7661; v.hMemory = h; v.type = p->type;
    v.flags = (p->flags & (NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE |
                           NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE)) |
              NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED |
              NVOS32_ALLOC_FLAGS_NO_SCANOUT | NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
              NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED;
    v.attr = p->attr; v.format = p->format; v.comprCovg = p->comprCovg;
    v.width = p->width; v.height = p->height;
    v.size = p->size; v.alignment = p->alignment ? p->alignment : 0x20000;
    v.offset = p->offset; v.attr2 = p->attr2; v.ctagOffset = p->ctagOffset;
    if (p->flags & NVOS32_ALLOC_FLAGS_USE_BEGIN_END) {
        v.flags |= NVOS32_ALLOC_FLAGS_USE_BEGIN_END;
        v.rangeBegin = p->rangeLo; v.rangeEnd = p->rangeHi;
    }
    v.status = NV_ERR_GENERIC;
    int e = g_xport(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v));
    NvU32 st = x_status(e, v.status);
    if (st) return st;
    if (v.hMemory != h) {
        r387_free(c, hDevice, v.hMemory);
        return NV_ERR_INVALID_OBJECT_HANDLE;
    }
    p->attr = v.attr; p->attr2 = v.attr2; p->format = v.format;
    p->comprCovg = v.comprCovg; p->zcullCovg = v.zcullCovg;
    p->size = v.size; p->offset = v.offset; p->limit = v.offset + v.limit;
    memcpy(&p->address, &v.address, sizeof(p->address));
    return NV_OK;
}
static NvU32 r387_ctxdma(NvHandle c, NvHandle h, NvU32 flags, NvHandle hMem, NvU64 off, NvU64 limit)
{
    NVOS39_PARAMETERS_R387 d = { .hObjectParent = c, .hObjectNew = h, .hClass = CL_CONTEXT_DMA, .flags = flags,
                                 .hMemory = hMem, .offset = off, .limit = limit, .status = NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_ALLOC_CONTEXT_DMA2, &d, sizeof(d));
    return x_status(e, d.status);
}
static NvU32 r387_bind_ctxdma(NvHandle c, NvHandle hChannel, NvHandle hCtx)
{
    NvU32 b[4] = { c, hChannel, hCtx, NV_ERR_GENERIC };
    int e = g_xport(NV387_ESC_RM_BIND_CONTEXT_DMA, b, sizeof(b));
    return x_status(e, b[3]);
}

/* ---------------------------------------------------------------------------------------- */
/* VA emulado                                                                                */
/* ---------------------------------------------------------------------------------------- */
static struct devva *devva_get(NvHandle c, NvHandle hDevice, int create)
{
    struct devva *freeSlot = NULL;
    for (int i = 0; i < MAXDEV; i++) {
        if (g_dev[i].hClient == c && g_dev[i].hDevice == hDevice && (g_dev[i].hClient || g_dev[i].hDevice))
            return &g_dev[i];
        if (!g_dev[i].hClient && !g_dev[i].hDevice && !freeSlot) freeSlot = &g_dev[i];
    }
    if (!create || !freeSlot) return NULL;
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->hClient = c; freeSlot->hDevice = hDevice;
    freeSlot->fr[0].base = NVRM387_VA_DEFAULT_BASE;
    freeSlot->fr[0].end = NVRM387_VA_DEFAULT_END;
    freeSlot->nfr = 1;
    return freeSlot;
}

/* Cria o NV01_MEMORY_VIRTUAL 0x70 que cobre o VA do device e o ctxdma sobre ele */
static NvU32 devva_ready(struct devva *d)
{
    if (d->ready) return NV_OK;
    NvHandle hv = internal_handle(), hc = internal_handle();
    NvU64 limit = 0;
    NvU32 st = r387_alloc_memory(d->hClient, d->hDevice, hv, CL_MEM_VIRTUAL_R387, MEM_FLAGS_SYS_WC,
                                 NVRM387_VA_DEFAULT_END, &limit);
    if (st) { WARN("VA: ALLOC_MEMORY 0x70 -> 0x%x\n", st); return st; }
    st = r387_ctxdma(d->hClient, hc, CTXDMA_FLAGS_DEFAULT, hv, 0, limit);
    if (st) { WARN("VA: ctxdma -> 0x%x\n", st); r387_free(d->hClient, d->hDevice, hv); return st; }
    d->hVirtAll = hv; d->hCtxAll = hc; d->ready = 1;
    LOG("VA do device 0x%x: virtual 0x%x ctxdma 0x%x limite 0x%llx\n", d->hDevice, hv, hc,
        (unsigned long long)limit);
    return NV_OK;
}

static int va_alloc(struct devva *d, NvU64 size, NvU64 align, NvU64 fixed, NvU64 *out)
{
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xFFF) & ~0xFFFull;
    for (int i = 0; i < d->nfr; i++) {
        NvU64 a = fixed ? fixed : (d->fr[i].base + align - 1) & ~(align - 1);
        if (a < d->fr[i].base || a + size > d->fr[i].end) continue;
        /* parte a faixa livre em [base,a) e [a+size,end) */
        NvU64 b0 = d->fr[i].base, e0 = d->fr[i].end;
        int idx = i;
        if (a > b0) { d->fr[idx].end = a; idx++; }
        else { memmove(&d->fr[i], &d->fr[i + 1], (size_t)(d->nfr - i - 1) * sizeof(struct range)); d->nfr--; }
        if (a + size < e0) {
            if (d->nfr >= MAXRANGE) return -1;
            memmove(&d->fr[idx + 1], &d->fr[idx], (size_t)(d->nfr - idx) * sizeof(struct range));
            d->fr[idx].base = a + size; d->fr[idx].end = e0; d->nfr++;
        }
        *out = a;
        return 0;
    }
    return -1;
}
static void va_free(struct devva *d, NvU64 base, NvU64 size)
{
    size = (size + 0xFFF) & ~0xFFFull;
    NvU64 end = base + size;
    int i = 0;
    while (i < d->nfr && d->fr[i].base < base) i++;
    if (d->nfr >= MAXRANGE) return;
    memmove(&d->fr[i + 1], &d->fr[i], (size_t)(d->nfr - i) * sizeof(struct range));
    d->fr[i].base = base; d->fr[i].end = end; d->nfr++;
    if (i + 1 < d->nfr && d->fr[i].end == d->fr[i + 1].base) {          /* junta com a seguinte */
        d->fr[i].end = d->fr[i + 1].end;
        memmove(&d->fr[i + 1], &d->fr[i + 2], (size_t)(d->nfr - i - 2) * sizeof(struct range)); d->nfr--;
    }
    if (i > 0 && d->fr[i - 1].end == d->fr[i].base) {                  /* junta com a anterior */
        d->fr[i - 1].end = d->fr[i].end;
        memmove(&d->fr[i], &d->fr[i + 1], (size_t)(d->nfr - i - 1) * sizeof(struct range)); d->nfr--;
    }
}

/* device dono de um objeto (sobe pela arvore) */
static NvHandle device_of(NvHandle c, NvHandle h)
{
    for (int depth = 0; depth < 16 && h; depth++) {
        struct obj *o = obj_find(c, h);
        if (!o) return 0;
        if (o->kind == K_DEVICE) return o->h;
        h = o->parent;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* FREE                                                                                      */
/* ---------------------------------------------------------------------------------------- */
static void drop_emulated(struct obj *o)
{
    if (o->kind == K_VIRT && o->emulated) {
        struct devva *d = devva_get(o->hClient, o->hDevice, 0);
        if (d && d->ready) {
            for (int i = 0; i < o->nmap; i++) {
                NVOS47_PARAMETERS_R387 u;
                memset(&u, 0, sizeof(u));
                u.hClient = o->hClient; u.hDevice = o->hDevice; u.hDma = d->hCtxAll;
                u.hMemory = o->mapMem[i]; u.dmaOffset = o->mapVa[i]; u.status = NV_ERR_GENERIC;
                int e = g_xport(NV387_ESC_RM_UNMAP_MEMORY_DMA, &u, sizeof(u));
                LOG("FREE virtual 0x%x: desfaz mapa mem 0x%x VA 0x%llx status 0x%x\n", o->h, o->mapMem[i],
                    (unsigned long long)o->mapVa[i], x_status(e, u.status));
            }
        }
        o->nmap = 0;
        if (d) va_free(d, o->va, o->size);
    }
}

/* remove da tabela h e todos os descendentes (o RM libera em cascata) */
static void purge_tree(NvHandle c, NvHandle h)
{
    static NvHandle work[MAXOBJ];
    int n = 0;
    struct obj *root = obj_find(c, h);
    const uint8_t rk = root ? root->kind : K_FREE;
    if (root) { drop_emulated(root); obj_del(root); }
    /* memoria, dup de memoria, ctxdma e reserva de VA nao tem filhos na tabela: sem a varredura de 65536 entradas a cada
     * FREE (era ~6% de um nucleo do WindowServer, que solta uma importacao por quadro) */
    if (rk == K_MEM || rk == K_OTHER || rk == K_CTXDMA || rk == K_VIRT) return;
    work[n++] = h;
    while (n) {
        NvHandle cur = work[--n];
        for (uint32_t i = 0; i < MAXOBJ; i++) {
            struct obj *o = &g_obj[i];
            if (o->kind == K_FREE || o->kind == K_TOMB || o->hClient != c || o->parent != cur) continue;
            if (n < (int)MAXOBJ) work[n++] = o->h;
            drop_emulated(o);
            obj_del(o);
        }
    }
}

static void free_client_state(NvHandle c)
{
    for (uint32_t i = 0; i < MAXOBJ; i++)
        if (g_obj[i].kind != K_FREE && g_obj[i].kind != K_TOMB && g_obj[i].hClient == c) obj_del(&g_obj[i]);
    for (int i = 0; i < MAXDEV; i++)
        if (g_dev[i].hClient == c) memset(&g_dev[i], 0, sizeof(g_dev[i]));
}

static NvU32 do_free(NvHandle c, NvHandle parentHint, NvHandle h)
{
    struct obj *o = obj_find(c, h);
    if (h == c || (o && o->kind == K_CLIENT)) {
        NvU32 st = r387_free(c, c, c);
        free_client_state(c);
        return st;
    }
    if (o && o->emulated) {
        drop_emulated(o);
        obj_del(o);
        return NV_OK;
    }
    NvHandle parent = parentHint ? parentHint : (o ? o->parent : 0);
    if (!parent) { LOG("FREE 0x%x: pai desconhecido, usando o cliente\n", h); parent = c; }
    if (o && o->kind == K_DEVICE) {          /* o VA interno usa ctxdma pendurado no cliente */
        struct devva *d = devva_get(c, h, 0);
        if (d && d->ready) { r387_free(c, c, d->hCtxAll); }
        if (d) memset(d, 0, sizeof(*d));
    }
    NvU32 st = r387_free(c, parent, h);
    if (st == NV_OK) purge_tree(c, h);
    return st;
}

/* ---------------------------------------------------------------------------------------- */
/* ALLOC (por classe)                                                                        */
/* ---------------------------------------------------------------------------------------- */
/* VRAM fisica: NVOS32 ALLOC_SIZE sem VIRTUAL (como o AllocPageLevel do GeForceWeb).
 * attr do NVK com localizacao VIDMEM e coerencia write-combine; kind/pagina/fisicalidade passam. */
#define ATTR_LOCATION_MASK  (3u << 25)
#define ATTR_COHERENCY_MASK (7u << 29)
#define ATTR_COHERENCY_WC   (2u << 29)
static NvU32 r387_alloc_vidmem(NvHandle c, NvHandle hDevice, NvHandle h, NV_MEMORY_ALLOCATION_PARAMS *p)
{
    NVOS32_ALLOC_R387 v;
    memset(&v, 0, sizeof(v));
    v.hRoot = c; v.hObjectParent = hDevice; v.function = NVOS32_FUNCTION_ALLOC_SIZE;
    v.owner = 0x6e766b20; v.hMemory = h; v.type = p->type;
    v.flags = (p->flags & (NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE)) |
              NVOS32_ALLOC_FLAGS_NO_SCANOUT | NVOS32_ALLOC_FLAGS_IGNORE_BANK_PLACEMENT |
              NVOS32_ALLOC_FLAGS_MEMORY_HANDLE_PROVIDED;
    v.attr = (p->attr & ~(ATTR_LOCATION_MASK | ATTR_COHERENCY_MASK)) | ATTR_COHERENCY_WC;
    /* pagina de 4 KiB: com pagina grande (128 KiB) o RM arredonda a VRAM e recusa (0x51) mapas menores que
     * 128 KiB ou em offsets nao alinhados, que o NVK faz (texturas pequenas, subfaixas de heap) */
    if (!env_on("NVRM387_VRAM_BIGPAGE"))
        v.attr = (v.attr & ~ATTR_PAGE_SIZE_MASK) | ATTR_PAGE_SIZE_4KB;
    v.attr2 = p->attr2; v.format = p->format; v.comprCovg = p->comprCovg;
    v.size = (p->size + 0xFFF) & ~0xFFFull; v.alignment = p->alignment; v.offset = p->offset;
    v.status = NV_ERR_GENERIC;
    int e = g_xport(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v));
    NvU32 st = x_status(e, v.status);
    if (st) return st;
    if (v.hMemory != h) { r387_free(c, hDevice, v.hMemory); return NV_ERR_INVALID_OBJECT_HANDLE; }
    p->attr = v.attr; p->attr2 = v.attr2; p->format = v.format;
    p->size = v.size; p->offset = v.offset; p->limit = v.offset + v.limit;
    return NV_OK;
}

static NvU32 alloc_sysmem(NvHandle c, NvHandle parent, NvHandle h, NvU32 cls, NV_MEMORY_ALLOCATION_PARAMS *p)
{
    static int toldVram;
    if (!p || !p->size) return NV_ERR_INVALID_ARGUMENT;
    if (cls == CL_MEM_LOCAL_USER && !env_on("NVRM387_VRAM_SYSMEM")) {
        NvHandle hDevice = device_of(c, parent);
        NvU32 st = hDevice ? r387_alloc_vidmem(c, hDevice, h, p) : NV_ERR_INVALID_OBJECT_PARENT;
        if (st == NV_OK) {
            struct obj *o = obj_add(c, h, parent, cls, K_MEM);
            if (o) o->size = p->size;
            LOG("VRAM 0x%x: offset 0x%llx +0x%llx attr 0x%08x kind 0x%x\n", h, (unsigned long long)p->offset,
                (unsigned long long)p->size, p->attr, p->format);
            return NV_OK;
        }
        if (env_on("NVRM387_NO_VRAM_FALLBACK")) return st;
        if (!toldVram) { toldVram = 1; WARN("VRAM recusada (0x%x): caindo para memoria de sistema WC\n", st); }
    }
    NvU64 size = (p->size + 0xFFF) & ~0xFFFull, limit = 0;
    NvU32 flags = env_on("NVRM387_SYSMEM_CACHED") ? MEM_FLAGS_SYS_CACHED : MEM_FLAGS_SYS_WC;
    NvU32 st = r387_alloc_memory(c, parent, h, CL_MEM_SYSTEM, flags, size, &limit);
    if (st) return st;
    p->size = size;
    p->limit = limit;
    p->offset = 0;
    struct obj *o = obj_add(c, h, parent, cls, K_MEM);
    if (o) o->size = size;
    return NV_OK;
}

static NvU32 alloc_virtual(NvHandle c, NvHandle parent, NvHandle h, NV_MEMORY_ALLOCATION_PARAMS *p)
{
    if (!p || !p->size) return NV_ERR_INVALID_ARGUMENT;
    NvHandle hDevice = device_of(c, parent);
    if (!hDevice) return NV_ERR_INVALID_OBJECT_PARENT;
    if (p->format != 0) {   /* kind nativo: a r387 deriva o kind de type + attr (Z/BL); format so confere */
        NvU32 st = r387_alloc_virtual_kind(c, hDevice, h, p);
        if (st) return st;
        struct obj *o = obj_add(c, h, parent, CL_MEM_VIRTUAL, K_VIRT);
        if (!o) { r387_free(c, hDevice, h); return NV_ERR_INSUFFICIENT_RESOURCES; }
        o->va = p->offset; o->size = p->size; o->hDevice = hDevice;
        LOG("NVOS32 VIRTUAL kind 0x%x 0x%x: VA 0x%llx +0x%llx\n", p->format, h,
            (unsigned long long)p->offset, (unsigned long long)p->size);
        return NV_OK;
    }
    struct devva *d = devva_get(c, hDevice, 1);
    if (!d) return NV_ERR_INSUFFICIENT_RESOURCES;
    NvU32 st = devva_ready(d);
    if (st) return st;
    NvU64 fixed = (p->flags & NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE) ? p->offset : 0;
    NvU64 align = (p->flags & NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE) ? p->alignment : 0;
    if (align < MAXWELL_BIG_PAGE) align = MAXWELL_BIG_PAGE;   /* VRAM com pagina grande exige VA alinhado */
    NvU64 va = 0, size = (p->size + 0xFFF) & ~0xFFFull;
    if (va_alloc(d, size, align, fixed, &va)) return NV_ERR_NO_MEMORY;
    struct obj *o = obj_add(c, h, parent, CL_MEM_VIRTUAL, K_VIRT);
    if (!o) { va_free(d, va, size); return NV_ERR_INSUFFICIENT_RESOURCES; }
    o->emulated = 1; o->va = va; o->size = size; o->hDevice = hDevice;
    p->offset = va;
    p->size = size;
    p->limit = va + size - 1;
    LOG("NV50_MEMORY_VIRTUAL 0x%x: VA 0x%llx +0x%llx\n", h, (unsigned long long)va, (unsigned long long)size);
    return NV_OK;
}

static NvU32 alloc_channel(NvHandle c, NvHandle parent, NvHandle h, NvU32 cls, NV_CHANNEL_ALLOC_PARAMS *p)
{
    static int toldUserd;
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    if (p->hContextShare) return NV_ERR_NOT_SUPPORTED;      /* TSG/subcontexto: nao na r387 */
    NvHandle hDevice = device_of(c, parent);
    if (!hDevice) return NV_ERR_INVALID_OBJECT_PARENT;
    struct devva *d = devva_get(c, hDevice, 1);
    if (!d) return NV_ERR_INSUFFICIENT_RESOURCES;
    NvU32 st = devva_ready(d);
    if (st) return st;
    if (p->hUserdMemory[0] && !toldUserd) {
        toldUserd = 1;
        WARN("hUserdMemory ignorado: na r387 a USERD vem de MAP_MEMORY do proprio canal (GPPut em +0x8C)\n");
    }
    /* params r387: prefixo igual a r615, resto zerado com folga (ANALISE_RM.md 7d) */
    NvU32 cp[64];
    memset(cp, 0, sizeof(cp));
    cp[0] = p->hObjectError;
    cp[1] = d->hCtxAll;                                      /* hObjectBuffer = ctxdma do VA */
    memcpy(&cp[2], &p->gpFifoOffset, sizeof(NvU64));
    cp[4] = p->gpFifoEntries;
    cp[5] = 0;                                               /* flags: sem PRIVILEGED */
    st = r387_alloc(c, parent, h, cls, cp);
    if (st) return st;
    st = r387_bind_ctxdma(c, h, d->hCtxAll);
    if (st) { WARN("canal 0x%x: BIND_CONTEXT_DMA -> 0x%x\n", h, st); r387_free(c, parent, h); return st; }
    struct obj *o = obj_add(c, h, parent, cls, K_CHANNEL);
    if (o) o->hDevice = hDevice;
    return NV_OK;
}

static NvU32 alloc_ctxdma_r615(NvHandle c, NvHandle parent, NvHandle h, NV_CONTEXT_DMA_ALLOCATION_PARAMS *p)
{
    if (!p) return NV_ERR_INVALID_ARGUMENT;
    NvU32 st = r387_ctxdma(c, h, p->flags, p->hMemory, p->offset, p->limit);
    if (st) return st;
    (void)parent;
    obj_add(c, h, c, CL_CONTEXT_DMA, K_CTXDMA);              /* na r387 o pai e o cliente */
    return NV_OK;
}


/* Copy engine (0xB0B5) num canal de graficos: o RM r387 so aceita a copy engine parceira do
 * GR (GRCE); com params NULL da INVALID_STATE (0x40). VERSION_0 = numero da instancia do CE.
 * Tenta a pedida pelo NVK (VERSION_1 COPYn ou VERSION_0 n) e depois 2, 1, 0. */
#define ENGINE_TYPE_COPY0 0x09u
static NvU32 alloc_copy(NvHandle c, NvHandle parent, NvHandle h, NvU32 cls, const NvU32 *p615)
{
    NvU32 st = r387_alloc(c, parent, h, cls, NULL);
    if (st == NV_OK) return st;
    int order[4], n = 0;
    if (p615) {
        if (p615[0] == 1 && p615[1] >= ENGINE_TYPE_COPY0 && p615[1] < ENGINE_TYPE_COPY0 + 10)
            order[n++] = (int)(p615[1] - ENGINE_TYPE_COPY0);
        else if (p615[0] == 0 && p615[1] < 10)
            order[n++] = (int)p615[1];
    }
    for (int inst = 2; inst >= 0; inst--) {
        int dup = 0;
        for (int i = 0; i < n; i++) if (order[i] == inst) dup = 1;
        if (!dup) order[n++] = inst;
    }
    NvU32 first = st;
    for (int i = 0; i < n; i++) {
        NvU32 q[16];
        memset(q, 0, sizeof(q));
        q[0] = 0;                       /* VERSION_0 */
        q[1] = (NvU32)order[i];         /* instancia do CE */
        st = r387_alloc(c, parent, h, cls, q);
        LOG("0x%04x VERSION_0 instancia %d -> 0x%x\n", cls, order[i], st);
        if (st == NV_OK) return st;
    }
    return first;
}

static int is_engine_class(NvU32 cls)
{
    switch (cls) {
    case 0xB097: case 0xB197:            /* MAXWELL_A/B */
    case 0xB0C0: case 0xB1C0:            /* MAXWELL_COMPUTE_A/B */
    case 0xB0B5:                         /* MAXWELL_DMA_COPY_A */
    case 0x902D:                         /* FERMI_TWOD_A */
    case 0xA140:                         /* KEPLER_INLINE_TO_MEMORY_B */
        return 1;
    }
    return 0;
}

static NvU32 do_alloc(NvHandle c, NvHandle parent, NvHandle *ph, NvU32 cls, void *params)
{
    NvHandle h = *ph;

    if (cls == CL_ROOT || cls == CL_ROOT_CLIENT) {
        NvHandle hc = 0;
        NvU32 st = r387_alloc(0, 0, 0, CL_ROOT, &hc);
        if (st == NV_OK) {
            if (h && h != hc) LOG("cliente pedido 0x%x, RM deu 0x%x\n", h, hc);
            *ph = hc;
            obj_add(hc, hc, 0, CL_ROOT_CLIENT, K_CLIENT);
        }
        return st;
    }
    if (!h) h = *ph = auto_handle();

    switch (cls) {
    case CL_DEVICE: {
        NV0080_ALLOC_PARAMETERS_R387 dp;
        memset(&dp, 0, sizeof(dp));
        if (params) {
            const NV0080_ALLOC_PARAMETERS *p = params;
            dp.deviceId = p->deviceId;
            dp.hClientShare = p->hClientShare;
        }
        NvU8 buf[0x80];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, &dp, sizeof(dp));
        NvU32 st = r387_alloc(c, parent, h, cls, buf);
        if (!st) obj_add(c, h, parent, cls, K_DEVICE);
        return st;
    }
    case CL_SUBDEVICE: {
        NvU32 sp[4] = { params ? *(const NvU32 *)params : 0, 0, 0, 0 };
        NvU32 st = r387_alloc(c, parent, h, cls, sp);
        if (!st) obj_add(c, h, parent, cls, K_SUBDEV);
        return st;
    }
    case CL_VASPACE: {
        /* a r387 usa o VA padrao do device; o VASPACE vira so um registro */
        struct obj *o = obj_add(c, h, parent, cls, K_VASPACE);
        if (!o) return NV_ERR_INSUFFICIENT_RESOURCES;
        o->emulated = 1;
        o->hDevice = device_of(c, parent);
        return NV_OK;
    }
    case CL_MEM_VIRTUAL:
        return alloc_virtual(c, parent, h, params);
    case CL_MEM_SYSTEM:
    case CL_MEM_LOCAL_USER:
        return alloc_sysmem(c, parent, h, cls, params);
    case CL_CONTEXT_DMA:
        return alloc_ctxdma_r615(c, parent, h, params);
    case CL_CHANNEL_B06F:
        return alloc_channel(c, parent, h, cls, params);
    case CL_TSG:
    case CL_CTXSHARE:
    case CL_EVENT:
    case CL_EVENT_OS_EVENT:
        LOG("classe 0x%04x nao suportada na r387 (NVK tem fallback)\n", cls);
        return NV_ERR_NOT_SUPPORTED;
    }
    if (cls == 0xB0B5u) {
        NvU32 st = alloc_copy(c, parent, h, cls, params);
        if (!st) obj_add(c, h, parent, cls, K_ENGINE);
        return st;
    }
    if (is_engine_class(cls)) {
        if (params) LOG("classe 0x%04x: params r615 ignorados (r387 aceita NULL)\n", cls);
        NvU32 st = r387_alloc(c, parent, h, cls, NULL);
        if (!st) obj_add(c, h, parent, cls, K_ENGINE);
        return st;
    }
    WARN("ALLOC classe 0x%04x sem traducao: recusada\n", cls);
    return NV_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------------------- */
/* CONTROL                                                                                   */
/* ---------------------------------------------------------------------------------------- */
typedef struct { NvU32 numClasses; NvU32 classList[200]; } CLASSLIST_V2_R615;
typedef struct { NvU32 gpuId, gpuFlags, deviceInstance, subDeviceInstance, sliStatus, boardId, gpuInstance;
                 NvS32 numaId; } ID_INFO_V2_R615;
typedef struct { NvU32 gpuId, gpuFlags, deviceInstance, subDeviceInstance; NvU64 szName;
                 NvU32 sliStatus, boardId, gpuInstance, numaId; } ID_INFO_R387;
typedef struct { NvU32 numClasses, _pad; NvU64 classList; } CLASSLIST_R387;
typedef struct { NvU32 index, data; } INFO_ENTRY;
typedef struct { NvU32 listSize, _pad; NvU64 list; } INFO_LIST_R387;      /* GR/FB_GET_INFO por ponteiro */
typedef struct { NvU32 fbInfoListSize; INFO_ENTRY fbInfoList[128]; } FB_GET_INFO_V2_R615;


/* GR_GET_INFO da r387: no subdevice, um indice por vez. Na faixa que o NVK usa a numeracao
 * da r387 e a da r615 menos 3 (confirmado no hackintosh: SM_VERSION r615 0x0C = r387 0x09 = 0x502,
 * GPU_CORE_COUNT r615 0x1D = r387 0x1A = 1024). */
#define GR_IDX_R615_FIRST 0x0Cu
#define GR_IDX_R615_LAST  0x20u
#define GR_IDX_SHIFT      3u
static int gr_idx_r387(NvU32 r615, NvU32 *out)
{
    if (r615 < GR_IDX_R615_FIRST || r615 > GR_IDX_R615_LAST) return 0;
    *out = r615 - GR_IDX_SHIFT;
    return 1;
}
static NvU32 r387_gr_info(NvHandle c, NvHandle hSub, NvU32 idx387, NvU32 *data)
{
    INFO_ENTRY e = { idx387, 0 };
    INFO_LIST_R387 q = { 1, 0, (NvU64)(uintptr_t)&e };
    NvU32 st = r387_control(c, hSub, CTRL_GR_GET_INFO_2080, &q, sizeof(q));
    if (st == NV_OK) *data = e.data;
    return st;
}
static NvHandle subdev_for(NvHandle c, NvHandle o)
{
    struct obj *x = obj_find(c, o);
    if (x && x->kind == K_SUBDEV) return o;
    for (uint32_t i = 0; i < MAXOBJ; i++)
        if (g_obj[i].kind == K_SUBDEV && g_obj[i].hClient == c && g_obj[i].parent == o) return g_obj[i].h;
    return 0;
}
/* Topologia derivada (as mascaras GPC/TPC nao existem na r387). So Maxwell: 128 cores por SM. */
static NvU32 gr_topology(NvHandle c, NvHandle hSub, NvU32 *gpcs, NvU32 *tpcPerGpc, NvU32 *tpcs)
{
    NvU32 sm = 0, cores = 0, tpg = 0, smPerTpc = 0, st;
    if ((st = r387_gr_info(c, hSub, 0x0C - GR_IDX_SHIFT, &sm))) return st;
    if ((sm >> 8) != 5) return NV_ERR_NOT_SUPPORTED;
    if ((st = r387_gr_info(c, hSub, 0x1D - GR_IDX_SHIFT, &cores))) return st;
    if ((st = r387_gr_info(c, hSub, 0x17 - GR_IDX_SHIFT, &tpg))) return st;
    if ((st = r387_gr_info(c, hSub, 0x20 - GR_IDX_SHIFT, &smPerTpc))) return st;
    if (!cores || !tpg || !smPerTpc || tpg > 32) return NV_ERR_INVALID_STATE;
    NvU32 nTpc = cores / 128 / smPerTpc;
    *tpcPerGpc = tpg; *tpcs = nTpc;
    *gpcs = (nTpc + tpg - 1) / tpg;
    if (*gpcs == 0 || *gpcs > 32) return NV_ERR_INVALID_STATE;
    return NV_OK;
}

static int ctrl_whitelisted(NvU32 cmd)
{
    switch (cmd) {
    case CTRL_ATTACHED_IDS:
    case CTRL_ID_INFO:
    case CTRL_CLASSLIST:
    case CTRL_SHORT_NAME:
    case CTRL_MC_ARCH_INFO:       /* testados no hackintosh: mesmo layout */
    case CTRL_TIMER_GET_TIME:
    case CTRL_FIFO_ENG_CTX_PROP:
    case CTRL_ENGINE_PARTNERS:    /* r387 responde NOT_SUPPORTED; mesmo tamanho, sem risco */
        return 1;
    }
    return 0;
}

static NvU32 do_control(NvHandle c, NvHandle o, NvU32 cmd, void *p, NvU32 size)
{
    switch (cmd) {
    case CTRL_CLASSLIST_V2: {
        if (!p || size != sizeof(CLASSLIST_V2_R615)) return NV_ERR_INVALID_PARAM_STRUCT;
        CLASSLIST_V2_R615 *v = p;
        CLASSLIST_R387 q = { 0, 0, 0 };
        NvU32 st = r387_control(c, o, CTRL_CLASSLIST, &q, sizeof(q));
        if (st) return st;
        if (q.numClasses > 200) return NV_ERR_INSUFFICIENT_RESOURCES;
        NvU32 tmp[200 + 16];
        memset(tmp, 0, sizeof(tmp));
        q.classList = (NvU64)(uintptr_t)tmp;
        st = r387_control(c, o, CTRL_CLASSLIST, &q, sizeof(q));
        if (st) return st;
        v->numClasses = q.numClasses;
        memcpy(v->classList, tmp, q.numClasses * sizeof(NvU32));
        for (NvU32 i = 0; i < q.numClasses; i++) LOG("classe do RM [%u] 0x%04x\n", i, tmp[i]);
        return NV_OK;
    }
    case CTRL_ID_INFO_V2: {
        if (!p || size != sizeof(ID_INFO_V2_R615)) return NV_ERR_INVALID_PARAM_STRUCT;
        ID_INFO_V2_R615 *v = p;
        ID_INFO_R387 q;
        memset(&q, 0, sizeof(q));
        q.gpuId = v->gpuId;
        NvU32 st = r387_control(c, o, CTRL_ID_INFO, &q, sizeof(q));
        if (st) return st;
        v->gpuFlags = q.gpuFlags; v->deviceInstance = q.deviceInstance;
        v->subDeviceInstance = q.subDeviceInstance; v->sliStatus = q.sliStatus;
        v->boardId = q.boardId; v->gpuInstance = q.gpuInstance; v->numaId = (NvS32)q.numaId;
        return NV_OK;
    }
    case CTRL_GR_GET_INFO_V2: {
        typedef struct { NvU32 n; INFO_ENTRY l[0x3F]; NvU32 _pad; NvU32 rflags, _pad2; NvU64 route; } GR_V2_R615;
        _Static_assert(sizeof(GR_V2_R615) == 528, "GR_GET_INFO_V2 r615");
        if (!p || size != sizeof(GR_V2_R615)) return NV_ERR_INVALID_PARAM_STRUCT;
        GR_V2_R615 *v = p;
        if (v->n > 0x3F) return NV_ERR_INVALID_ARGUMENT;
        NvHandle hSub = subdev_for(c, o);
        if (!hSub) return NV_ERR_INVALID_OBJECT_HANDLE;
        for (NvU32 i = 0; i < v->n; i++) {
            NvU32 idx387, data = 0;
            if (!gr_idx_r387(v->l[i].index, &idx387) || r387_gr_info(c, hSub, idx387, &data) != NV_OK) {
                LOG("GR_GET_INFO_V2: indice r615 0x%x sem equivalente na r387 (0)\n", v->l[i].index);
                data = 0;
            }
            v->l[i].data = data;
        }
        return NV_OK;
    }
    case CTRL_GR_GPC_MASK:
    case CTRL_GR_TPC_MASK: {
        /* r615: grRouteInfo (16) + gpcMask | gpcId, tpcMask */
        if (!p || size != 24) return NV_ERR_INVALID_PARAM_STRUCT;
        NvHandle hSub = subdev_for(c, o);
        if (!hSub) return NV_ERR_INVALID_OBJECT_HANDLE;
        NvU32 gpcs, tpg, tpcs;
        NvU32 st = gr_topology(c, hSub, &gpcs, &tpg, &tpcs);
        if (st) return st;
        NvU32 *f = (NvU32 *)((NvU8 *)p + 16);
        if (cmd == CTRL_GR_GPC_MASK) {
            f[0] = (gpcs >= 32) ? 0xFFFFFFFFu : ((1u << gpcs) - 1);
        } else {
            NvU32 g = f[0];
            NvU32 n = (g < gpcs) ? ((g + 1 < gpcs || tpcs % tpg == 0) ? tpg : tpcs % tpg) : 0;
            f[1] = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1);
        }
        return NV_OK;
    }
    case CTRL_FB_GET_INFO_V2: {
        /* r387: FB_GET_INFO 0x20801301 {n, ponteiro}; indices iguais aos da r615 */
        if (!p || size != sizeof(FB_GET_INFO_V2_R615)) return NV_ERR_INVALID_PARAM_STRUCT;
        FB_GET_INFO_V2_R615 *v = p;
        if (v->fbInfoListSize == 0 || v->fbInfoListSize > 128) return NV_ERR_INVALID_ARGUMENT;
        INFO_LIST_R387 q = { v->fbInfoListSize, 0, (NvU64)(uintptr_t)v->fbInfoList };
        return r387_control(c, o, CTRL_FB_GET_INFO, &q, sizeof(q));
    }
    case CTRL_A06F_SCHEDULE: {
        if (!p || size < 1) return NV_ERR_INVALID_PARAM_STRUCT;
        NvU8 en = ((const NvU8 *)p)[0] ? 1 : 0;              /* r387: so bEnable (1 byte) */
        NvU8 q[8] = { en, 0, 0, 0, 0, 0, 0, 0 };
        return r387_control(c, o, CTRL_A06F_SCHEDULE, q, 1);
    }
    case CTRL_NAME_STRING: {
        /* r615: flags + 64 bytes (68); r387: flags + 128 bytes (0x84) */
        if (!p || size != 4 + 64) return NV_ERR_INVALID_PARAM_STRUCT;
        NvU8 q[4 + 128];
        memset(q, 0, sizeof(q));
        memcpy(q, p, 4);
        NvU32 st = r387_control(c, o, CTRL_NAME_STRING, q, sizeof(q));
        if (st) return st;
        memcpy(p, q, 4 + 64);
        ((NvU8 *)p)[4 + 63] = 0;
        return NV_OK;
    }
    case CTRL_A06F_BIND:
        return NV_OK;                                        /* r387 liga a engine pelo objeto */
    case CTRL_C36F_GET_TOKEN:
    case CTRL_C36F_TOKEN_NOTIF:
        return NV_ERR_NOT_SUPPORTED;                         /* sem doorbell no Maxwell */
    }
    if (ctrl_whitelisted(cmd) || env_on("NVRM387_CTRL_PASSTHROUGH"))
        return r387_control(c, o, cmd, p, size);
    static NvU32 told[64]; static int ntold;
    int seen = 0;
    for (int i = 0; i < ntold; i++) if (told[i] == cmd) seen = 1;
    if (!seen) {
        if (ntold < 64) told[ntold++] = cmd;
        WARN("CONTROL 0x%08x sem traducao: recusado (NVRM387_CTRL_PASSTHROUGH=1 para mandar direto)\n", cmd);
    }
    return NV_ERR_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------------------- */
/* Escapes r615 -> r387                                                                      */
/* ---------------------------------------------------------------------------------------- */
static int esc_alloc(void *data, uint32_t size)
{
    NvHandle *hRoot, *hParent, *hNew; NvU32 *hClass; void *pParams; NvV32 *status;
    if (size == sizeof(NVOS21_PARAMETERS)) {
        NVOS21_PARAMETERS *a = data;
        hRoot = &a->hRoot; hParent = &a->hObjectParent; hNew = &a->hObjectNew; hClass = &a->hClass;
        memcpy(&pParams, &a->pAllocParms, sizeof(void *)); status = &a->status;
    } else if (size == sizeof(NVOS64_PARAMETERS)) {
        NVOS64_PARAMETERS *a = data;
        hRoot = &a->hRoot; hParent = &a->hObjectParent; hNew = &a->hObjectNew; hClass = &a->hClass;
        memcpy(&pParams, &a->pAllocParms, sizeof(void *)); status = &a->status;
    } else {
        return EINVAL;
    }
    NvHandle h = *hNew;
    *status = do_alloc(*hRoot, *hParent, &h, *hClass, pParams);
    if (*status == NV_OK) *hNew = h;
    LOG("ALLOC classe 0x%04x pai 0x%x -> 0x%x status 0x%x\n", *hClass, *hParent, h, *status);
    return 0;
}

static int esc_alloc_memory(void *data, uint32_t size)
{
    NVOS02_PARAMETERS *m;
    if (size == sizeof(nv_ioctl_nvos02_parameters_with_fd)) m = &((nv_ioctl_nvos02_parameters_with_fd *)data)->params;
    else if (size == sizeof(NVOS02_PARAMETERS)) m = data;
    else return EINVAL;
    NVOS02_PARAMETERS_R387 r;
    memset(&r, 0, sizeof(r));
    r.hRoot = m->hRoot; r.hObjectParent = m->hObjectParent; r.hObjectNew = m->hObjectNew;
    r.hClass = m->hClass; r.flags = m->flags;
    memcpy(&r.pMemory, &m->pMemory, sizeof(NvU64));
    r.limit = m->limit; r.status = NV_ERR_GENERIC;
    if (!r.hObjectNew) r.hObjectNew = m->hObjectNew = auto_handle();
    int e = g_xport(NV387_ESC_RM_ALLOC_MEMORY, &r, sizeof(r));
    m->status = x_status(e, r.status);
    if (m->status == NV_OK) {
        memcpy(&m->pMemory, &r.pMemory, sizeof(NvU64));
        m->limit = r.limit;
        struct obj *o = obj_add(r.hRoot, r.hObjectNew, r.hObjectParent, r.hClass, K_MEM);
        if (o) o->size = r.limit + 1;
    }
    return 0;
}

static int esc_map_memory(void *data, uint32_t size)
{
    NVOS33_PARAMETERS *m; int fd = -1;
    if (size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *w = data;
        m = &w->params; fd = w->fd;
    } else if (size == sizeof(NVOS33_PARAMETERS)) m = data;
    else return EINVAL;
    NVOS33_PARAMETERS_R387 r;
    memset(&r, 0, sizeof(r));
    r.hClient = m->hClient; r.hDevice = m->hDevice; r.hMemory = m->hMemory;
    r.offset = m->offset; r.length = m->length; r.flags = m->flags; r.status = NV_ERR_GENERIC;
    int e = g_xport(NV387_ESC_RM_MAP_MEMORY, &r, sizeof(r));
    m->status = x_status(e, r.status);
    m->flags = r.flags;
    if (m->status == NV_OK) {
        void *ptr = (void *)(uintptr_t)r.pLinearAddress;
        memcpy(&m->pLinearAddress, &ptr, sizeof(void *));
        if (fd >= 0 && fd < MAXFD) { g_fd[fd].ptr = ptr; g_fd[fd].len = (size_t)r.length; }
        LOG("MAP_MEMORY 0x%x len 0x%llx -> %p (fd %d)\n", r.hMemory, (unsigned long long)r.length, ptr, fd);
    }
    return 0;
}

static int esc_unmap_memory(void *data, uint32_t size)
{
    if (size != sizeof(NVOS34_PARAMETERS)) return EINVAL;
    NVOS34_PARAMETERS *m = data;
    NVOS34_PARAMETERS_R387 r;
    memset(&r, 0, sizeof(r));
    r.hClient = m->hClient; r.hDevice = m->hDevice; r.hMemory = m->hMemory; r.flags = m->flags;
    memcpy(&r.pLinearAddress, &m->pLinearAddress, sizeof(NvU64));
    r.status = NV_ERR_GENERIC;
    int e = g_xport(NV387_ESC_RM_UNMAP_MEMORY, &r, sizeof(r));
    m->status = x_status(e, r.status);
    return 0;
}

static int esc_map_dma(void *data, uint32_t size)
{
    if (size != sizeof(NVOS46_PARAMETERS)) return EINVAL;
    NVOS46_PARAMETERS *m = data;
    NVOS46_PARAMETERS_R387 r;
    memset(&r, 0, sizeof(r));
    r.hClient = m->hClient; r.hDevice = m->hDevice; r.hDma = m->hDma; r.hMemory = m->hMemory;
    r.offset = m->offset; r.length = m->length; r.flags = m->flags; r.dmaOffset = m->dmaOffset;
    r.status = NV_ERR_GENERIC;
    struct obj *v = obj_find(m->hClient, m->hDma);
    NvU64 base = 0;
    if (v && v->kind == K_VIRT) {
        if (m->dmaOffset + m->length > v->size) { m->status = NV_ERR_INVALID_LIMIT; return 0; }
        base = v->va;
        if (v->emulated) {
            struct devva *d = devva_get(m->hClient, v->hDevice, 0);
            if (!d || !d->ready) { m->status = NV_ERR_INVALID_STATE; return 0; }
            r.hDma = d->hCtxAll;
            r.flags = (m->flags & OS46_FLAGS_KEEP) | OS46_FLAG_FIXED;
        } else {
            r.hDma = v->h;
            r.flags = (m->flags & (OS46_FLAGS_KEEP | OS46_FLAG_KIND)) | OS46_FLAG_FIXED;
        }
        r.dmaOffset = base + m->dmaOffset;
    }
    int e = g_xport(NV387_ESC_RM_MAP_MEMORY_DMA, &r, sizeof(r));
    m->status = x_status(e, r.status);
    if (m->status == NV_OK) m->dmaOffset = r.dmaOffset - base;
    if (m->status == NV_OK && v && v->kind == K_VIRT && v->emulated) {
        if (v->nmap < VIRT_MAPS) { v->mapMem[v->nmap] = m->hMemory; v->mapVa[v->nmap] = r.dmaOffset; v->nmap++; }
        else WARN("virtual 0x%x com mais de %d mapas: o FREE nao desfaz o excedente\n", v->h, VIRT_MAPS);
    }
    LOG("MAP_MEMORY_DMA mem 0x%x dma 0x%x -> 0x%llx (VA 0x%llx) status 0x%x\n", m->hMemory, m->hDma,
        (unsigned long long)m->dmaOffset, (unsigned long long)r.dmaOffset, m->status);
    return 0;
}

static int esc_unmap_dma(void *data, uint32_t size)
{
    if (size != sizeof(NVOS47_PARAMETERS)) return EINVAL;
    NVOS47_PARAMETERS *m = data;
    NVOS47_PARAMETERS_R387 r;
    memset(&r, 0, sizeof(r));
    r.hClient = m->hClient; r.hDevice = m->hDevice; r.hDma = m->hDma; r.hMemory = m->hMemory;
    r.flags = m->flags; r.dmaOffset = m->dmaOffset; r.status = NV_ERR_GENERIC;
    struct obj *v = obj_find(m->hClient, m->hDma);
    if (v && v->kind == K_VIRT) {
        if (v->emulated) {
            struct devva *d = devva_get(m->hClient, v->hDevice, 0);
            if (!d || !d->ready) { m->status = NV_ERR_INVALID_STATE; return 0; }
            r.hDma = d->hCtxAll;
            r.flags = m->flags & OS46_FLAGS_KEEP;
        } else {
            r.hDma = v->h;
            r.flags = m->flags & (OS46_FLAGS_KEEP | OS46_FLAG_KIND);
        }
        r.dmaOffset = v->va + m->dmaOffset;
    }
    int e = g_xport(NV387_ESC_RM_UNMAP_MEMORY_DMA, &r, sizeof(r));
    m->status = x_status(e, r.status);
    if (m->status == NV_OK && v && v->kind == K_VIRT && v->emulated) {
        for (int i = 0; i < v->nmap; i++) {
            if (v->mapMem[i] != m->hMemory || v->mapVa[i] != r.dmaOffset) continue;
            v->nmap--; v->mapMem[i] = v->mapMem[v->nmap]; v->mapVa[i] = v->mapVa[v->nmap];
            break;
        }
    }
    return 0;
}

/* Escapes de superficie do NVK (IOSurface do acelerador MaxwellAccel): vao pela ponte ao kernel, que faz o dup da VRAM do
 * IOSurface direto neste cliente (a r387 nao tem NV04_SHARE). O NVK depois faz o proprio DUP desse handle; o handle
 * intermediario e liberado logo apos esse DUP. */
#define NVRM_ESC_SURF_VRAM  0xE2u
#define NVRM_ESC_SURF_DIRTY 0xE3u
#define MM_SEL_SURFSHARE    100u
#define SS_OFF              0x80000002u
struct ss_esc { uint32_t version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad; uint64_t size; };
#define MAXKDUP 64
static struct { NvHandle c, h, parent; } g_kdup[MAXKDUP];

static NvHandle client_device(NvHandle c)
{
    for (uint32_t i = 0; i < MAXOBJ; i++)
        if (g_obj[i].kind == K_DEVICE && g_obj[i].hClient == c) return g_obj[i].h;
    return 0;
}

static int esc_surfshare(uint32_t cmd, void *data, uint32_t size)
{
    if (size != sizeof(struct ss_esc)) return EINVAL;
    struct ss_esc *e = data;
#ifdef NVRM387_MOCK
    (void)cmd;
    e->status = SS_OFF;
#else
    NvHandle hDev = 0, hNew = 0;
    if (!g_via_bridge || !g_conn) { e->status = SS_OFF; return 0; }
    if (cmd == NVRM_ESC_SURF_VRAM) {
        hDev = client_device(e->hClient);
        if (!hDev) { e->status = SS_OFF; return 0; }
        hNew = internal_handle();
        e->hSrcClient = hDev; e->hSrcMemory = hNew;
    }
    uint64_t sc = cmd;
    size_t out = sizeof(*e);
    kern_return_t kr = IOConnectCallMethod(g_conn, MM_SEL_SURFSHARE, &sc, 1, e, sizeof(*e), NULL, NULL, e, &out);
    if (kr != KERN_SUCCESS || out != sizeof(*e)) { e->status = SS_OFF; return 0; }
    if (cmd == NVRM_ESC_SURF_VRAM && e->status == 0) {
        obj_add(e->hClient, hNew, hDev, 0, K_OTHER);
        for (int i = 0; i < MAXKDUP; i++)
            if (!g_kdup[i].h) { g_kdup[i].c = e->hClient; g_kdup[i].h = hNew; g_kdup[i].parent = hDev; break; }
        LOG("SURF_VRAM superficie %u plano %u -> handle 0x%x (%llu B)\n", e->surfaceID, e->plane, hNew,
            (unsigned long long)e->size);
    }
#endif
    return 0;
}

static int esc_dup(void *data, uint32_t size)
{
    if (size != sizeof(NVOS55_PARAMETERS)) return EINVAL;
    NVOS55_PARAMETERS *m = data;
    NVOS55_PARAMETERS_R387 r;
    memcpy(&r, m, sizeof(r));
    r.status = NV_ERR_GENERIC;
    /* a r387 nao gera handle no dup: hObject 0 volta NV_ERR_INVALID_OBJECT_HANDLE (0x33) */
    if (!r.hObject) r.hObject = auto_handle();
    int e = g_xport(NV387_ESC_RM_DUP_OBJECT, &r, sizeof(r));
    m->status = x_status(e, r.status);
    if (m->status == NV_OK) { m->hObject = r.hObject; obj_add(m->hClient, m->hObject, m->hParent, 0, K_OTHER); }
    if (m->status == NV_OK && m->hClientSrc == m->hClient)
        for (int i = 0; i < MAXKDUP; i++)
            if (g_kdup[i].h && g_kdup[i].c == m->hClient && g_kdup[i].h == m->hObjectSrc) {
                do_free(m->hClient, g_kdup[i].parent, g_kdup[i].h);
                g_kdup[i].h = 0;
                break;
            }
    return 0;
}

#ifndef NVRM387_MOCK
static NvU16 reg_u16(io_registry_entry_t e, CFStringRef key)
{
    NvU16 v = 0;
    CFTypeRef t = IORegistryEntrySearchCFProperty(e, kIOServicePlane, key, kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (t && CFGetTypeID(t) == CFDataGetTypeID() && CFDataGetLength(t) >= 2)
        memcpy(&v, CFDataGetBytePtr(t), 2);
    if (t) CFRelease(t);
    return v;
}
#endif

static int esc_card_info(void *data, uint32_t size)
{
    if (size < sizeof(nv_ioctl_card_info_t)) return EINVAL;
    memset(data, 0, size);
    nv_ioctl_card_info_t *ci = data;
    ci->valid = NV_TRUE;
    ci->minor_number = 0;
    /* gpu_id pelo RM: cliente temporario + GPU_GET_ATTACHED_IDS */
    NvHandle hc = 0;
    if (r387_alloc(0, 0, 0, CL_ROOT, &hc) == NV_OK && hc) {
        NvU32 ids[32];
        memset(ids, 0xFF, sizeof(ids));
        if (r387_control(hc, hc, CTRL_ATTACHED_IDS, ids, sizeof(ids)) == NV_OK && ids[0] != 0xFFFFFFFFu)
            ci->gpu_id = ids[0];
        r387_free(hc, hc, hc);
    }
#ifndef NVRM387_MOCK
    static const char *const pciSrc[] = { MM_BRIDGE_CLASS, "NVDA" };
    for (unsigned i = 0; i < 2 && !ci->pci_info.device_id; i++) {
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(pciSrc[i]));
        if (!svc) continue;
        ci->pci_info.vendor_id = reg_u16(svc, CFSTR("vendor-id"));
        ci->pci_info.device_id = reg_u16(svc, CFSTR("device-id"));
        IOObjectRelease(svc);
    }
#else
    ci->pci_info.vendor_id = 0x10DE;
#endif
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* API nvrm_xnu                                                                              */
/* ---------------------------------------------------------------------------------------- */
int nvrm_xnu_open(const char *node)
{
    (void)node;
#ifndef NVRM387_MOCK
    if (g_xport == nvdauser_escape && !g_conn && nvdauser_connect() != KERN_SUCCESS) {
        errno = (geteuid() != 0) ? EACCES : ENODEV;
        return -1;
    }
#endif
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= MAXFD) { close(fd); errno = EMFILE; return -1; }
    if (fd >= 0) { pthread_mutex_lock(&g_lock); g_fd[fd].ptr = NULL; g_fd[fd].len = 0; pthread_mutex_unlock(&g_lock); }
    return fd;
}

int nvrm_xnu_close(int fd)
{
    if (fd >= 0 && fd < MAXFD) { pthread_mutex_lock(&g_lock); g_fd[fd].ptr = NULL; g_fd[fd].len = 0; pthread_mutex_unlock(&g_lock); }
    return close(fd);
}

int nvrm_xnu_ioctl(int fd, uint32_t cmd, void *data, uint32_t size)
{
    (void)fd;
    if (!data) return EINVAL;
    int r;
    pthread_mutex_lock(&g_lock);
    switch (cmd) {
    case NV_ESC_RM_ALLOC:            r = esc_alloc(data, size); break;
    case NV_ESC_RM_FREE: {
        if (size != sizeof(NVOS00_PARAMETERS)) { r = EINVAL; break; }
        NVOS00_PARAMETERS *f = data;
        f->status = do_free(f->hRoot, f->hObjectParent, f->hObjectOld);
        LOG("FREE 0x%x status 0x%x\n", f->hObjectOld, f->status);
        r = 0; break;
    }
    case NV_ESC_RM_CONTROL: {
        if (size != sizeof(NVOS54_PARAMETERS)) { r = EINVAL; break; }
        NVOS54_PARAMETERS *k = data;
        void *p; memcpy(&p, &k->params, sizeof(void *));
        k->status = do_control(k->hClient, k->hObject, k->cmd, p, k->paramsSize);
        LOG("CONTROL 0x%08x obj 0x%x status 0x%x\n", k->cmd, k->hObject, k->status);
        r = 0; break;
    }
    case NV_ESC_RM_ALLOC_MEMORY:     r = esc_alloc_memory(data, size); break;
    case NV_ESC_RM_MAP_MEMORY:       r = esc_map_memory(data, size); break;
    case NV_ESC_RM_UNMAP_MEMORY:     r = esc_unmap_memory(data, size); break;
    case NV_ESC_RM_MAP_MEMORY_DMA:   r = esc_map_dma(data, size); break;
    case NV_ESC_RM_UNMAP_MEMORY_DMA: r = esc_unmap_dma(data, size); break;
    case NV_ESC_RM_DUP_OBJECT:       r = esc_dup(data, size); break;
    case NV_ESC_CARD_INFO:           r = esc_card_info(data, size); break;
    case NVRM_ESC_SURF_VRAM:
    case NVRM_ESC_SURF_DIRTY:        r = esc_surfshare(cmd, data, size); break;
    case NV_ESC_REGISTER_FD:         r = 0; break;           /* um unico user client: nada a fazer */
    case NV_ESC_ALLOC_OS_EVENT:
    case NV_ESC_FREE_OS_EVENT: {
        if (size != sizeof(nv_ioctl_alloc_os_event_t)) { r = EINVAL; break; }
        ((nv_ioctl_alloc_os_event_t *)data)->Status = NV_ERR_NOT_SUPPORTED;
        r = 0; break;
    }
    default:
        LOG("escape 0x%02x (%u bytes) nao suportado\n", cmd, size);
        r = EINVAL; break;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

void *nvrm_xnu_mmap(int fd, size_t length) { return nvrm_xnu_mmap_wc(fd, length, 0); }

void *nvrm_xnu_mmap_wc(int fd, size_t length, int writeCombine)
{
    (void)writeCombine;                                       /* o tipo de cache vem do RM */
    void *p = MAP_FAILED;
    pthread_mutex_lock(&g_lock);
    if (fd >= 0 && fd < MAXFD && g_fd[fd].ptr && length <= g_fd[fd].len) p = g_fd[fd].ptr;
    pthread_mutex_unlock(&g_lock);
    if (p == MAP_FAILED) errno = EINVAL;
    return p;
}

int nvrm_xnu_munmap(void *addr, size_t length)
{
    (void)addr; (void)length;                                 /* UNMAP_MEMORY desfaz o mapa */
    return 0;
}

int64_t nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us)
{
    (void)fd; (void)seen; (void)timeout_us;
    return -1;                                                /* sem eventos na r387 ainda */
}
