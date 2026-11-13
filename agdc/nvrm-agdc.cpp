/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nvrm_off.h"
#include <IOKit/IOService.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/IOLib.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>          // clock_get_uptime/nanoseconds_to_absolutetime: the 0x980 rescan throttle
#include <libkern/OSAtomic.h>
#define FBLOG(fmt, ...) IOLog("MaxwellAGDC: " fmt "\n", ##__VA_ARGS__)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(local.maxwell.MaxwellAGDC, "0.2.1", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"
#pragma pack(push, 1)
struct NVAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct NVAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(NVAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(NVAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

static int    gAgdcK5 = 0;
static int    gMxTry = 0;
SYSCTL_INT(_debug, OID_AUTO, mxagdc_try, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gMxTry, 0,
           "bits: 1 = 0x2001, 2 = 0x941, 4 = 0x3, 8 = 0x2 respondidos com sucesso e saida zerada (experimento)");
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
// Slot-table rescans done while answering 0x980 (see nvdaAgdcRescanForReply).
static SInt64 gAgdcRescans = 0;

enum { kAgdcMaxFB = 8 };   // porte Maxwell: slot = IOAccelIndex; o MaxwellFB fica no 6 (os NVDA abrem antes e levam 0..5)
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
static int gAgdcAuto = 0;   // 1 = o AGDC sobe sozinho no boot (boot-arg nvaccel), antes do NVDC do RM
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8 (kext 0x31b3), and kAgdcFBMapBytes assumes one digit for n and each slot index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;

class NVDADeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVDADeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDADeviceControl, AppleGraphicsDeviceControl)

// Porte Maxwell: o NVDA nao publica "NVRMTiming"; monta os 13 numeros a partir do IODetailedTimingInformationV2 que o
// proprio NVDA publica em IOFBDetailedTimings (o de IOFBCurrentPixelClock), so se ha um display ligado nele.
static bool nvdaTimingFromNVDA(IOService *fb, uint32_t *t)
{
    if (!fb) return false;
    bool lit = false;
    if (OSIterator *it = fb->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IORegistryEntry *c = OSDynamicCast(IORegistryEntry, o);
            if (c && c->metaCast("IODisplayConnect") && c->getChildEntry(gIOServicePlane)) { lit = true; break; }
        }
        it->release();
    }
    if (!lit) return false;
    OSNumber *pc = OSDynamicCast(OSNumber, fb->getProperty("IOFBCurrentPixelClock"));
    OSArray *a = OSDynamicCast(OSArray, fb->getProperty("IOFBDetailedTimings"));
    if (!pc || !a) return false;
    for (unsigned i = 0; i < a->getCount(); i++) {
        OSData *d = OSDynamicCast(OSData, a->getObject(i));
        if (!d || d->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
        const IODetailedTimingInformationV2 *v = (const IODetailedTimingInformationV2 *)d->getBytesNoCopy();
        if (v->pixelClock != pc->unsigned64BitValue() || !v->horizontalActive || !v->verticalActive) continue;
        const uint32_t htot = v->horizontalActive + v->horizontalBlanking, vtot = v->verticalActive + v->verticalBlanking;
        if (!htot || !vtot) continue;
        bzero(t, NVK5_TIMING_WORDS * sizeof(uint32_t));
        t[NVK5_T_TAG] = NVK5_TIMING_TAG; t[NVK5_T_PCLK] = (uint32_t)v->pixelClock;
        t[NVK5_T_HVIS] = v->horizontalActive; t[NVK5_T_HSS] = v->horizontalActive + v->horizontalSyncOffset;
        t[NVK5_T_HSE] = t[NVK5_T_HSS] + v->horizontalSyncPulseWidth; t[NVK5_T_HTOT] = htot;
        t[NVK5_T_VVIS] = v->verticalActive; t[NVK5_T_VSS] = v->verticalActive + v->verticalSyncOffset;
        t[NVK5_T_VSE] = t[NVK5_T_VSS] + v->verticalSyncPulseWidth; t[NVK5_T_VTOT] = vtot;
        t[NVK5_T_REFRESH_MHZ] = (uint32_t)((v->pixelClock * 1000ull) / ((uint64_t)htot * vtot));
        t[NVK5_T_HSYNCPOS] = v->horizontalSyncConfig ? 1 : 0; t[NVK5_T_VSYNCPOS] = v->verticalSyncConfig ? 1 : 0;
        static int logged;
        if (logged < 4) { logged++; FBLOG("timing do NVDA %s: %ux%u pclk %u tot %ux%u refresh %u mHz", fb->getName(),
                                          t[NVK5_T_HVIS], t[NVK5_T_VVIS], t[NVK5_T_PCLK], htot, vtot, t[NVK5_T_REFRESH_MHZ]); }
        return nvk5_timing_bad(t, NVK5_TIMING_WORDS * sizeof(uint32_t)) == 0;
    }
    return false;
}

static const char *agdcFBClass();
static IOReturn nvdaK5Answer(IOService *fb, unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    IOService *slot[kAgdcMaxFB]; unsigned nh = gAgdcNFB;
    if (nh == 0 || nh > kAgdcMaxFB) { slot[0] = fb; nh = 1; } else for (unsigned i = 0; i < nh; i++) slot[i] = gAgdcFB[i];
    uint32_t t[kAgdcMaxFB][NVK5_TIMING_WORDS]; struct nvk5_head h[kAgdcMaxFB];
    for (unsigned i = 0; i < nh; i++) {
        unsigned long tBytes = 0; bool have = false;
        OSObject *o = slot[i] ? slot[i]->copyProperty("NVRMTiming") : NULL; OSData *d = OSDynamicCast(OSData, o);
        if (d) { have = true; tBytes = d->getLength(); bzero(t[i], sizeof t[i]); if (tBytes == sizeof t[i]) memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]); }
        OSSafeReleaseNULL(o);
        if (!have && strcmp(agdcFBClass(), "MaxwellFB") && nvdaTimingFromNVDA(slot[i], t[i])) { have = true; tBytes = sizeof t[i]; }
        h[i].t = have ? t[i] : NULL; h[i].tBytes = tBytes;
    }
    int why = 0; int32_t idx = -1;
    int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    if (r == NVK5_R_LINK) { OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask); return kIOReturnSuccess; }
    if (r == NVK5_R_CAPS) { OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask); return kIOReturnSuccess; }
    if (r == NVK5_R_UNSUPPORTED) return kIOReturnUnsupported;
    if (r == NVK5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !slot[idx]) why = 0x30;
    gAgdcK5Why = why; gAgdcK5LastRefusedIdx = idx;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC K5: command %#x index %d NOT answered (why %#x, %u slot(s)) [%d/40]", cmd, idx, why, nh, gAgdcLogged); }
    return r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
}

