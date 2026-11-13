/*
 * nmrm_probe - fase 3b, teste de vida do caminho de usuario NVDAUser do RM r387.
 *
 * Fala com o user client NVDAUser do NVDAResmanWeb (tipo 0x80000000, seletor 1 = rm_api).
 * Nada fica instalado: e um processo comum; ao sair, o RM limpa o cliente.
 *
 * Etapas (cada uma inclui as anteriores):
 *   1  FREE de um handle inexistente. Struct sem ponteiros embutidos: so prova o transporte.
 *      Esperado: status != 0 (handle/cliente invalido), sem crash.
 *   2  ALLOC do cliente raiz. pAllocParms aponta para memoria DESTE processo:
 *      prova se o RM le/escreve ponteiros embutidos na task do chamador.
 *      Esperado: status 0 e hClient != 0. Libera o cliente no fim.
 *   3  CONTROL NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS (0x201) no cliente: so leitura.
 *      Esperado: status 0 e o ID da GTX 960 na lista.
 *
 * Uso: sudo ./nmrm_probe <1|2|3>
 * Compilar: clang -arch x86_64 -O1 -Wall -o nmrm_probe nmrm_probe.c -framework IOKit -framework CoreFoundation
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "../nvrm_r387.h"

#define BUF_MAX 512

static io_connect_t gConn;

/* Envia um escape r387: buf = {cmd, size, params[size]}; a saida volta no mesmo buffer. */
static kern_return_t rm_escape(NvU32 esc, void *params, NvU32 size)
{
    uint8_t buf[BUF_MAX];
    if (size + sizeof(NV387_NVDAUSER_HEADER) > sizeof(buf)) return KERN_INVALID_ARGUMENT;
    NV387_NVDAUSER_HEADER h = { NV387_ESC_BASE | esc, size };
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), params, size);
    size_t inSize = sizeof(h) + size, outSize = inSize;
    kern_return_t kr = IOConnectCallStructMethod(gConn, NV387_NVDAUSER_SEL_RM_API,
                                                 buf, inSize, buf, &outSize);
    if (kr == KERN_SUCCESS && outSize == inSize)
        memcpy(params, buf + sizeof(h), size);
    else if (kr == KERN_SUCCESS)
        fprintf(stderr, "  aviso: outSize %zu != %zu, saida ignorada\n", outSize, inSize);
    return kr;
}

/* Params r387 usados nas etapas 4 e 5 (tamanhos confirmados nos call sites do GeForceWeb) */
typedef struct {                 /* NV0000_CTRL_CMD_GPU_GET_ID_INFO 0x202, 0x28 */
    NvU32 gpuId, gpuFlags, deviceInstance, subDeviceInstance;
    NvP64 szName;                /* NULL: nao pedimos o nome por aqui */
    NvU32 sliStatus, boardId, gpuInstance, numaId;
} NV0000_GPU_GET_ID_INFO_R387;
/* NV0080_ALLOC_PARAMETERS_R387 agora vem de nvrm_r387.h */
typedef struct { NvU32 subDeviceId; } NV2080_ALLOC_PARAMETERS_R387;
typedef struct {                 /* NV2080_CTRL_CMD_GPU_GET_NAME_STRING 0x20800110, 0x84 */
    NvU32 gpuNameStringFlags;
    uint8_t ascii[128];
} NV2080_GPU_GET_NAME_STRING_R387;
typedef struct {                 /* NV0080_CTRL_CMD_GPU_GET_CLASSLIST 0x800201, 0x10 */
    NvU32 numClasses, _pad;
    NvP64 classList;
} NV0080_GPU_GET_CLASSLIST_R387;
_Static_assert(sizeof(NV0000_GPU_GET_ID_INFO_R387) == 0x28, "ID_INFO");
_Static_assert(sizeof(NV2080_GPU_GET_NAME_STRING_R387) == 0x84, "NAME_STRING");
_Static_assert(sizeof(NV0080_GPU_GET_CLASSLIST_R387) == 0x10, "CLASSLIST");

