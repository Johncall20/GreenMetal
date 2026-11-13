/*
 * backend_test - exercita o backend de GPU no kernel do MaxwellMetal 0.3 (etapa B1 do porte do NVAccel).
 *   sudo ./backend_test [cor 0xAARRGGBB] [segundos]
 * Passos (cliente 'MXBK', so root): RM do kernel -> VRAM 1920x1080 pintada pelo MAPA DE KERNEL -> leitura de volta
 * -> mapa no VA da GPU -> flip pelo canal base EVO -> segura N s -> desliga a camada -> libera. Ao fechar o
 * cliente o kext desfaz tudo (mesmo se este processo morrer no meio).
 * Tamanho: NDRV_W / NDRV_H (padrao 1920x1080).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <IOKit/IOKitLib.h>

#define MM_BACKEND_TEST_TYPE 0x4D58424Bu

static io_connect_t gC;
static kern_return_t call(uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t out[4])
{
    uint32_t nout = 4;
    memset(out, 0, 4 * sizeof(uint64_t));
    return IOConnectCallScalarMethod(gC, sel, in, nin, out, &nout);
}
static void stamp(const char *w)
{
    time_t t = time(NULL); char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&t));
    printf("  %s %s\n", b, w);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t color = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0xFF00C0FFu;
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    uint32_t w = getenv("NDRV_W") ? (uint32_t)strtoul(getenv("NDRV_W"), NULL, 0) : 1920;
    uint32_t h = getenv("NDRV_H") ? (uint32_t)strtoul(getenv("NDRV_H"), NULL, 0) : 1080;
    uint32_t pitch = (w * 4 + 255) & ~255u;

    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MaxwellMetal"));
    if (!s) { printf("servico MaxwellMetal ausente\n"); return 1; }
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), MM_BACKEND_TEST_TYPE, &gC);
    IOObjectRelease(s);
    if (kr) { printf("IOServiceOpen 'MXBK': 0x%x (precisa de root e do MaxwellMetal 0.3)\n", kr); return 1; }

    uint64_t o[4], in[4];
    int ok = 1;
    kr = call(0, NULL, 0, o);
    printf("[rm] kr 0x%x: cliente do kernel 0x%llx device 0x%llx subdevice 0x%llx\n", kr, o[0], o[1], o[2]);
    if (kr) goto out;

    in[0] = (uint64_t)pitch * h; in[1] = color; in[2] = 0;
    kr = call(1, in, 3, o);
    printf("[vram] kr 0x%x status 0x%llx id %llu offset 0x%llx tamanho 0x%llx\n", kr, o[0], o[1], o[2], o[3]);
    if (kr) { ok = 0; goto out; }
    uint64_t id = o[1], size = o[3];

    uint64_t r0[4], r1[4];
    in[0] = id; in[1] = 0; call(6, in, 2, r0);
    in[1] = size / 4 - 1; call(6, in, 2, r1);
    printf("[kva] leitura pelo mapa de kernel: [0] 0x%08llx [fim] 0x%08llx %s\n", r0[1], r1[1],
           (r0[1] == color && r1[1] == color) ? "(confere)" : "(ERRADO)");
    ok &= r0[1] == color && r1[1] == color;

    in[0] = id;
    kr = call(2, in, 1, o);
    printf("[gpuva] kr 0x%x status 0x%llx VA 0x%llx\n", kr, o[0], o[1]);
    ok &= kr == 0;

    in[0] = id; in[1] = w; in[2] = h; in[3] = pitch;
    stamp("flip");
    kr = call(3, in, 4, o);
    printf("[flip] kr 0x%x status 0x%llx PUT 0x%llx GET 0x%llx %s\n", kr, o[0], o[1], o[2], o[3] ? "(consumido)" : "");
    ok &= kr == 0 && o[3];
    if (!kr) { printf("segurando %d s (a tela deve ficar na cor 0x%08x)...\n", secs, color); sleep((unsigned)secs); }

    stamp("desligando a camada base");
    kr = call(4, NULL, 0, o);
    printf("[off] kr 0x%x PUT 0x%llx GET 0x%llx\n", kr, o[1], o[2]);
    in[0] = id;
    kr = call(5, in, 1, o);
    printf("[free] kr 0x%x\n", kr);
out:
    IOServiceClose(gC);
    printf("RESULTADO: %s\n", ok && !kr ? "backend ok" : "falhou (ver log: log show --last 5m --predicate 'sender == \"MaxwellMetal\"')");
    return !(ok && !kr);
}
