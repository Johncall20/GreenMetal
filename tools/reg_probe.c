/*
 * reg_probe - le (so LEITURA) o estado do display espelhado no BAR0 pelo controle EXEC_REG_OPS da r387.
 * Fala direto com o NVDAUser (structs r387), cria cliente/device/subdevice e libera no fim.
 * Nenhum controle aqui altera estado da GPU.
 *
 * Uso: sudo ./reg_probe
 * Compilar: clang -arch x86_64 -O1 -Wall -o reg_probe reg_probe.c -framework IOKit -framework CoreFoundation
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


/* layout r387 (0x20 bytes na tabela de controles do NVDAResmanWeb) e NV2080_CTRL_GPU_REG_OP (32 bytes) */
typedef struct { NvU32 hClientTarget, hChannelTarget, reserved00[3], regOpCount; NvU64 regOps; } REGOPS_P;
typedef struct { uint8_t regOp, regType, regStatus, regQuad; NvU32 regGroupMask, regSubGroupMask, regOffset,
                 regValueHi, regValueLo, regAndNMaskHi, regAndNMaskLo; } REGOP;
static const struct { NvU32 off; const char *name; } gRegs[] = {
    { 0x6404d0, "core head0 BASE_CHANNEL_USAGE_BOUNDS (em uso)" },
    { 0x6604d0, "core head0 BASE_CHANNEL_USAGE_BOUNDS (montagem)" },
    { 0x6404d4, "core head0 OVERLAY_USAGE_BOUNDS (em uso)" },
    { 0x640474, "core head0 CONTEXT_DMAS_ISO (superficie do core, em uso)" },
    { 0x640460, "core head0 OFFSET (em uso)" },
    { 0x640468, "core head0 SIZE (em uso)" },
    { 0x64046c, "core head0 STORAGE (em uso)" },
    { 0x66046c, "core head0 STORAGE (montagem)" },
    { 0x640470, "core head0 PARAMS (em uso)" },
    { 0x640440, "core head0 BASE_LUT_LO (em uso)" },
    { 0x640448, "core head0 OUTPUT_LUT_LO (em uso)" },
    { 0x64045c, "core head0 CONTEXT_DMA_LUT (em uso)" },
    { 0x640408, "core head0 CONTROL (em uso)" },
    { 0x640400, "core head0 PRESENT_CONTROL (em uso)" },
    { 0x640414, "core head0 RASTER_SIZE (em uso)" },
    { 0x6410c0, "base head0 CONTEXT_DMAS_ISO (em uso)" },
    { 0x6610c0, "base head0 CONTEXT_DMAS_ISO (montagem)" },
    { 0x641400, "base head0 SURFACE_OFFSET (em uso)" },
    { 0x641408, "base head0 SURFACE_SIZE (em uso)" },
    { 0x641410, "base head0 SURFACE_PARAMS (em uso)" },
    { 0x661084, "base head0 PRESENT_CONTROL (montagem)" },
    { 0x641084, "base head0 PRESENT_CONTROL (em uso)" },
    { 0x661400, "base head0 SURFACE_OFFSET (montagem)" },
    { 0x661408, "base head0 SURFACE_SIZE (montagem)" },
    { 0x66140c, "base head0 SURFACE_STORAGE (montagem)" },
    { 0x661410, "base head0 SURFACE_PARAMS (montagem)" },
    { 0x610088, "PDISP INTR (pendentes)" },
    { 0x61008c, "PDISP INTR excecao por canal (0x61008c)" },
    { 0x61009c, "PDISP INTR excecao por canal (0x61009c)" },
    { 0x6101f0, "excecao canal 0 (core): stat (tipo 14:12, metodo 11:0)" },
    { 0x6101f4, "excecao canal 0 (core): dado" },
    { 0x6101f8, "excecao canal 0 (core): codigo" },
    { 0x6101fc, "excecao canal 1 (base head0): stat (tipo 14:12, metodo 11:0)" },
    { 0x610200, "excecao canal 1 (base head0): dado" },
    { 0x610204, "excecao canal 1 (base head0): codigo" },
    { 0x610490, "controle canal 0 (core)" },
    { 0x610494, "estado canal 0 (core)" },
    { 0x6104a0, "controle canal 1 (base head0)" },
    { 0x6104a4, "estado canal 1 (base head0)" },
    { 0x640080, "core UPDATE (em uso)" },
    { 0x660080, "core UPDATE (montagem)" },
};
int main(void)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);
    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    for (unsigned i = 0; i < sizeof gRegs / sizeof gRegs[0]; i++) {
        REGOP op; memset(&op, 0, sizeof op);
        op.regOp = 0; /* READ_32 */ op.regType = 0; /* GLOBAL */ op.regStatus = 0xff; op.regOffset = gRegs[i].off;
        REGOPS_P p; memset(&p, 0, sizeof p);
        p.regOpCount = 1; p.regOps = (NvU64)(uintptr_t)&op;
        NvU32 st = ctrl(hc, hSub, 0x20800122, &p, sizeof p);
        printf("0x%06x %-56s ctrl 0x%x regStatus 0x%x valor 0x%08x\n", gRegs[i].off, gRegs[i].name, st, op.regStatus, op.regValueLo);
    }
out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof f);
    IOServiceClose(gConn);
    return 0;
}
