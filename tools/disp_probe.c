/*
 * disp_probe - estado do display pela r387 (so LEITURA): heads, displays conectados e o display de cada head.
 * Fala direto com o NVDAUser (structs r387), cria cliente/device/subdevice e libera no fim.
 * Nenhum controle aqui altera estado da GPU.
 *
 * Uso: sudo ./disp_probe
 * Compilar: clang -arch x86_64 -O1 -Wall -o disp_probe disp_probe.c -framework IOKit -framework CoreFoundation
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "../nvrm_r387.h"

static io_connect_t gConn;
static int esc(NvU32 e, void *p, NvU32 size)
{
    uint8_t buf[1024];
    NV387_NVDAUSER_HEADER h = { NV387_ESC_BASE | e, size };
    if (size > sizeof(buf) - sizeof(h)) return -1;
    memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), p, size);
    size_t in = sizeof(h) + size, out = in;
    if (IOConnectCallStructMethod(gConn, NV387_NVDAUSER_SEL_RM_API, buf, in, buf, &out) || out != in) return -1;
    memcpy(p, buf + sizeof(h), size);
    return 0;
}
static NvU32 ctrl(NvHandle c, NvHandle o, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS_R387 k = { .hClient = c, .hObject = o, .cmd = cmd, .params = (NvU64)(uintptr_t)p,
                                 .paramsSize = size, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_CONTROL, &k, sizeof(k)) ? 0xEEEEEEEE : k.status;
}
static NvU32 alloc(NvHandle c, NvHandle par, NvHandle h, NvU32 cls, void *p)
{
    NVOS21_PARAMETERS_R387 a = { .hRoot = c, .hObjectParent = par, .hObjectNew = h, .hClass = cls,
                                 .pAllocParms = (NvU64)(uintptr_t)p, .status = 0xFFFFFFFF };
    return esc(NV387_ESC_RM_ALLOC, &a, sizeof(a)) ? 0xEEEEEEEE : a.status;
}
#define R(name, st) printf("%-46s status 0x%08x %s", name, st, st == 0 ? "OK  " : "--  ")


int main(void)
{
    if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080, hDisp = 0xCAFE0073;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    NvU32 zp = 0; NvU32 st = alloc(hc, hDev, hDisp, 0x73, &zp);
    printf("NV04_DISPLAY_COMMON (0x73): status 0x%x\n", st);
    if (st) goto out;
    NvU32 nh[3] = { 0, 0, 0 };
    st = ctrl(hc, hDisp, 0x730102, nh, sizeof nh);
    R("GET_NUM_HEADS 0x730102", st); printf("heads %u\n", nh[2]);
    NvU32 sup[3] = { 0, 0, 0 };
    st = ctrl(hc, hDisp, 0x730120, sup, sizeof sup);
    R("GET_SUPPORTED 0x730120 (r387)", st); printf("displayMask 0x%x (DDC 0x%x)\n", sup[1], sup[2]);
    NvU32 cs[4] = { 0, 0, sup[1], 0 };
    st = ctrl(hc, hDisp, 0x730122, cs, sizeof cs);
    R("GET_CONNECT_STATE 0x730122 (r387)", st); printf("conectados 0x%x\n", cs[2]);
    unsigned n = nh[2] && nh[2] <= 8 ? nh[2] : 4;
    for (unsigned h = 0; h < n; h++) {
        NvU32 ga[4] = { 0, h, 0, 0 };
        st = ctrl(hc, hDisp, 0x730126, ga, sizeof ga);
        printf("GET_ACTIVE head %u                                status 0x%08x displayId 0x%x\n", h, st, ga[3]);
    }
out:;
    { NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
      esc(NV387_ESC_RM_FREE, &f, sizeof f); }
    IOServiceClose(gConn);
    return 0;
}