static void nvdaAgdcScan(IOService *pci);
// WindowServer reads AGDC command 0x980 when it (re)starts and builds one display pipe per entry in the reply. The
// slot table used to be scanned only once, when AGDC came up, so a framebuffer that started later -- a monitor
// plugged in after boot -- was never in the answer: WindowServer then built a single display pipe, the second
// display's composited frames had no pipe to go to, and the panel showed only its cursor while `screencapture -D 2`
// showed a full desktop (iop_ok_h0 grew, iop_ok_h1 stayed 0). Re-scan before answering, at most once a second and
// logging only when the count changes, so the reply is never stale. This runs in the vendor-call context, which
// holds no registry lock, and nvdaAgdcScan only matches services and reads their properties.
static void nvdaAgdcRescanForReply(IOService *pci)
{
    if (!pci) return;
    static uint64_t last = 0;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t per = 0; nanoseconds_to_absolutetime(1000000000ull, &per);
    if (last && now - last < per) return;
    last = now;
    const unsigned before = gAgdcNFB;
    nvdaAgdcScan(pci);
    OSAddAtomic64(1, &gAgdcRescans);
    if (gAgdcNFB != before)
        FBLOG("AGDC: 0x980 rescan -> %u framebuffer slot(s) (was %u)", gAgdcNFB, before);
}

