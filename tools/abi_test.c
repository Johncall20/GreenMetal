/*
 * abi_test - percorre a ABI que o acelerador portado (MaxwellAccel) usa no MaxwellMetal 0.4, pelo cliente de teste:
 *   0 nvScanoutInfo (cria a superficie home preta e poe na tela)  1 nvAllocVram (endereco fisico de BAR1, pinta a cor)
 *   2 nvFlipToSurface (cor na tela)  3 flip HOME (preto)  4 nvGpuVaAlloc/Map/Free  5 nvFreeVram
 * Ao fechar, o kext desliga a camada base e libera tudo.
 *   sudo ./abi_test [cor 0xAARRGGBB] [segundos]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include "../nvrm_r387.h"

static io_connect_t gRm;
static int esc(NvU32 e, void *p, NvU32 size)
{
    uint8_t buf[1024];
    NV387_NVDAUSER_HEADER h = { NV387_ESC_BASE | e, size };
    memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), p, size);
    size_t in = sizeof(h) + size, out = in;
    if (IOConnectCallStructMethod(gRm, NV387_NVDAUSER_SEL_RM_API, buf, in, buf, &out) || out != in) return -1;
    memcpy(p, buf + sizeof(h), size);
    return 0;
}
static NvU32 rmalloc(NvHandle c, NvHandle par, NvHandle h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS_R387 a = { .hRoot = c, .hObjectParent = par, .hObjectNew = h, .hClass = cls,
                                 .pAllocParms = (NvU64)(uintptr_t)p, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_ALLOC, &a, sizeof(a)) ? 0xEEEEEEEE : a.status;
}

static io_connect_t gC;
static uint64_t step(uint64_t n, uint64_t color, uint64_t o[4])
{
    uint64_t in[2] = { n, color };
    uint32_t nout = 4;
    memset(o, 0, 4 * sizeof(uint64_t));
    kern_return_t kr = IOConnectCallScalarMethod(gC, 7, in, 2, o, &nout);
    time_t t = time(NULL); char b[16]; strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
    printf("  %s passo %llu: kr 0x%x retorno 0x%llx | 0x%llx 0x%llx 0x%llx\n", b, n, kr, o[0], o[1], o[2], o[3]);
    return kr ? kr : o[0];
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t color = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0xFF20E020u;
    int secs = argc > 2 ? atoi(argv[2]) : 5;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaxwellMetal"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4D58424Bu, &gC)) { printf("cliente 'MXBK' (root, MaxwellMetal 0.4)\n"); return 1; }
    IOObjectRelease(s);
    uint64_t o[4];
    int ok = 1;
    printf("[0] scanout/home: a tela deve ficar PRETA\n");
    ok &= step(0, 0, o) == 0;
    printf("    home: phys 0x%llx  %llux%llu pitch %llu\n", o[1], o[2] >> 32, o[2] & 0xffffffff, o[3]);
    sleep((unsigned)secs);
    printf("[1] nvAllocVram + cor 0x%08x\n", color);
    ok &= step(1, color, o) == 0;
    printf("    phys 0x%llx tamanho 0x%llx BAR1 usado 0x%llx\n", o[1], o[2], o[3]);
    {
        printf("[6] compartilhamento: dup da VRAM do kernel para um cliente RM de usuario (o caminho do NVK)\n");
        io_service_t b = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaxwellMetal"));
        NvHandle hc = 0;
        const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hNew = 0xCAFE5500;
        int okss = 0;
        if (b && !IOServiceOpen(b, mach_task_self(), 0x4D58524Du, &gRm) && !rmalloc(0, 0, 0, 0, &hc) && hc) {
            NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
            NvU32 sp = 0;
            if (!rmalloc(hc, hc, hDev, 0x80, &dp) && !rmalloc(hc, hDev, hSub, 0x2080, &sp)) {
                uint64_t r = step(6, ((uint64_t)hc << 32) | hDev, o);
                printf("    dup no cliente 0x%x device 0x%x: status 0x%llx -> cliente 0x%llx handle 0x%llx\n", hc, hDev, o[1], o[2], o[3]);
                if (!r) {
                    NVOS33_PARAMETERS_R387 m = { .hClient = hc, .hDevice = hDev, .hMemory = hNew, .offset = 0, .length = 0x10000,
                                                 .status = 0xFFFFFFFF };
                    if (!esc(NV387_ESC_RM_MAP_MEMORY, &m, sizeof(m)) && !m.status && m.pLinearAddress) {
                        uint32_t v = *(volatile uint32_t *)(uintptr_t)m.pLinearAddress;
                        printf("    leitura pelo handle do usuario: 0x%08x %s\n", v, v == color ? "(confere: e a mesma VRAM)" : "(DIFERENTE)");
                        okss = v == color;
                    } else printf("    MAP_MEMORY do handle novo: status 0x%x\n", m.status);
                }
            }
            NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
            esc(NV387_ESC_RM_FREE, &f, sizeof(f));
            IOServiceClose(gRm);
        } else printf("    cliente RM de usuario pela ponte: falhou\n");
        if (b) IOObjectRelease(b);
        ok &= okss;
    }
    printf("[2] flip: a tela deve ficar na COR\n");
    ok &= step(2, 0, o) == 0;
    sleep((unsigned)secs);
    printf("[3] flip HOME: PRETA de novo\n");
    ok &= step(3, 0, o) == 0;
    sleep((unsigned)secs);
    printf("[4] VA da GPU\n");
    ok &= step(4, 0, o) == 0;
    printf("[5] nvFreeVram\n");
    ok &= step(5, 0, o) == 0;
    IOServiceClose(gC);
    printf("fechado: a tela volta ao normal\nRESULTADO: %s\n", ok ? "ABI ok" : "falhou (log show --last 5m --predicate 'sender == \"MaxwellMetal\"')");
    return !ok;
}
