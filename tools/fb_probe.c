/*
 * fb_probe - SO LEITURA: procura no RM r387 a memoria do framebuffer da tela (classe 0x76
 * NV01_MEMORY_FRAMEBUFFER_CONSOLE), descobre o tamanho pelo MAP_MEMORY, le pela BAR1 e salva um PPM.
 * Nao escreve na memoria do framebuffer. Uso: ./fb_probe [saida.ppm] [largura altura]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
static NvU32 map_cpu(NvHandle c, NvHandle hDev, NvHandle hMem, NvU64 off, NvU64 len, volatile uint8_t **p)
{
    NVOS33_PARAMETERS_R387 m = { .hClient = c, .hDevice = hDev, .hMemory = hMem, .offset = off, .length = len,
                                 .status = 0xFFFFFFFF };
    if (esc(NV387_ESC_RM_MAP_MEMORY, &m, sizeof(m))) return 0xEEEEEEEE;
    *p = (volatile uint8_t *)(uintptr_t)m.pLinearAddress;
    return m.status;
}
static void unmap_cpu(NvHandle c, NvHandle hDev, NvHandle hMem, volatile uint8_t *p)
{
    NVOS34_PARAMETERS_R387 u = { .hClient = c, .hDevice = hDev, .hMemory = hMem,
                                 .pLinearAddress = (NvU64)(uintptr_t)p, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_UNMAP_MEMORY, &u, sizeof(u));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *outp = argc > 1 ? argv[1] : "/tmp/fb_probe.ppm";
    unsigned W = argc > 3 ? (unsigned)atoi(argv[2]) : 1920, H = argc > 3 ? (unsigned)atoi(argv[3]) : 1080;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hFb = 0xCAFE0076;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0, st;
    int ok = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }

    st = alloc(hc, hDev, hFb, 0x76, NULL);
    printf("ALLOC NV01_MEMORY_FRAMEBUFFER_CONSOLE (0x76) sob o device: status 0x%x\n", st);
    if (st) {
        uint8_t pp[256]; memset(pp, 0, sizeof pp);
        NvU32 s1 = alloc(hc, hDev, hFb, 0xC2, NULL);
        NvU32 s2 = s1 ? alloc(hc, hDev, hFb, 0xC2, pp) : 0;
        printf("ALLOC NV01_MEMORY_LOCAL_PHYSICAL (0xC2): NULL -> 0x%x, zeros -> 0x%x  memSize(0x00)=0x%llx\n", s1, s2,
               (unsigned long long)*(NvU64 *)pp);
        st = s1 && s2;
    }
    if (st) goto out;

    /* maior tamanho que o MAP_MEMORY aceita = tamanho do objeto (busca binaria em 4 KiB) */
    NvU64 lo = 0, hi = 256ull << 20;
    while (hi - lo > 0x1000) {
        NvU64 mid = ((lo + hi) / 2) & ~0xFFFull;
        volatile uint8_t *p = NULL;
        if (map_cpu(hc, hDev, hFb, 0, mid, &p) == 0 && p) { unmap_cpu(hc, hDev, hFb, p); lo = mid; } else hi = mid;
    }
    printf("tamanho mapeavel: 0x%llx bytes (%.2f MiB)  | 1920x1080x4 = 0x%x\n", (unsigned long long)lo, lo / 1048576.0, 1920 * 1080 * 4);
    if (!lo) goto out;
    volatile uint8_t *fb = NULL;
    st = map_cpu(hc, hDev, hFb, 0, lo, &fb);
    printf("MAP_MEMORY 0x%llx: status 0x%x ptr %p\n", (unsigned long long)lo, st, (void *)fb);
    if (st || !fb) goto out;
    unsigned pitch = (unsigned)(lo / H);
    unsigned cand[] = { W * 4, (W * 4 + 255) & ~255u, (W * 4 + 1023) & ~1023u, 8192 };
    for (unsigned i = 0; i < 4; i++) if (cand[i] * H <= lo) { pitch = cand[i]; break; }
    printf("pitch suposto %u (W %u H %u)\n", pitch, W, H);
    unsigned nz = 0;
    for (unsigned y = 0; y < H; y += 97) for (unsigned x = 0; x < W; x += 89)
        if (*(volatile uint32_t *)(fb + (size_t)y * pitch + x * 4) & 0x00FFFFFF) nz++;
    printf("amostras nao pretas: %u\n", nz);
    FILE *f = fopen(outp, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", W, H);
        uint8_t *row = malloc(W * 3);
        for (unsigned y = 0; y < H; y++) {
            for (unsigned x = 0; x < W; x++) {
                uint32_t v = *(volatile uint32_t *)(fb + (size_t)y * pitch + x * 4);
                row[x * 3] = (v >> 16) & 0xff; row[x * 3 + 1] = (v >> 8) & 0xff; row[x * 3 + 2] = v & 0xff;
            }
            fwrite(row, 1, W * 3, f);
        }
        free(row); fclose(f);
        printf("salvo %s\n", outp);
        ok = 1;
    }
    unmap_cpu(hc, hDev, hFb, fb);
out:;
    NVOS00_PARAMETERS_R387 fr = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &fr, sizeof(fr));
    printf("RESULTADO: %s\n", ok ? "framebuffer lido" : "falhou");
    IOServiceClose(gConn);
    return !ok;
}