IOReturn NVDADeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                     unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCVendorInfo)) return kIOReturnBadArgument;
        NVAGDCVendorInfo *v = (NVAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, "NVIDIA", sizeof v->vendor); v->vendorID = 0x10de; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCGPUCapability)) return kIOReturnBadArgument;
        nvdaAgdcRescanForReply(fPci);   // a framebuffer that started after boot has to be in this reply
        NVAGDCGPUCapability *c = (NVAGDCGPUCapability *)out; bzero(c, sizeof *c);
        unsigned n = gAgdcNFB;
        if ((n == 0 || n > kAgdcMaxFB) && !fFB) {
            c->maxFramebuffers = 0; c->pciDevice = fPci;
        } else if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; if (gAgdcFB[i]) c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        gAgdcMaxFBOut = (int)c->maxFramebuffers;
        return kIOReturnSuccess;
    }
    if (gAgdcK5 && (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX)) {
        IOReturn k5 = nvdaK5Answer(fFB, cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    // Porte Maxwell: experimento com os comandos que o CoreDisplay do Tahoe pede e o NullMoth nao responde.
    // debug.mxagdc_try: bit 0 = responder 0x2001 com sucesso e saida zerada; bit 1 = idem 0x941; bit 2 = 0x3; bit 3 = 0x2
    const unsigned long osz = outSize ? *outSize : 0;
    if (gAgdcLogged < 40) {
        gAgdcLogged++;
        unsigned long w0 = (in && inSize >= 4) ? (unsigned long)*(const volatile uint32_t *)in : 0;
        unsigned long w1 = (in && inSize >= 8) ? (unsigned long)*((const volatile uint32_t *)in + 1) : 0;
        FBLOG("AGDC: command %#x in %lu bytes [%#lx %#lx] out %lu bytes -> %s [%d/40]", cmd, inSize, w0, w1, osz,
              ((cmd == 0x2001 && (gMxTry & 1)) || (cmd == 0x941 && (gMxTry & 2)) || (cmd == 0x3 && (gMxTry & 4)) ||
               (cmd == 0x2 && (gMxTry & 8))) ? "ZERO OK" : "Unsupported", gAgdcLogged);
    }
    if (((cmd == 0x2001 && (gMxTry & 1)) || (cmd == 0x941 && (gMxTry & 2)) || (cmd == 0x3 && (gMxTry & 4)) ||
         (cmd == 0x2 && (gMxTry & 8))) && out && osz && osz <= 0x4000) {
        bzero(out, osz);
        return kIOReturnSuccess;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static NVDADeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;
static void nvdaAgdcDropTable(void)
{
    unsigned n = gAgdcNFB; gAgdcNFB = 0;
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { IOService *f = gAgdcFB[i]; gAgdcFB[i] = nullptr; if (f) f->release(); }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
}
static void nvdaAgdcDropFB(void) { if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; } nvdaAgdcDropTable(); }
// Porte Maxwell: os nos de framebuffer sao os do MaxwellFB quando o kext dele esta carregado; mxfb=0 ou -mxfboff volta ao NVDA.
static const char *agdcFBClass()
{
    static int v = -1;
    if (v < 0) {
        int x = 1; char b[4];
        const bool off = (PE_parse_boot_argn("mxfb", &x, sizeof x) && !x) || PE_parse_boot_argn("-mxfboff", b, sizeof b);
        const OSSymbol *n = OSSymbol::withCStringNoCopy("MaxwellFB");
        const bool loaded = n && OSMetaClass::getMetaClassWithName(n);
        OSSafeReleaseNULL(n);
        v = (!off && loaded) ? 1 : 0;
    }
    return v ? "MaxwellFB" : "NVDA";
}
static void nvdaAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    if (gAgdcAuto) {
        // modo automatico no boot: a GPU vem direto da GFX0 (IOPCIDevice NVIDIA de video), sem esperar o nub do MaxwellFB,
        // que so se registra quando o backend fica pronto -- depois do NVDC (0.4.0: nub 22:26:46.2, NVDC 22:26:45.9).
        OSDictionary *nm = IOService::serviceMatching("IOPCIDevice");
        if (nm) {
            OSString *v = OSString::withCString("0x000010de&0x0000ffff"), *c = OSString::withCString("0x03000000&0xff000000");
            if (v) { nm->setObject("IOPCIPrimaryMatch", v); v->release(); }
            if (c) { nm->setObject("IOPCIClassMatch", c); c->release(); }
        }
        IOService *p = nm ? IOService::waitForMatchingService(nm, 30ull * 1000 * 1000 * 1000) : nullptr; OSSafeReleaseNULL(nm);
        if (!p) { FBLOG("AGDC: modo automatico: sem IOPCIDevice NVIDIA de video em 30 s"); return; }
        p->release();   // a GFX0 vive enquanto a maquina estiver ligada; o AGDC se prende nela
        *pci = p;
        OSDictionary *m = IOService::serviceMatching("MaxwellFB");
        IOService *f = m ? IOService::copyMatchingService(m) : nullptr; OSSafeReleaseNULL(m);
        if (f) { gAgdcFBRef = f; *fb = f; }
        return;
    }
    OSDictionary *m = IOService::serviceMatching(agdcFBClass()); if (!m) return;
    IOService *f = IOService::copyMatchingService(m); m->release(); if (!f) return;
    IOService *p = f->getProvider(); int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
    if (!p || !p->metaCast("IOPCIDevice")) { f->release(); return; }
    gAgdcFBRef = f; *fb = f; *pci = p;
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; FBLOG("AGDC scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)
static void nvdaAgdcScan(IOService *pci)
{
    nvdaAgdcDropTable(); gAgdcScanLogged = 0;
    IOService *tab[kAgdcMaxFB] = {}; unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0;
    // O kext AGDC da Apple converte framebuffer[i] -> registry ID so ate o primeiro slot vazio (KDK 26.7.2, 0x29ef): os
    // slots tem que ser contiguos. Com o MaxwellFB no IOAccelIndex 6, os NVDA (0..5) preenchem 0..5.
    const bool mx = !strcmp(agdcFBClass(), "MaxwellFB");
    const char *classes[2] = { agdcFBClass(), mx ? "NVDA" : nullptr };
    for (int ci = 0; ci < 2 && classes[ci]; ci++) {
    OSDictionary *m = IOService::serviceMatching(classes[ci]);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    if (!it) continue;
    OSObject *o;
    while (seen < 16 && (o = it->getNextObject())) {
        seen++;
        IOService *f = OSDynamicCast(IOService, o); if (!f) continue;
        unsigned long long rid = f->getRegistryEntryID();
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (p != pci) { other++; SCANLOG("framebuffer id %#llx is under another PCI device: not ours to report", rid); continue; }
        // o IOPresentment numera cada framebuffer pela posicao no 0x980 e essa posicao vira o IOAccelIndex da entrada do
        // mapa (o display pipe do acelerador): o slot tem que ser o IOAccelIndex que a familia deu (MaxwellFB: 6)
        OSObject *io = f->copyProperty("IOAccelIndex");
        if (!OSDynamicCast(OSNumber, io)) {
            OSSafeReleaseNULL(io);
            if (ci == 1) { noidx++; continue; }   // NVDA sem IOAccelIndex (o WindowServer ainda nao o abriu): fora
            io = f->copyProperty("IOFBDependentIndex");
        }
        OSNumber *num = OSDynamicCast(OSNumber, io);
        long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
        if (ix < 0) { noidx++; SCANLOG("framebuffer id %#llx has no IOFBDependentIndex number: left out", rid); continue; }
        if (ix >= kAgdcMaxFB) { range++; SCANLOG("framebuffer id %#llx IOFBDependentIndex %lld >= %d: left out", rid, ix, (int)kAgdcMaxFB); continue; }
        if (tab[ix]) { dup++; SCANLOG("IOFBDependentIndex %lld claimed twice: id %#llx kept, id %#llx left out", ix, (unsigned long long)tab[ix]->getRegistryEntryID(), rid); continue; }
        f->retain(); tab[ix] = f;
    }
    it->release();
    }
    unsigned n = 0; while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++) if (tab[i]) {
        hole++; SCANLOG("slot %u (id %#llx) depois de um buraco no slot %u: fora (o AGDC da Apple para no buraco)", i, (unsigned long long)tab[i]->getRegistryEntryID(), n);
        tab[i]->release(); tab[i] = nullptr;
    }
    for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
    gAgdcNFB = n;
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", n);
    for (unsigned i = 0; i < n && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        if (gAgdcFB[i]) w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i, (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap && (noidx | range | dup | other | hole))
        snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u)", noidx, range, dup, other, hole);
    FBLOG("AGDC: slot table %s (%u framebuffer node(s) seen)", gAgdcFBMap, seen);
}

// Porte Maxwell: o RM r387 cria o AGDC dele (NVDC) na GFX0 quando inicializa (~5 s depois do boot). O IOPresentment
// (ioPresentmentGenerateDeviceMap) fica com o PRIMEIRO AGDC da lista para cada GPU, e a lista segue a ordem de attach
// na GFX0: o NVDC ganhava e o mapa so tinha os NVDA. Retirar o NVDC (0.3.1-0.3.3) travou o kernel; agora o nosso AGDC
// nasce no boot, antes do NVDC (modo automatico, abaixo), e fica primeiro.
static SInt32 gAgdcBusy = 0;
static void nvdaAgdcWorkBody(void);
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); nvdaAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}
static void nvdaAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        IOService *pci = nullptr, *fb = nullptr; nvdaAgdcTargets(&pci, &fb);
        if (!pci || (!fb && !gAgdcAuto)) { gAgdcState = -1; FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p)", pci, fb); return; }
        if (!fb) FBLOG("AGDC: modo automatico no boot: sobe na %s sem framebuffer; o MaxwellFB entra no 0x980 quando aparecer", pci->getName());
        nvdaAgdcScan(pci);
        NVDADeviceControl *d = OSTypeAlloc(NVDADeviceControl);
        if (!d) { gAgdcState = -2; FBLOG("AGDC: alloc failed"); nvdaAgdcDropFB(); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; FBLOG("AGDC: init failed"); d->release(); nvdaAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { gAgdcState = -4; FBLOG("AGDC: attach(%s) failed", pci->getName()); d->release(); nvdaAgdcDropFB(); return; }
        if (!d->start(pci)) { gAgdcState = -5; FBLOG("AGDC: AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); nvdaAgdcDropFB(); return; }
        gAgdc = d; gAgdcState = 1;
        FBLOG("AGDC: UP on %s — %u framebuffer slot(s), maxFramebuffers %d; vendor calls %lld, refused %lld (last %#x)", pci->getName(), gAgdcNFB, gAgdcMaxFBOut, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        NVDADeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); nvdaAgdcDropFB(); gAgdcState = 2;
        FBLOG("AGDC: terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}
static int nvrmfb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
    thread_call_enter(gAgdcCall); return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvrmfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvrmfb_agdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5,     0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities (0 after every boot)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,    "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,    "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why,  0, "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, nvrmfb_agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_rescans, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcRescans, "slot-table rescans done while answering 0x980 (a framebuffer that started after boot)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in our last 0x980 reply (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last BADARG/NOTFOUND refusal named (-1 none); an Unsupported 0x711 type shows in k5_asked only");
static void nvdaAgdcRemoveSysctl(void)
{
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}
static void nvdaAgdcInstallSysctl(void)
{
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_rescans); sysctl_register_oid(&sysctl__debug_mxagdc_try);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}

class NVDAGDCLoader : public IOService {
    OSDeclareDefaultStructors(NVDAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDAGDCLoader, IOService)
bool NVDAGDCLoader::start(IOService *provider)
{
    { if (nvrm_driver_off()) return false; }
    if (!IOService::start(provider)) return false;
    nvdaAgdcInstallSysctl();
    {
        int nv = 0, x = 1; char b[4];
        const bool want = PE_parse_boot_argn("nvaccel", &nv, sizeof nv) && nv && !PE_parse_boot_argn("-mxfboff", b, sizeof b)
                          && !(PE_parse_boot_argn("mxfb", &x, sizeof x) && !x) && !PE_parse_boot_argn("-mxagdcoff", b, sizeof b);
        if (want) {
            gAgdcAuto = 1; gAgdcWant = 1;
            if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
            if (gAgdcCall) thread_call_enter(gAgdcCall);
            FBLOG("loaded; modo automatico (nvaccel): o AGDC sobe agora, antes do NVDC do RM (-mxagdcoff desliga)");
            return true;
        }
    }
    FBLOG("loaded; idle until `sysctl debug.nvrmfb_agdc=1`"); return true;
}
void NVDAGDCLoader::stop(IOService *provider)
{
    nvdaAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) FBLOG("stop: the worker is STILL running after 2 s — leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; nvdaAgdcWorkBody(); }
    IOService::stop(provider);
}
