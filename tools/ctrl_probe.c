/*
 * ctrl_probe - testa na r387 os controles de LEITURA que o NVK usa na inicializacao.
 * Fala direto com o NVDAUser (structs r387), cria cliente/device/subdevice e libera no fim.
 * Nenhum controle aqui altera estado da GPU.
 *
 * Uso: sudo ./ctrl_probe
 * Compilar: clang -arch x86_64 -O1 -Wall -o ctrl_probe ctrl_probe.c -framework IOKit -framework CoreFoundation
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

typedef struct { NvU32 index, data; } INFO;
typedef struct { NvU32 n, _pad; NvU64 list; } LISTP;

int main(void)
{
    if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVDA"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), NV387_NVDAUSER_OPEN_TYPE, &gConn)) { fprintf(stderr, "NVDAUser\n"); return 1; }
    IOObjectRelease(svc);

    NvHandle hc = 0;
    if (alloc(0, 0, 0, 0, &hc) || !hc) { fprintf(stderr, "cliente\n"); return 1; }
    const NvHandle hDev = 0xCAFE0080, hSub = 0xCAFE2080;
    NV0080_ALLOC_PARAMETERS_R387 dp; memset(&dp, 0, sizeof(dp)); dp.hClientShare = hc;
    NvU32 sp = 0;
    if (alloc(hc, hc, hDev, 0x80, &dp) || alloc(hc, hDev, hSub, 0x2080, &sp)) { fprintf(stderr, "device\n"); goto out; }
    printf("cliente 0x%x, device e subdevice ok\n\n", hc);

    /* 1. MC_GET_ARCH_INFO */
    { NvU32 p[4] = { 0 }; NvU32 st = ctrl(hc, hSub, 0x20801701, p, 16);
      R("MC_GET_ARCH_INFO 0x20801701 (16)", st); printf("arch 0x%x impl 0x%x rev 0x%x\n", p[0], p[1], p[2]); }

    /* 2. TIMER_GET_TIME */
    { NvU64 t1 = 0, t2 = 0; NvU32 st = ctrl(hc, hSub, 0x20800403, &t1, 8); usleep(10000);
      ctrl(hc, hSub, 0x20800403, &t2, 8);
      R("TIMER_GET_TIME 0x20800403 (8)", st); printf("t1 %llu t2-t1 %lld ns\n", (unsigned long long)t1, (long long)(t2 - t1)); }

    /* 3. SHORT_NAME_STRING (64) */
    { char s[64 + 16]; memset(s, 0, sizeof(s)); NvU32 st = ctrl(hc, hSub, 0x20800111, s, 64); s[63] = 0;
      R("GPU_GET_SHORT_NAME_STRING 0x20800111 (64)", st); printf("\"%s\"\n", s); }

    /* 4. NAME_STRING com o tamanho da r615 (68): o RM confere paramsSize? buffer de 256 com sentinela */
    { uint8_t b[256]; memset(b, 0xA5, sizeof(b)); memset(b, 0, 68);
      NvU32 st = ctrl(hc, hSub, 0x20800110, b, 68);
      int over = 0; for (int i = 68; i < 256; i++) if (b[i] != 0xA5) over = 1;
      R("NAME_STRING com paramsSize 68 (r615)", st); printf("%s\n", over ? "ESCREVEU ALEM DE 68 BYTES" : "nada escrito alem de 68"); }

    /* 5. GR_GET_INFO por ponteiro: 0x801102 (o do GeForceWeb) e 0x801104 (r615) */
    NvU32 grIdx[3] = { 0x0C /* SM_VERSION */, 0x0D /* MAX_WARPS_PER_SM */, 0x20 /* LITTER_NUM_SM_PER_TPC */ };
    const char *grNome[3] = { "SM_VERSION", "MAX_WARPS_PER_SM", "LITTER_NUM_SM_PER_TPC" };
    NvU32 grCmds[2] = { 0x801102, 0x801104 };
    for (int c = 0; c < 2; c++) {
        INFO l[3]; for (int i = 0; i < 3; i++) { l[i].index = grIdx[i]; l[i].data = 0xDEADBEEF; }
        LISTP q = { 3, 0, (NvU64)(uintptr_t)l };
        char nome[64]; snprintf(nome, sizeof(nome), "GR_GET_INFO 0x%06x (3 indices)", grCmds[c]);
        NvU32 st = ctrl(hc, hDev, grCmds[c], &q, sizeof(q));
        R(nome, st); printf("SM 0x%x warps %u sm/tpc %u\n", l[0].data, l[1].data, l[2].data);
        if (st) for (int i = 0; i < 3; i++) {             /* um por vez, para ver qual indice falta */
            INFO one = { grIdx[i], 0xDEADBEEF }; LISTP q1 = { 1, 0, (NvU64)(uintptr_t)&one };
            NvU32 s1 = ctrl(hc, hDev, grCmds[c], &q1, sizeof(q1));
            printf("    %-24s idx 0x%02x status 0x%08x data 0x%x\n", grNome[i], grIdx[i], s1, one.data);
        }
    }

    /* 6. FB_GET_INFO por ponteiro 0x20801301 */
    { NvU32 fbIdx[4] = { 0x07 /* RAM_SIZE */, 0x05 /* BAR1_SIZE */, 0x08 /* TOTAL_RAM_SIZE */, 0x16 /* HEAP_FREE */ };
      const char *fbNome[4] = { "RAM_SIZE", "BAR1_SIZE", "TOTAL_RAM_SIZE", "HEAP_FREE" };
      INFO l[4]; for (int i = 0; i < 4; i++) { l[i].index = fbIdx[i]; l[i].data = 0xDEADBEEF; }
      LISTP q = { 4, 0, (NvU64)(uintptr_t)l };
      NvU32 st = ctrl(hc, hSub, 0x20801301, &q, sizeof(q));
      R("FB_GET_INFO 0x20801301 (4 indices)", st);
      printf("RAM %u KiB BAR1 %u KiB total %u KiB livre %u KiB\n", l[0].data, l[1].data, l[2].data, l[3].data);
      if (st) for (int i = 0; i < 4; i++) {
          INFO one = { fbIdx[i], 0xDEADBEEF }; LISTP q1 = { 1, 0, (NvU64)(uintptr_t)&one };
          NvU32 s1 = ctrl(hc, hSub, 0x20801301, &q1, sizeof(q1));
          printf("    %-24s idx 0x%02x status 0x%08x data 0x%x (%u)\n", fbNome[i], fbIdx[i], s1, one.data, one.data);
      } }

    /* 7. GPC / TPC mask (grRouteInfo {flags, pad, route} + campos) */
    { NvU32 p[6] = { 0 }; NvU32 st = ctrl(hc, hSub, 0x2080122a, p, 24);
      R("GR_GET_GPC_MASK 0x2080122a (24)", st); printf("gpcMask 0x%x\n", p[4]);
      NvU32 mask = st ? 0 : p[4];
      for (NvU32 g = 0; g < 32; g++) {
          if (st == 0 && !(mask & (1u << g))) continue;
          NvU32 t[6] = { 0 }; t[4] = g;
          NvU32 s2 = ctrl(hc, hSub, 0x2080122b, t, 24);
          printf("    GR_GET_TPC_MASK gpc %u: status 0x%08x tpcMask 0x%x\n", g, s2, t[5]);
          if (st) break;
      } }

    /* 8. ENGINE_PARTNERLIST (144): GRAPHICS(1) x MAXWELL_DMA_COPY_A */
    { NvU32 p[36]; memset(p, 0, sizeof(p)); p[0] = 1; p[1] = 0xB0B5;
      NvU32 st = ctrl(hc, hSub, 0x20800147, p, 144);
      R("GPU_GET_ENGINE_PARTNERLIST 0x20800147 (144)", st); printf("numPartners %u primeiro 0x%x\n", p[3], p[4]); }

    /* 9. FIFO_GET_ENGINE_CONTEXT_PROPERTIES (12), engineId 0 */
    { NvU32 p[3] = { 0, 0, 0 }; NvU32 st = ctrl(hc, hDev, 0x801707, p, 12);
      R("FIFO_GET_ENGINE_CONTEXT_PROPERTIES 0x801707", st); printf("align 0x%x size 0x%x\n", p[1], p[2]); }

    /* 10. SEMAPHORE_SURFACE_LAYOUT (r615, 32): esperado inexistente */
    { NvU32 p[8] = { 0 }; NvU32 st = ctrl(hc, hSub, 0x20801352, p, 32);
      R("FB_GET_SEMAPHORE_SURFACE_LAYOUT 0x20801352", st); printf("\n"); }


    /* 11. GR_GET_INFO no SUBDEVICE 0x20801201 (0x10, como o GeForceWeb), um indice por vez */
    { static const struct { NvU32 idx; const char *n; } gi[] = {
        { 0x0, "MAXCLIPS" },
        { 0x1, "MIN_ATTRS_BUG_261894" },
        { 0x3, "BUFFER_ALIGNMENT" },
        { 0x4, "SWIZZLE_ALIGNMENT" },
        { 0x5, "VERTEX_CACHE_SIZE" },
        { 0x6, "VPE_COUNT" },
        { 0x7, "SHADER_PIPE_COUNT" },
        { 0x8, "THREAD_STACK_SCALING_FACTOR" },
        { 0x9, "SHADER_PIPE_SUB_COUNT" },
        { 0xa, "SM_REG_BANK_COUNT" },
        { 0xb, "SM_REG_BANK_REG_COUNT" },
        { 0xc, "SM_VERSION" },
        { 0xd, "MAX_WARPS_PER_SM" },
        { 0xe, "MAX_THREADS_PER_WARP" },
        { 0xf, "GEOM_GS_OBUF_ENTRIES" },
        { 0x10, "GEOM_XBUF_ENTRIES" },
        { 0x11, "FB_MEMORY_REQUEST_GRANULARITY" },
        { 0x12, "HOST_MEMORY_REQUEST_GRANULARITY" },
        { 0x13, "MAX_SP_PER_SM" },
        { 0x14, "LITTER_NUM_GPCS" },
        { 0x15, "LITTER_NUM_FBPS" },
        { 0x16, "LITTER_NUM_ZCULL_BANKS" },
        { 0x17, "LITTER_NUM_TPC_PER_GPC" },
        { 0x18, "LITTER_NUM_MIN_FBPS" },
        { 0x19, "LITTER_NUM_MXBAR_FBP_PORTS" },
        { 0x1a, "TIMESLICE_ENABLED" },
        { 0x1b, "LITTER_NUM_FBPAS" },
        { 0x1c, "LITTER_NUM_PES_PER_GPC" },
        { 0x1d, "GPU_CORE_COUNT" },
        { 0x1e, "LITTER_NUM_TPCS_PER_PES" },
        { 0x1f, "LITTER_NUM_MXBAR_HUB_PORTS" },
        { 0x20, "LITTER_NUM_SM_PER_TPC" },
        { 0x21, "LITTER_NUM_HSHUB_FBP_PORTS" },
        { 0x22, "RT_CORE_COUNT" },
        { 0x23, "TENSOR_CORE_COUNT" },
        { 0x24, "LITTER_NUM_GRS" },
        { 0x25, "LITTER_NUM_LTCS" },
        { 0x26, "LITTER_NUM_LTC_SLICES" },
        { 0x27, "LITTER_NUM_GPCMMU_PER_GPC" },
        { 0x28, "LITTER_NUM_LTC_PER_FBP" },
        { 0x29, "LITTER_NUM_ROP_PER_GPC" },
        { 0x2a, "FAMILY_MAX_TPC_PER_GPC" },
        { 0x2b, "LITTER_NUM_FBPA_PER_FBP" },
        { 0x2c, "MAX_SUBCONTEXT_COUNT" },
        { 0x2d, "MAX_LEGACY_SUBCONTEXT_COUNT" },
        { 0x2e, "MAX_PER_ENGINE_SUBCONTEXT_COUNT" }
      };
      printf("\nGR_GET_INFO 0x20801201 no subdevice, um indice por vez:\n");
      int okc = 0;
      for (size_t i = 0; i < sizeof(gi) / sizeof(gi[0]); i++) {
          INFO one = { gi[i].idx, 0xDEADBEEF }; LISTP q1 = { 1, 0, (NvU64)(uintptr_t)&one };
          NvU32 s1 = ctrl(hc, hSub, 0x20801201, &q1, sizeof(q1));
          if (s1 == 0) okc++;
          printf("    0x%02x %-34s status 0x%08x data 0x%x (%u)\n", gi[i].idx, gi[i].n, s1, one.data, one.data);
      }
      printf("    => %d indices respondidos\n", okc); }

out:;
    NVOS00_PARAMETERS_R387 f = { .hRoot = hc, .hObjectParent = hc, .hObjectOld = hc, .status = 0xFFFFFFFF };
    esc(NV387_ESC_RM_FREE, &f, sizeof(f));
    printf("\nFREE cliente: status 0x%x\n", f.status);
    IOServiceClose(gConn);
    return 0;
}
