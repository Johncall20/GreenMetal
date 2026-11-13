/*
 * ndrv_present - teste do protocolo Presentment do NDRV do web driver (csCode 26) por user space.
 *
 *   ./ndrv_present probe
 *       So leitura: acha os heads com 0x40a {flags = 0} (volta 0 se o head existe, -0x11 se nao, sem efeito),
 *       le o modo atual pelo CoreGraphics e pergunta ao RM o pitch/tamanho (NvRmControl 0x20801324, o mesmo
 *       calculo que o NDRV faz). Nao mexe na tela.
 *
 *   ./ndrv_present show <head> [cor 0xAARRGGBB] [segundos]
 *       Aloca uma superficie nossa em VRAM (contigua, como a primaria do NDRV), pinta uma cor solida pela BAR1
 *       (cor solida nao depende de pitch/block linear), e manda 0x40d {registrar + aplicar} com o nosso hClient
 *       e hMemory. Segura N segundos (padrao 10) e sai. MEXE NA TELA.
 *       Para voltar: `pmset displaysleepnow` e acordar a tela (o GeForceWeb refaz o DupCurrentSurfaceInNDRV).
 *
 * Abre o RM pela ponte do MaxwellMetal (sem root); sem ela, NVDAUser direto (root).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <CoreGraphics/CoreGraphics.h>
#include "../nvrm_r387.h"

#define MM_BRIDGE_TYPE 0x4D58524Du
#define NDRV_CMD_CONTROL 0x84u
#define NDRV_KIND_INDIRECT 0x82u

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
static NvU32 control(NvHandle c, NvHandle obj, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS_R387 k = { .hClient = c, .hObject = obj, .cmd = cmd, .params = (NvU64)(uintptr_t)p,
                                 .paramsSize = size, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_CONTROL, &k, sizeof(k)) ? 0xEEEEEEEE : k.status;
}

/* ndrv_api (seletor 3): cabecalho de 0x14 bytes + bloco. O NVDAUser copia o bloco para o kernel e monta
 * sozinho o {blk, len, &resultado, 4} que o NDRV recebe; na volta, cabecalho+0x08 = retorno do NDRV
 * (ou o resultado de 4 bytes, se o retorno for 0) e o bloco volta em +0x14. */
static int ndrv_present(void *blk, uint32_t len, int32_t *ret)
{
    uint8_t buf[0x14 + 0x100];
    if (len > 0x100) return -1;
    memset(buf, 0, sizeof(buf));
    *(uint32_t *)(buf + 0x00) = NDRV_CMD_CONTROL;
    *(uint16_t *)(buf + 0x04) = NDRV_CSCODE_PRESENTMENT;
    *(uint32_t *)(buf + 0x0c) = NDRV_KIND_INDIRECT;
    memcpy(buf + 0x14, blk, len);
    size_t in = 0x14 + len, out = in;
    kern_return_t kr = IOConnectCallStructMethod(gConn, NV387_NVDAUSER_SEL_NDRV_API, buf, in, buf, &out);
    if (kr) { printf("  ndrv_api: kern_return 0x%x\n", kr); return -1; }
    *ret = *(int32_t *)(buf + 0x08);
    memcpy(blk, buf + 0x14, len);
    return 0;
}

static int head_exists(uint32_t head, int32_t *ret)
{
    uint8_t blk[0x88];
    memset(blk, 0, sizeof(blk));
    *(uint32_t *)(blk + 0x00) = NDRV_PRESENT_UPDATE;
    *(uint32_t *)(blk + 0x08) = head;
    if (ndrv_present(blk, sizeof(blk), ret)) return -1;
    return *ret == 0;
}

static int open_rm(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaxwellMetal"));
    if (s) {
        kern_return_t kr = IOServiceOpen(s, mach_task_self(), MM_BRIDGE_TYPE, &gConn);
        IOObjectRelease(s);
        if (!kr) { printf("RM: ponte do MaxwellMetal\n"); return 0; }
    }
    s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!s) return -1;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn);
    IOObjectRelease(s);
    if (kr) return -1;
    printf("RM: NVDAUser direto\n");
    return 0;
}