static NvU32 gLastStatus;
static NvU32 rm_alloc(NvHandle, NvHandle, NvHandle, NvU32, void *);

static kern_return_t rm_ctrl(NvHandle hClient, NvHandle hObject, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS_R387 ct = { .hClient = hClient, .hObject = hObject, .cmd = cmd,
                                  .params = (NvP64)(uintptr_t)p, .paramsSize = size,
                                  .status = 0xFFFFFFFFu };
    kern_return_t kr = rm_escape(NV387_ESC_RM_CONTROL, &ct, sizeof(ct));
    gLastStatus = ct.status;
    return kr;
}

static NvU32 rm_alloc(NvHandle hClient, NvHandle hParent, NvHandle hObject, NvU32 hClass, void *p)
{
    NVOS21_PARAMETERS_R387 al = { .hRoot = hClient, .hObjectParent = hParent, .hObjectNew = hObject,
                                  .hClass = hClass, .pAllocParms = (NvP64)(uintptr_t)p,
                                  .status = 0xFFFFFFFFu };
    kern_return_t kr = rm_escape(NV387_ESC_RM_ALLOC, &al, sizeof(al));
    return kr == KERN_SUCCESS ? al.status : 0xEEEEEEEEu;
}

/* ---------------------------------------------------------------------------------------
 * Etapas 6-9: memoria, VA, canal 0xB06F e semaforo escrito pela GPU.
 * Layout seguido: o do GeForceWeb (NV01_MEMORY_VIRTUAL por ALLOC_MEMORY, canal com
 * hVASpace 0 = VA padrao do device, USERD por MAP_MEMORY do canal, GPPut em +0x8C).
 * ------------------------------------------------------------------------------------- */
#define H_VIRT    0xCAFE0070u
#define H_MEM     0xCAFE003Eu
#define H_NOTIF   0xCAFE003Fu
#define H_CTXERR  0xCAFE0002u
#define H_CTXVA   0xCAFE0202u
#define H_CHAN    0xCAFEB06Fu
#define H_ENGINE  0xCAFEB1C0u
#define MEM_SIZE  0x10000u      /* 64 KiB: GPFIFO @0, pushbuffer @0x1000, semaforo @0x2000 */
#define GPFIFO_ENTRIES 0x200u
#define SEM_MAGIC 0xCAFEF00Du

static int user_ptr_ok(NvU64 p) { return p >= 0x1000 && p < 0x00007FFFFFFFF000ull; }

static NvU32 alloc_memory(NvHandle hClient, NvHandle hParent, NvHandle hMem, NvU32 cls,
                          NvU32 flags, NvU64 size)
{
    NVOS02_PARAMETERS_R387 p = { .hRoot = hClient, .hObjectParent = hParent, .hObjectNew = hMem,
                                 .hClass = cls, .flags = flags, .pMemory = 0, .limit = size - 1,
                                 .status = 0xFFFFFFFFu };
    kern_return_t kr = rm_escape(NV387_ESC_RM_ALLOC_MEMORY, &p, sizeof(p));
    return kr == KERN_SUCCESS ? p.status : 0xEEEEEEEEu;
}

static NvU32 map_cpu(NvHandle hClient, NvHandle hDevice, NvHandle hMem, NvU64 len, NvU64 *out)
{
    NVOS33_PARAMETERS_R387 p = { .hClient = hClient, .hDevice = hDevice, .hMemory = hMem,
                                 .offset = 0, .length = len, .pLinearAddress = 0, .flags = 0,
                                 .status = 0xFFFFFFFFu };
    kern_return_t kr = rm_escape(NV387_ESC_RM_MAP_MEMORY, &p, sizeof(p));
    *out = p.pLinearAddress;
    return kr == KERN_SUCCESS ? p.status : 0xEEEEEEEEu;
}