static void mode_of_main(uint32_t *w, uint32_t *h)
{
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    *w = m ? (uint32_t)CGDisplayModeGetPixelWidth(m) : 0;
    *h = m ? (uint32_t)CGDisplayModeGetPixelHeight(m) : 0;
    if (m) CGDisplayModeRelease(m);
}

#define ATTR_DEPTH_32   0x4u
#define ATTR_COMPR_NONE (2u << 8)
#define ATTR_PAGE_4K    (1u << 23)
#define ATTR_CONTIG     (2u << 27)
#define ATTR_WC         (2u << 29)
#define FL_HANDLE       0x4000u
#define FL_IGNORE_BANK  0x0001u
#define TYPE_IMAGE      0u
#define TYPE_PRIMARY    8u

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int show = argc > 1 && !strcmp(argv[1], "show");
    if (argc < 2 || (!show && strcmp(argv[1], "probe")) || (show && argc < 3)) {
        fprintf(stderr, "uso: %s probe | show <head> [cor 0xAARRGGBB] [segundos]\n", argv[0]);
        return 2;
    }
    if (open_rm()) { fprintf(stderr, "nao abriu o RM\n"); return 1; }

    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente RM\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hSurf = 0xCAFE0D00;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    int rc = 1;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    printf("cliente RM 0x%x\n", hc);

    printf("[heads] 0x40a {flags = 0}:\n");
    int found = -1;
    for (uint32_t h = 0; h < 4; h++) {
        int32_t r = 0;
        int e = head_exists(h, &r);
        printf("  head %u: retorno %d -> %s\n", h, r, e < 0 ? "ERRO" : e ? "existe" : "nao existe");
        if (e > 0 && found < 0) found = (int)h;
    }

    uint32_t w = 0, hgt = 0;
    mode_of_main(&w, &hgt);
    if ((!w || !hgt) && getenv("NDRV_W") && getenv("NDRV_H")) {
        w = (uint32_t)strtoul(getenv("NDRV_W"), NULL, 0);
        hgt = (uint32_t)strtoul(getenv("NDRV_H"), NULL, 0);
    }
    printf("[modo] tela principal %u x %u (pixels)\n", w, hgt);
    uint32_t q[6] = { w, hgt, 32, 0, 0, 0 };
    NvU32 st = control(hc, hSub, 0x20801324, q, sizeof(q));
    printf("[rm] 0x20801324 {%u, %u, 32}: status 0x%x pitch %u tamanho 0x%x\n", w, hgt, st, q[3], q[4]);
    if (!show) { rc = found < 0; printf("RESULTADO: %s\n", found < 0 ? "nenhum head" : "probe ok"); goto out; }

    uint32_t head = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t color = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0xFFFF00FFu;
    int secs = argc > 4 ? atoi(argv[4]) : 10;
    int32_t r = 0;
    if (head_exists(head, &r) != 1) { printf("head %u nao existe (retorno %d): abortado\n", head, r); goto out; }
    if (!w || !hgt) { printf("modo desconhecido: abortado\n"); goto out; }

    uint64_t need = (!st && q[4]) ? q[4] : (uint64_t)((w * 4u + 255u) & ~255u) * hgt;
    uint64_t size = (need * 2 + 0xFFFFF) & ~0xFFFFFull;
    NVOS32_ALLOC_R387 v;
    NvU32 types[2] = { TYPE_PRIMARY, TYPE_IMAGE };
    st = 0xFFFFFFFF;
    for (int i = 0; i < 2 && st; i++) {
        memset(&v, 0, sizeof(v));
        v.hRoot = hc; v.hObjectParent = hDev; v.function = NV387_NVOS32_FUNCTION_ALLOC_SIZE; v.status = 0xFFFFFFFF;
        v.owner = 0x6e646d6d; v.hMemory = hSurf; v.type = types[i]; v.flags = FL_HANDLE | FL_IGNORE_BANK;
        v.attr = ATTR_DEPTH_32 | ATTR_COMPR_NONE | ATTR_PAGE_4K | ATTR_CONTIG | ATTR_WC;
        v.size = size; v.alignment = 0x1000;
        if (esc(NV387_ESC_RM_VID_HEAP_CONTROL, &v, sizeof(v))) v.status = 0xEEEEEEEE;
        st = v.status;
        printf("[vram] tipo %u, %llu KiB contiguos: status 0x%x h 0x%x offset 0x%llx size 0x%llx kind 0x%x attr 0x%08x\n",
               types[i], (unsigned long long)(size >> 10), st, v.hMemory, (unsigned long long)v.offset,
               (unsigned long long)v.size, v.format, v.attr);
    }
    if (st) goto out;

    NVOS33_PARAMETERS_R387 m = { .hClient = hc, .hDevice = hDev, .hMemory = v.hMemory, .offset = 0, .length = v.size,
                                 .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY, &m, sizeof(m)) || m.status || !m.pLinearAddress) {
        printf("[bar1] MAP_MEMORY falhou: status 0x%x\n", m.status); goto out;
    }
    volatile uint32_t *px = (volatile uint32_t *)(uintptr_t)m.pLinearAddress;
    for (uint64_t i = 0; i < v.size / 4; i++) px[i] = color;
    __sync_synchronize();
    printf("[bar1] %llu KiB pintados com 0x%08x; leitura px[0] = 0x%08x\n", (unsigned long long)(v.size >> 10), color, px[0]);

    NDRV_REGISTER_SURFACE_R387 b;
    memset(&b, 0, sizeof(b));
    b.cmd = NDRV_PRESENT_REGISTER;
    b.head = head;
    b.doRegister = 1;
    b.doApply = 1;
    b.hClientSrc = hc;
    b.surf.hMemory = v.hMemory;
    b.surf.size = v.size;
    b.surf.kind = v.format;
    b.surf.offset = v.offset;
    ((uint8_t *)&b)[NDRV_REG_APPLY_GATE_OFF] = 1;
    printf("[0x40d] head %u, hClient 0x%x, hMemory 0x%x, offset 0x%llx, kind 0x%x, registrar + aplicar...\n",
           head, hc, v.hMemory, (unsigned long long)v.offset, v.format);
    if (ndrv_present(&b, sizeof(b), &r)) goto out;
    printf("[0x40d] retorno %d\n", r);
    if (getenv("NDRV_SEQ")) {
        uint8_t up[0x88];
        for (uint32_t mask = 0; mask < 2; mask++) {
            sleep(6);
            memset(up, 0, sizeof(up));
            *(uint32_t *)(up + 0x00) = NDRV_PRESENT_UPDATE;
            *(uint32_t *)(up + 0x08) = head;
            *(uint32_t *)(up + 0x0c) = mask;
            *(uint64_t *)(up + 0x10) = 1;
            int32_t ur = 0;
            int e = ndrv_present(up, sizeof(up), &ur);
            system("date '+  %H:%M:%S'");
            printf("[0x40a] commit flags 1, mascara %u: %s %d\n", mask, e ? "ERRO" : "retorno", ur);
        }
        sleep(6);
        system("date '+  %H:%M:%S'");
        printf("[tela] dormir (pmset displaysleepnow)\n");
        system("pmset displaysleepnow");
        sleep(5);
        system("date '+  %H:%M:%S'");
        printf("[tela] acordar (caffeinate -u)\n");
        system("caffeinate -u -t 2");
    }
    printf("segurando %d s (a tela deve ficar na cor 0x%08x)...\n", secs, color);
    sleep((unsigned)secs);
    printf("para voltar: pmset displaysleepnow e acordar a tela\n");
    rc = r != 0;
    printf("RESULTADO: %s\n", r == 0 ? "0x40d aceito" : "0x40d recusado");
out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
    IOServiceClose(gConn);
    return rc;
}