static int channel_test(NvHandle hClient, NvHandle hDevice, int stage)
{
    NvU32 st;
    NvU32 st2;
    kern_return_t kr;
    /* 6a. memoria de sistema cached (0x1010) para GPFIFO/pushbuffer/semaforo e notifier */
    /* write-combine (0x2010), como os pushbuffers do GeForceWeb: com memoria cached (0x1010) e
     * mapa de GPU sem snoop, a GPU le a RAM por fora do cache da CPU e ve o GPFIFO zerado */
    st = alloc_memory(hClient, hDevice, H_MEM, 0x3E, 0x2010, MEM_SIZE);
    st2 = alloc_memory(hClient, hDevice, H_NOTIF, 0x3E, 0x2010, 0x1000);
    printf("[6] ALLOC_MEMORY sistema 64 KiB / notifier 4 KiB (WC): status 0x%08x / 0x%08x\n", st, st2);
    if (st || st2) return 1;
    /* 6b. mapa da CPU: o ponteiro tem que ser de usuario antes de tocar */
    NvU64 cpu = 0, ncpu = 0;
    st = map_cpu(hClient, hDevice, H_MEM, MEM_SIZE, &cpu);
    st2 = map_cpu(hClient, hDevice, H_NOTIF, 0x1000, &ncpu);
    printf("[6] MAP_MEMORY (CPU): status 0x%08x ptr 0x%llx / notifier 0x%08x ptr 0x%llx\n",
           st, (unsigned long long)cpu, st2, (unsigned long long)ncpu);
    if (st || st2 || !user_ptr_ok(cpu) || !user_ptr_ok(ncpu)) {
        printf("    ponteiro invalido para user space: nao vou tocar\n"); return 1;
    }
    volatile NvU32 *mem = (volatile NvU32 *)(uintptr_t)cpu;
    volatile NvU32 *notif = (volatile NvU32 *)(uintptr_t)ncpu;
    for (NvU32 i = 0; i < MEM_SIZE / 4; i++) mem[i] = 0;
    for (NvU32 i = 0; i < 0x1000 / 4; i++) notif[i] = 0;
    mem[0x2000 / 4] = 0x12345678u;
    printf("[6] escrita/leitura pela CPU: 0x%08x (esperado 0x12345678)\n", mem[0x2000 / 4]);
    if (mem[0x2000 / 4] != 0x12345678u) return 1;

    /* 6c. mapa da GPU: varias receitas numa execucao; para na primeira que funcionar */
    static const struct { const char *nome; NvU32 vflags; NvU64 vsize; int ctx; NvU32 mflags; NvU64 off; } V[] = {
        { "A virtual ALLOC_NONE + ctxdma, offset fixo 1 MiB", 0x12010, 0x1000000, 1, 1u << 15, 0x100000 },
        { "B virtual 16 MiB + ctxdma, RM escolhe",            0x02010, 0x1000000, 1, 0,        0 },
        { "C virtual 16 MiB + ctxdma, offset fixo 1 MiB",     0x02010, 0x1000000, 1, 1u << 15, 0x100000 },
        { "D virtual 16 MiB direto (sem ctxdma), RM escolhe", 0x02010, 0x1000000, 0, 0,        0 },
        { "E virtual 16 MiB + ctxdma, RM escolhe, SNOOP",     0x02010, 0x1000000, 1, 1u << 4,  0 },
    };
    NvU64 va = 0;
    NvHandle hCtxVa = 0;
    for (unsigned i = 0; i < sizeof(V) / sizeof(V[0]) && !va; i++) {
        NvHandle hv = H_VIRT + 0x10000u * (i + 1), hc = H_CTXVA + 0x10000u * (i + 1);
        NVOS02_PARAMETERS_R387 vp = { .hRoot = hClient, .hObjectParent = hDevice, .hObjectNew = hv,
                                      .hClass = 0x70, .flags = V[i].vflags, .pMemory = 0,
                                      .limit = V[i].vsize - 1, .status = 0xFFFFFFFFu };
        kr = rm_escape(NV387_ESC_RM_ALLOC_MEMORY, &vp, sizeof(vp));
        printf("[6%c] %s\n      virtual: status 0x%08x base 0x%llx limit 0x%llx\n", 'A' + i, V[i].nome,
               kr ? 0xEEEEEEEEu : vp.status, (unsigned long long)vp.pMemory, (unsigned long long)vp.limit);
        if (kr || vp.status) continue;
        NvHandle hDma = hv;
        if (V[i].ctx) {
            NVOS39_PARAMETERS_R387 cv = { .hObjectParent = hClient, .hObjectNew = hc, .hClass = 0x2,
                                          .flags = 0x20000000u, .hMemory = hv, .offset = 0,
                                          .limit = vp.limit, .status = 0xFFFFFFFFu };
            kr = rm_escape(NV387_ESC_RM_ALLOC_CONTEXT_DMA2, &cv, sizeof(cv));
            printf("      ctxdma: status 0x%08x\n", kr ? 0xEEEEEEEEu : cv.status);
            if (kr || cv.status) continue;
            hDma = hc;
        }
        NVOS46_PARAMETERS_R387 md = { .hClient = hClient, .hDevice = hDevice, .hDma = hDma,
                                      .hMemory = H_MEM, .offset = 0, .length = MEM_SIZE,
                                      .flags = V[i].mflags, .dmaOffset = V[i].off,
                                      .status = 0xFFFFFFFFu };
        kr = rm_escape(NV387_ESC_RM_MAP_MEMORY_DMA, &md, sizeof(md));
        printf("      MAP_MEMORY_DMA: status 0x%08x dmaOffset 0x%llx\n",
               kr ? 0xEEEEEEEEu : md.status, (unsigned long long)md.dmaOffset);
        if (!kr && md.status == 0) {
            va = vp.pMemory + md.dmaOffset;
            hCtxVa = V[i].ctx ? hc : 0;
            printf("      => OK: gpuVA 0x%010llx (base + dmaOffset)\n", (unsigned long long)va);
        }
    }
    if (!va) { printf("[6] nenhuma receita mapeou na GPU\n"); return 1; }

    if (stage < 7) return 0;

    /* 7a. context DMA do error notifier (NVOS39, igual a r615) */
    NVOS39_PARAMETERS_R387 cd = { .hObjectParent = hClient, .hObjectNew = H_CTXERR, .hClass = 0x2,
                                  .flags = 0x20000000u, .hMemory = H_NOTIF, .offset = 0,
                                  .limit = 0xFFF, .status = 0xFFFFFFFFu };
    kr = rm_escape(NV387_ESC_RM_ALLOC_CONTEXT_DMA2, &cd, sizeof(cd));
    printf("[7] ALLOC_CONTEXT_DMA2 (error notifier): kr 0x%08x status 0x%08x\n", kr, cd.status);
    if (kr || cd.status) return 1;
    /* 7b. canal: params zerados com folga; so o prefixo confirmado e preenchido */
    NvU32 cp[64];                              /* 0x100 bytes > 0x80 do GeForceWeb */
    memset(cp, 0, sizeof(cp));
    cp[0] = H_CTXERR;                          /* 0x00 hObjectError */
    cp[1] = hCtxVa;                            /* 0x04 hObjectBuffer (ctxdma do VA, se houver) */
    memcpy(&cp[2], &va, sizeof(va));           /* 0x08 gpFifoOffset (GPFIFO em va+0) */
    cp[4] = GPFIFO_ENTRIES;                    /* 0x10 gpFifoEntries */
    cp[5] = 0;                                 /* 0x14 flags */
    st = rm_alloc(hClient, hDevice, H_CHAN, 0xB06F, cp);
    printf("[7] ALLOC MAXWELL_CHANNEL_GPFIFO_A: status 0x%08x\n", st);
    if (st) return 1;
    /* 7b'. BindContextDma(canal, ctxdma do VA), igual ao GeForceWeb logo apos o Allocate */
    if (hCtxVa) {
        NvU32 bd[4] = { hClient, H_CHAN, hCtxVa, 0xFFFFFFFFu };
        kr = rm_escape(NV387_ESC_RM_BIND_CONTEXT_DMA, bd, sizeof(bd));
        printf("[7] BIND_CONTEXT_DMA(canal, ctxdma VA): kr 0x%08x status 0x%08x\n", kr, bd[3]);
        if (kr || bd[3]) return 1;
    }
    /* 7c. USERD: MAP_MEMORY do proprio canal, 0x200 bytes */
    NvU64 uctl = 0;
    st = map_cpu(hClient, hDevice, H_CHAN, 0x200, &uctl);
    printf("[7] MAP_MEMORY (USERD): status 0x%08x ptr 0x%016llx\n", st, (unsigned long long)uctl);
    if (st || !user_ptr_ok(uctl)) return 1;
    volatile NvU32 *userd = (volatile NvU32 *)(uintptr_t)uctl;
    printf("[7] USERD: GPGet 0x%x GPPut 0x%x\n", userd[0x88 / 4], userd[0x8C / 4]);
    if (stage < 8) return 0;

    /* 9 (opcional): objeto de engine, caso o canal so entre na runlist com um */
    if (stage >= 9) {
        st = rm_alloc(hClient, H_CHAN, H_ENGINE, 0xB1C0, NULL);
        printf("[9] ALLOC MAXWELL_COMPUTE_B no canal: status 0x%08x\n", st);
        if (st) return 1;
    }

    /* 10: agendamento explicito NVA06F_CTRL_CMD_GPFIFO_SCHEDULE (0xa06f0103), bEnable = 1.
     * r615 = 3 bytes; na r387 o tamanho e desconhecido: tenta 1..4 (tamanho errado so da 0x3A). */
    if (stage >= 10) {
        int ok = 0;
        for (NvU32 sz = 1; sz <= 4 && !ok; sz++) {
            uint8_t sp[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
            kern_return_t k2 = rm_ctrl(hClient, H_CHAN, 0xa06f0103, sp, sz);
            printf("[10] GPFIFO_SCHEDULE(bEnable=1) size %u: kr 0x%08x status 0x%08x\n", sz, k2, gLastStatus);
            if (k2 == KERN_SUCCESS && gLastStatus == 0) ok = 1;
        }
        if (!ok) return 1;
        printf("[10] USERD apos schedule: GPGet 0x%x GPPut 0x%x\n", userd[0x88 / 4], userd[0x8C / 4]);
    }

    /* 8. pushbuffer: SEMAPHOREA..D (host, subcanal 0), RELEASE 4 bytes de SEM_MAGIC */
    const NvU64 pbVA = va + 0x1000, semVA = va + 0x2000;
    volatile NvU32 *pb = mem + 0x1000 / 4;
    mem[0x2000 / 4] = 0;
    pb[0] = (1u << 29) | (4u << 16) | (0u << 13) | (0x10u >> 2);  /* INC_METHOD, 4 dados, 0x10 */
    pb[1] = (NvU32)(semVA >> 32) & 0xFF;                          /* SEMAPHOREA offset upper */
    pb[2] = (NvU32)semVA & ~3u;                                   /* SEMAPHOREB offset lower */
    pb[3] = SEM_MAGIC;                                            /* SEMAPHOREC payload */
    pb[4] = 0x2u | (1u << 24);                                    /* SEMAPHORED RELEASE, 4BYTE */
    const NvU32 pbDwords = 5;
    /* entrada GPFIFO 0 */
    mem[0] = (NvU32)pbVA & ~3u;
    mem[1] = ((NvU32)(pbVA >> 32) & 0xFF) | (pbDwords << 10);
    __sync_synchronize();
    __asm__ volatile("sfence" ::: "memory");
    printf("[8] kick: GPPut 0 -> 1 (pushbuffer 0x%010llx, semaforo 0x%010llx)\n",
           (unsigned long long)pbVA, (unsigned long long)semVA);
    userd[0x8C / 4] = 1;
    NvU32 v = 0;
    int ms;
    for (ms = 0; ms < 2000; ms++) {
        v = mem[0x2000 / 4];
        if (v == SEM_MAGIC) break;
        usleep(1000);
    }
    printf("[8] semaforo = 0x%08x apos %d ms | GPGet 0x%x GPPut 0x%x | notifier erro: "
           "info32 0x%08x status 0x%04x\n", v, ms, userd[0x88 / 4], userd[0x8C / 4],
           notif[2], notif[3] >> 16);
    if (v != SEM_MAGIC) {
        printf("    => a GPU NAO escreveu o semaforo%s\n", stage < 9 ? " (tente a etapa 9)" : stage < 10 ? " (tente a etapa 10)" : "");
        return 1;
    }
    printf("    => A GPU EXECUTOU NOSSO PUSHBUFFER\n");
    return 0;
}

int main(int argc, char **argv)
{
    int stage = argc > 1 ? atoi(argv[1]) : 0;
    if (stage < 1 || stage > 10) {
        fprintf(stderr, "uso: sudo %s <1..5>\n  1 = transporte (FREE invalido)\n"
                        "  2 = + cliente raiz\n  3 = + CONTROL GPU_GET_ATTACHED_IDS\n"
                        "  4 = + device/subdevice, nome da GPU, contagem de classes\n"
                        "  5 = + lista de classes (ponteiro aninhado)\n"
                        "  6 = + memoria de sistema, mapa CPU, objeto virtual, mapa GPU\n"
                        "  7 = + context DMA, canal 0xB06F, USERD (sem kick)\n"
                        "  8 = + kick: GPU escreve um semaforo\n"
                        "  9 = 8 com objeto MAXWELL_COMPUTE_B no canal\n"
                        "  10 = 9 + GPFIFO_SCHEDULE(bEnable) antes do kick\n", argv[0]);
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "precisa de root: o NVDAUser so aceita uid 0 (kauth com.nvidia.NVDA)\n");
        return 2;
    }

    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("NVDA"), &it) != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices falhou\n"); return 1;
    }
    io_service_t svc = 0, s;
    int n = 0;
    while ((s = IOIteratorNext(it))) { n++; if (!svc) svc = s; else IOObjectRelease(s); }
    IOObjectRelease(it);
    printf("servicos NVDA: %d\n", n);
    if (!svc) { fprintf(stderr, "nenhum servico NVDA\n"); return 1; }

    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn);
    IOObjectRelease(svc);
    printf("IOServiceOpen(NVDA, 0x%08x) -> 0x%08x\n", NV387_NVDAUSER_OPEN_TYPE, kr);
    if (kr != KERN_SUCCESS) return 1;

    int rc = 0;

    /* Etapa 1: FREE de handle inexistente, sem ponteiros */
    NVOS00_PARAMETERS_R387 fr = { .hRoot = 0xC1D00BADu, .hObjectParent = 0xC1D00BADu,
                                  .hObjectOld = 0xC1D00BADu, .status = 0xFFFFFFFFu };
    kr = rm_escape(NV387_ESC_RM_FREE, &fr, sizeof(fr));
    /* Com cliente inexistente o despachante rejeita em CliGetClientUnderLock e sai sem gravar
     * status (confirmado no codigo: so loga no nvlog). Logo, status intacto = esperado. */
    printf("[1] FREE invalido: kr 0x%08x status 0x%08x (esperado kr 0; status 0xffffffff =\n"
           "    rejeitado antes do RM por cliente inexistente; outro valor tambem e ok)\n", kr, fr.status);
    if (kr != KERN_SUCCESS) { rc = 1; goto out; }
    if (stage < 2) goto out;

    /* Etapa 2: cliente raiz; pAllocParms aponta para hClient neste processo */
    NvHandle hClient = 0;
    NVOS21_PARAMETERS_R387 al = { 0 };
    al.pAllocParms = (NvP64)(uintptr_t)&hClient;
    al.status = 0xFFFFFFFFu;
    kr = rm_escape(NV387_ESC_RM_ALLOC, &al, sizeof(al));
    printf("[2] ALLOC raiz: kr 0x%08x status 0x%08x hClient 0x%08x hObjectNew 0x%08x\n",
           kr, al.status, hClient, al.hObjectNew);
    if (kr != KERN_SUCCESS || al.status != 0 || hClient == 0) {
        printf("    => ponteiro embutido NAO foi tratado na nossa task (ou ALLOC recusado)\n");
        rc = 1; goto out;
    }
    printf("    => RM escreveu na memoria deste processo: caminho de usuario OK\n");

    NvU32 gpuId0 = 0;
    if (stage >= 3) {
        /* Etapa 3: NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS: NvU32 gpuIds[32], so saida */
        NvU32 ids[32];
        memset(ids, 0xFF, sizeof(ids));
        NVOS54_PARAMETERS_R387 ct = { .hClient = hClient, .hObject = hClient, .cmd = 0x201,
                                      .params = (NvP64)(uintptr_t)ids, .paramsSize = sizeof(ids),
                                      .status = 0xFFFFFFFFu };
        kr = rm_escape(NV387_ESC_RM_CONTROL, &ct, sizeof(ct));
        printf("[3] CONTROL GPU_GET_ATTACHED_IDS: kr 0x%08x status 0x%08x\n", kr, ct.status);
        if (kr == KERN_SUCCESS && ct.status == 0) {
            printf("    gpuIds:");
            for (int i = 0; i < 32 && ids[i] != 0xFFFFFFFFu; i++) printf(" 0x%08x", ids[i]);
            printf("\n");
            gpuId0 = ids[0];
        } else rc = 1;
    }

    if (stage >= 4 && rc == 0 && gpuId0 != 0xFFFFFFFFu) {
        /* Etapa 4a: NV0000_CTRL_CMD_GPU_GET_ID_INFO (0x202), 0x28 bytes, szName = NULL */
        NV0000_GPU_GET_ID_INFO_R387 ii = { .gpuId = gpuId0, .szName = 0 };
        NvU32 devInst = 0;
        kr = rm_ctrl(hClient, hClient, 0x202, &ii, sizeof(ii));
        printf("[4] GPU_GET_ID_INFO(0x%x): kr 0x%08x status 0x%08x flags 0x%x deviceInstance %u "
               "subDeviceInstance %u\n", gpuId0, kr, gLastStatus, ii.gpuFlags, ii.deviceInstance,
               ii.subDeviceInstance);
        if (kr == KERN_SUCCESS && gLastStatus == 0) devInst = ii.deviceInstance;
        else printf("    (seguindo com deviceInstance 0)\n");

        /* Etapa 4b: device (0x80) e subdevice (0x2080) */
        const NvHandle hDevice = 0xCAFE0080u, hSubdev = 0xCAFE2080u;
        NV0080_ALLOC_PARAMETERS_R387 dp = { .deviceId = devInst, .hClientShare = hClient };
        NvU32 st = rm_alloc(hClient, hClient, hDevice, 0x80, &dp);
        printf("[4] ALLOC NV01_DEVICE_0: status 0x%08x\n", st);
        if (st != 0) { rc = 1; goto free_client; }
        NV2080_ALLOC_PARAMETERS_R387 sp = { .subDeviceId = 0 };
        st = rm_alloc(hClient, hDevice, hSubdev, 0x2080, &sp);
        printf("[4] ALLOC NV20_SUBDEVICE_0: status 0x%08x\n", st);
        if (st != 0) { rc = 1; goto free_client; }

        /* Etapa 4c: nome da GPU (buffer dentro dos params, sem ponteiro aninhado) */
        NV2080_GPU_GET_NAME_STRING_R387 nm = { .gpuNameStringFlags = 0 /* ASCII */ };
        kr = rm_ctrl(hClient, hSubdev, 0x20800110, &nm, sizeof(nm));
        nm.ascii[sizeof(nm.ascii) - 1] = 0;
        printf("[4] GPU_GET_NAME_STRING: kr 0x%08x status 0x%08x nome \"%s\"\n", kr, gLastStatus, (char *)nm.ascii);
        if (kr != KERN_SUCCESS || gLastStatus != 0) rc = 1;

        /* Etapa 4d: so a contagem de classes (classList = NULL) */
        NV0080_GPU_GET_CLASSLIST_R387 cl = { .numClasses = 0, .classList = 0 };
        kr = rm_ctrl(hClient, hDevice, 0x800201, &cl, sizeof(cl));
        printf("[4] GPU_GET_CLASSLIST (contagem): kr 0x%08x status 0x%08x numClasses %u\n",
               kr, gLastStatus, cl.numClasses);
        if (kr != KERN_SUCCESS || gLastStatus != 0 || cl.numClasses == 0 || cl.numClasses > 1024) {
            rc = 1; goto free_client;
        }

        if (stage >= 5) {
            /* Etapa 5: lista de classes via ponteiro ANINHADO para memoria deste processo */
            NvU32 n = cl.numClasses;
            NvU32 *list = calloc(n + 16, sizeof(NvU32));   /* folga contra escrita a mais */
            if (!list) { rc = 1; goto free_client; }
            for (NvU32 i = 0; i < n + 16; i++) list[i] = 0xFFFFFFFFu;
            cl.classList = (NvP64)(uintptr_t)list;
            kr = rm_ctrl(hClient, hDevice, 0x800201, &cl, sizeof(cl));
            printf("[5] GPU_GET_CLASSLIST (lista): kr 0x%08x status 0x%08x numClasses %u\n",
                   kr, gLastStatus, cl.numClasses);
            if (kr == KERN_SUCCESS && gLastStatus == 0) {
                static const struct { NvU32 c; const char *n; } want[] = {
                    {0xB06F,"MAXWELL_CHANNEL_GPFIFO_A"}, {0xA06F,"KEPLER_CHANNEL_GPFIFO_A"},
                    {0xA06C,"KEPLER_CHANNEL_GROUP_A"}, {0xB197,"MAXWELL_B"}, {0xB1C0,"MAXWELL_COMPUTE_B"},
                    {0xB0B5,"MAXWELL_DMA_COPY_A"}, {0x902D,"FERMI_TWOD_A"}, {0xA140,"KEPLER_INLINE_TO_MEMORY_B"},
                    {0x90F1,"FERMI_VASPACE_A"}, {0x50A0,"NV50_MEMORY_VIRTUAL"}, {0x9067,"FERMI_CONTEXT_SHARE_A"},
                    {0xC36F,"VOLTA_CHANNEL_GPFIFO_A"}, {0xC361,"VOLTA_USERMODE_A"}, {0x00DA,"NV_SEMAPHORE_SURFACE"},
                };
                printf("    classes:");
                for (NvU32 i = 0; i < n; i++) printf("%s 0x%04x", (i % 12) ? "" : "\n     ", list[i]);
                printf("\n    interesse:\n");
                for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
                    int found = 0;
                    for (NvU32 i = 0; i < n; i++) if (list[i] == want[w].c) found = 1;
                    printf("      %-28s 0x%04x %s\n", want[w].n, want[w].c, found ? "SIM" : "nao");
                }
                if (list[n] != 0xFFFFFFFFu) printf("    AVISO: RM escreveu alem de numClasses\n");
            } else rc = 1;
            free(list);
        }

        if (stage >= 6 && rc == 0) {
            if (channel_test(hClient, hDevice, stage)) rc = 1;
        }
    }

free_client:;

    /* Libera o cliente raiz */
    NVOS00_PARAMETERS_R387 fc = { .hRoot = hClient, .hObjectParent = hClient,
                                  .hObjectOld = hClient, .status = 0xFFFFFFFFu };
    kr = rm_escape(NV387_ESC_RM_FREE, &fc, sizeof(fc));
    printf("[*] FREE cliente: kr 0x%08x status 0x%08x\n", kr, fc.status);

out:
    IOServiceClose(gConn);
    printf(rc ? "RESULTADO: falhou (veja acima)\n" : "RESULTADO: ok ate a etapa %d\n", stage);
    return rc;
}
