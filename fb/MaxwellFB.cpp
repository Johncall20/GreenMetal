// MaxwellFB: framebuffer nosso para a GTX 960 (Tahoe), no lugar do NVDA do web driver para a tela.
// O NDRV do NVDA recusa o InstallTiming e o preflight do CoreDisplay do Tahoe, entao o display fica offline e o
// IOPresentment nao consegue as capabilities (0x2006). Este framebuffer:
//   - tem um raster so, o que o head ja mostra (o modo do backend MaxwellMetal, 1920x1080 por padrao), com o timing
//     do 1o descritor detalhado do EDID do monitor (ou CEA 1080p60);
//   - aceita na validacao/instalacao de timing o que bate com esse raster (sem escalar, sem rotacao);
//   - copia o EDID do monitor que o NVDA le (AppleDisplay/IODisplayEDID), com o serial mexido para nao virar o mesmo
//     display do NVDA;
//   - mostra a superficie home do backend (VRAM no BAR1, que o canal base EVO ja varre) como abertura do sistema;
//   - vblank por timer no periodo do timing.
// Sobe so com boot-arg nvaccel (a troca feita); -mxfboff desliga.
// Referencia do contrato com o IOGraphics/CoreDisplay: NVRMFB do NullMoth (PolyForm Noncommercial, uso pessoal).

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>

#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>
#include "../accel/nvrm_vram_abi.h"
#include "../agdc/nvrm-agdc-k5.h"

// o SDK publico esconde o ID de modo do timing detalhado em __reservedA[0] (como o NVRMFB faz)
#ifndef detailedTimingModeID
#define detailedTimingModeID __reservedA[0]
#endif

#define FBLOG(fmt, ...) IOLog("MaxwellFB: " fmt "\n", ##__VA_ARGS__)

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(local.maxwell.MaxwellFB, "0.2.2", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

static const IODisplayModeID kModeId = 1;
static const char kPixelFormats[] = IO32BitDirectPixels "\0";
static const unsigned kMaxDT = 64;

static IOPMPowerState kFBPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

struct MXInterrupt { IOFBInterruptProc proc; OSObject *target; void *ref; UInt32 enabled; };

class MaxwellFB : public IOFramebuffer
{
    OSDeclareDefaultStructors(MaxwellFB)

    IOService *fPci = nullptr;
    IOService *fBackend = nullptr;
    IODeviceMemory *fAperture = nullptr;
    UInt64 fPhys = 0, fApBytes = 0;
    UInt32 fW = 0, fH = 0, fPitch = 0;
    IODetailedTimingInformationV2 fNative;
    UInt8 fEdid[512]; UInt32 fEdidSize = 0;
    UInt32 fMmW = 0, fMmH = 0;
    IODetailedTimingInformationV2 fDT[kMaxDT]; unsigned fNDT = 0;
    IODisplayModeID fCurId = kModeId;
    MXInterrupt fVbl = {}, fConnect = {};
    thread_call_t fVblCall = nullptr;
    uint64_t fVblPeriod = 0, fVblNext = 0;
    volatile SInt32 fStopping = 0;
    UInt32 fValidateLog = 0;
    UInt64 fVblCount = 0;

    static void vblFire(thread_call_param_t p0, thread_call_param_t);
    bool rasterMatches(const IODetailedTimingInformationV2 *d) const;
    int dtIndex(IODisplayModeID id) const;
    bool fetchScanout();
    void copyEdid();
    void buildNative();
    void publishCaps();
    IODeviceMemory *apertureSub();

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn enableController() override;
    bool isConsoleDevice() override { return false; }
    const char *getPixelFormats() override { return kPixelFormats; }
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
        { if (mode) *mode = kModeId; if (depth) *depth = 0; return kIOReturnSuccess; }
    IOReturn validateDetailedTiming(void *description, IOByteCount descripSize) override;
    IOReturn setDetailedTimings(OSArray *array) override;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }

    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) override;
    IOReturn getAppleSense(IOIndex, UInt32 *, UInt32 *, UInt32 *, UInt32 *) override { return kIOReturnUnsupported; }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override
        { if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag; return kIOReturnSuccess; }
    bool hasDDCConnect(IOIndex) override { return fEdidSize >= 128; }
    IOReturn getDDCBlock(IOIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits, UInt8 *data, IOByteCount *length) override;

    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }

    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **interruptRef) override;
    IOReturn unregisterInterrupt(void *interruptRef) override;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) override;

    IOReturn setPowerState(unsigned long ordinal, IOService *device) override;
};

OSDefineMetaClassAndStructors(MaxwellFB, IOFramebuffer)

static bool bootArgFlag(const char *name)
{
    char buf[8];
    return PE_parse_boot_argn(name, buf, sizeof buf);
}

static bool mxfbWanted()
{
    if (bootArgFlag("-mxfboff")) return false;
    int nvaccel = 0;
    if (!PE_parse_boot_argn("nvaccel", &nvaccel, sizeof nvaccel) || !nvaccel) return false;
    int mxfb = 1;
    if (PE_parse_boot_argn("mxfb", &mxfb, sizeof mxfb) && !mxfb) return false;
    return true;
}

// EDID do monitor: o AppleDisplay embaixo do NVDA que tem o display (NVDA,Display-x/NVDA/display0/AppleDisplay)
static UInt32 findMonitorEdid(UInt8 *buf, UInt32 cap, IOService **fromFB)
{
    UInt32 got = 0;
    OSDictionary *m = IOService::serviceMatching("IODisplay");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    OSSafeReleaseNULL(m);
    if (!it) return 0;
    while (OSObject *o = it->getNextObject()) {
        IOService *disp = OSDynamicCast(IOService, o);
        if (!disp) continue;
        IOService *fb = disp->getProvider() ? disp->getProvider()->getProvider() : nullptr;
        if (!fb || !fb->metaCast("NVDA")) continue;
        OSData *e = OSDynamicCast(OSData, disp->getProperty("IODisplayEDID"));
        if (!e || e->getLength() < 128 || e->getLength() > cap || (e->getLength() % 128)) continue;
        if (buf) bcopy(e->getBytesNoCopy(), buf, e->getLength());
        got = e->getLength();
        if (fromFB) *fromFB = fb;
        break;
    }
    it->release();
    return got;
}

// O nub casa na GFX0 e devolve o start na hora: o IOKit da start nos drivers de um provedor um de cada vez, e um start
// que espera (o 0.1.2 esperava o backend) segurava a GFX0 e com ela o RM e os NVDA (GFX0 busy 90 s, RM nunca pronto).
// Uma thread espera a superficie home do backend e o EDID do monitor e so entao registra o nub; o MaxwellFB casa nele.
class MaxwellFBNub : public IOService
{
    OSDeclareDefaultStructors(MaxwellFBNub)
    thread_call_t fWork = nullptr;
    static void work(thread_call_param_t p0, thread_call_param_t);
public:
    IOService *probe(IOService *provider, SInt32 *score) override
    {
        if (!mxfbWanted()) { FBLOG("nub: boot-args pedem a tela no NVDA (sem nvaccel, mxfb=0 ou -mxfboff)"); return nullptr; }
        return IOService::probe(provider, score);
    }
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        fWork = thread_call_allocate(&MaxwellFBNub::work, this);
        if (!fWork) return false;
        PMinit();
        provider->joinPMtree(this);
        registerPowerDriver(this, kFBPowerStates, 2);
        // Sem adjustBusy: o RM r387 so aceita cliente (NvRmAllocRoot com handle) depois que o WindowServer abre o NVDA;
        // segurar o registro ocupado (0.2.1) segurava o WindowServer e o RM nunca ficava pronto. O LaunchDaemon
        // local.maxwell.mxfb-ws reinicia o WindowServer uma vez quando o MaxwellFB aparece depois dele.
        retain();
        thread_call_enter(fWork);
        FBLOG("nub em %s: a GFX0 fica livre; o framebuffer sobe quando o backend e o EDID estiverem prontos", provider->getName());
        return true;
    }
};
OSDefineMetaClassAndStructors(MaxwellFBNub, IOService)

void MaxwellFBNub::work(thread_call_param_t p0, thread_call_param_t)
{
    MaxwellFBNub *me = (MaxwellFBNub *)p0;
    OSDictionary *m = IOService::serviceMatching("MaxwellMetal");
    IOService *b = m ? IOService::waitForMatchingService(m, 180ull * 1000 * 1000 * 1000) : nullptr;
    OSSafeReleaseNULL(m);
    bool ready = false;
    if (!b) FBLOG("nub: sem o servico MaxwellMetal em 180 s");
    const OSSymbol *fn = OSSymbol::withCString(NVRM_VRAM_FN_SCANOUT);
    uint64_t t0 = 0, t1 = 0; clock_get_uptime(&t0);
    for (int i = 0; b && fn && i < 240 && !ready; i++) {
        struct NVRMVramRequest r; bzero(&r, sizeof r);
        r.version = NVRM_VRAM_ABI_VERSION;
        IOReturn ret = b->callPlatformFunction(fn, false, &r, nullptr, nullptr, nullptr);
        ready = ret == kIOReturnSuccess && r.phys && r.width >= 640 && r.height >= 480;
        if (!ready) { if (i % 20 == 0) FBLOG("nub: backend ainda sem superficie home (0x%x), tentativa %d", ret, i); IOSleep(1000); }
    }
    OSSafeReleaseNULL(fn);
    OSSafeReleaseNULL(b);
    clock_get_uptime(&t1);
    uint64_t ns = 0; absolutetime_to_nanoseconds(t1 - t0, &ns);
    if (ready) {
        int k = 0;
        while (k < 60 && !findMonitorEdid(nullptr, 512, nullptr)) { IOSleep(500); k++; }
        FBLOG("nub: backend pronto em %llu ms, EDID %s; registrando", ns / 1000000, k < 60 ? "presente" : "ausente (30 s)");
        me->registerService();
    } else FBLOG("nub: o backend nao deu a superficie home em 240 s: sem MaxwellFB neste boot");
    me->release();
}

IOService *MaxwellFB::probe(IOService *provider, SInt32 *score)
{
    if (!mxfbWanted()) return nullptr;
    return IOFramebuffer::probe(provider, score);
}

bool MaxwellFB::rasterMatches(const IODetailedTimingInformationV2 *d) const
{
    if (!d || d->horizontalActive != fW || d->verticalActive != fH) return false;
    if (d->horizontalScaledInset || d->verticalScaledInset) return false;
    if (d->horizontalBorderLeft | d->horizontalBorderRight | d->verticalBorderTop | d->verticalBorderBottom) return false;
    if (d->scalerFlags & kIOScaleRotateFlags) return false;
    if ((d->horizontalScaled || d->verticalScaled) && (d->horizontalScaled != fW || d->verticalScaled != fH)) return false;
    const UInt64 want = fNative.pixelClock, got = d->pixelClock;
    if (!got) return false;
    const UInt64 diff = got > want ? got - want : want - got;
    return diff * 200 <= want;   // 0,5%: 60 Hz e 59,94 Hz caem aqui; o head fica no timing de boot de qualquer jeito
}

int MaxwellFB::dtIndex(IODisplayModeID id) const
{
    for (unsigned i = 0; i < fNDT; i++) if ((IODisplayModeID)fDT[i].detailedTimingModeID == id) return (int)i;
    return -1;
}

// a superficie home do backend: o que o canal base EVO varre no head
bool MaxwellFB::fetchScanout()
{
    const OSSymbol *fn = OSSymbol::withCString(NVRM_VRAM_FN_SCANOUT);
    if (!fn) return false;
    bool ok = false;
    for (int tries = 0; tries < 10 && !ok; tries++) {
        struct NVRMVramRequest r; bzero(&r, sizeof r);
        r.version = NVRM_VRAM_ABI_VERSION;
        IOReturn ret = fBackend->callPlatformFunction(fn, false, &r, nullptr, nullptr, nullptr);
        if (ret == kIOReturnSuccess && r.phys && r.width >= 640 && r.height >= 480 && r.pitch >= r.width * 4) {
            fPhys = r.phys; fW = r.width; fH = r.height; fPitch = r.pitch;
            fApBytes = ((UInt64)fPitch * fH + 4095) & ~4095ull;
            ok = true;
        } else {
            if (tries == 0 || tries % 20 == 19) FBLOG("nvScanoutInfo -> 0x%x (phys 0x%llx %ux%u), tentando de novo", ret, r.phys, r.width, r.height);
            IOSleep(500);
        }
    }
    fn->release();
    if (ok) FBLOG("superficie home: phys 0x%llx %ux%u pitch %u (%llu B)", fPhys, fW, fH, fPitch, fApBytes);
    return ok;
}

void MaxwellFB::copyEdid()
{
    for (int tries = 0; tries < 4 && !fEdidSize; tries++) {
        IOService *fb = nullptr;
        fEdidSize = findMonitorEdid(fEdid, sizeof fEdid, &fb);
        if (fEdidSize) FBLOG("EDID de %u B copiado do display do %s (id 0x%llx)", fEdidSize, fb->getName(), fb->getRegistryEntryID());
        else IOSleep(500);
    }
    if (!fEdidSize) { FBLOG("sem EDID do monitor: display generico"); return; }
    static const UInt8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(fEdid, hdr, 8) != 0) { FBLOG("EDID sem cabecalho valido: descartado"); fEdidSize = 0; return; }
    // outro serial: o CoreDisplay nao junta este display com o do NVDA (que fica offline)
    fEdid[15] ^= 0x4D;
    UInt8 sum = 0;
    for (int i = 0; i < 127; i++) sum += fEdid[i];
    fEdid[127] = (UInt8)(0x100 - sum);
    fMmW = fEdid[21] * 10; fMmH = fEdid[22] * 10;
}

void MaxwellFB::buildNative()
{
    bzero(&fNative, sizeof fNative);
    IODetailedTimingInformationV2 &d = fNative;
    bool fromEdid = false;
    if (fEdidSize >= 128) {
        for (int k = 0; k < 4 && !fromEdid; k++) {
            const UInt8 *b = fEdid + 54 + 18 * k;
            const UInt32 pclk = (UInt32)(b[0] | (b[1] << 8)) * 10000u;
            if (!pclk) continue;
            const UInt32 ha = b[2] | ((b[4] & 0xF0) << 4), hb = b[3] | ((b[4] & 0x0F) << 8);
            const UInt32 va = b[5] | ((b[7] & 0xF0) << 4), vb = b[6] | ((b[7] & 0x0F) << 8);
            const UInt32 hso = b[8] | ((b[11] & 0xC0) << 2), hsw = b[9] | ((b[11] & 0x30) << 4);
            const UInt32 vso = (b[10] >> 4) | ((b[11] & 0x0C) << 2), vsw = (b[10] & 0x0F) | ((b[11] & 0x03) << 4);
            if (ha != fW || va != fH || (b[17] & 0x80)) continue;   // outro raster ou entrelacado
            d.pixelClock = pclk;
            d.horizontalActive = ha; d.horizontalBlanking = hb; d.horizontalSyncOffset = hso; d.horizontalSyncPulseWidth = hsw;
            d.verticalActive = va; d.verticalBlanking = vb; d.verticalSyncOffset = vso; d.verticalSyncPulseWidth = vsw;
            d.horizontalSyncConfig = (b[17] & 0x18) == 0x18 ? ((b[17] & 0x02) ? 1 : 0) : 1;
            d.verticalSyncConfig = (b[17] & 0x18) == 0x18 ? ((b[17] & 0x04) ? 1 : 0) : 1;
            const UInt32 mw = b[12] | ((b[14] & 0xF0) << 4), mh = b[13] | ((b[14] & 0x0F) << 8);
            if (mw && mh) { fMmW = mw; fMmH = mh; }
            fromEdid = true;
        }
    }
    if (!fromEdid) {
        if (fW == 1920 && fH == 1080) {   // CEA-861 VIC 16
            d.pixelClock = 148500000ull;
            d.horizontalActive = 1920; d.horizontalBlanking = 280; d.horizontalSyncOffset = 88; d.horizontalSyncPulseWidth = 44;
            d.verticalActive = 1080; d.verticalBlanking = 45; d.verticalSyncOffset = 4; d.verticalSyncPulseWidth = 5;
        } else {                          // blanking reduzido generico a 60 Hz
            d.horizontalActive = fW; d.horizontalBlanking = 160; d.horizontalSyncOffset = 48; d.horizontalSyncPulseWidth = 32;
            d.verticalActive = fH; d.verticalBlanking = fH / 24 + 6; d.verticalSyncOffset = 3; d.verticalSyncPulseWidth = 5;
            d.pixelClock = (UInt64)(fW + 160) * (fH + d.verticalBlanking) * 60u;
        }
        d.horizontalSyncConfig = 1; d.verticalSyncConfig = 1;
    }
    d.minPixelClock = d.maxPixelClock = d.pixelClock;
    d.detailedTimingModeID = (UInt32)kModeId;
    d.signalConfig = kIODigitalSignal;
    d.numLinks = 1;
    const UInt64 tot = (UInt64)(d.horizontalActive + d.horizontalBlanking) * (d.verticalActive + d.verticalBlanking);
    UInt64 ns = tot ? tot * 1000000000ull / d.pixelClock : 16666667ull;
    if (ns < 4000000ull || ns > 50000000ull) ns = 16666667ull;
    nanoseconds_to_absolutetime(ns, &fVblPeriod);
    FBLOG("timing %s: %ux%u pclk %llu total %llux%u, vblank a cada %llu us",
          fromEdid ? "do EDID" : "padrao", (unsigned)d.horizontalActive, (unsigned)d.verticalActive, d.pixelClock,
          (unsigned long long)(d.horizontalActive + d.horizontalBlanking), (unsigned)(d.verticalActive + d.verticalBlanking), ns / 1000);
}

void MaxwellFB::publishCaps()
{
    IODisplayScalerInformation si; bzero(&si, sizeof si);
    si.scalerFeatures = 0;
    si.maxHorizontalPixels = fW; si.maxVerticalPixels = fH;
    if (OSData *s = OSData::withBytes(&si, sizeof si)) { setProperty(kIOFBScalerInfoKey, s); s->release(); }

    IODisplayTimingRangeV1 r; bzero(&r, sizeof r);
    r.minPixelClock = 25000000ull; r.maxPixelClock = 600000000ull; r.maxPixelError = 0;
    r.supportedSyncFlags = kIORangeSupportsSeparateSyncs;
    r.supportedSignalLevels = 0; r.supportedSignalConfigs = kIODigitalSignal;
    r.minFrameRate = 23; r.maxFrameRate = 240; r.minLineRate = 10000; r.maxLineRate = 1000000;
    r.maxHorizontalTotal = 16384; r.maxVerticalTotal = 16384;
    r.charSizeHorizontalActive = r.charSizeHorizontalBlanking = r.charSizeHorizontalSyncOffset = r.charSizeHorizontalSyncPulse = 1;
    r.charSizeVerticalActive = r.charSizeVerticalBlanking = r.charSizeVerticalSyncOffset = r.charSizeVerticalSyncPulse = 1;
    r.charSizeHorizontalBorderLeft = r.charSizeHorizontalBorderRight = r.charSizeVerticalBorderTop = r.charSizeVerticalBorderBottom = 1;
    r.charSizeHorizontalTotal = r.charSizeVerticalTotal = 1;
    r.minHorizontalActiveClocks = 320;  r.maxHorizontalActiveClocks = 8192;
    r.minHorizontalBlankingClocks = 0;  r.maxHorizontalBlankingClocks = 8192;
    r.minHorizontalSyncOffsetClocks = 0; r.maxHorizontalSyncOffsetClocks = 8192;
    r.minHorizontalPulseWidthClocks = 1; r.maxHorizontalPulseWidthClocks = 8192;
    r.minVerticalActiveClocks = 200;    r.maxVerticalActiveClocks = 8192;
    r.minVerticalBlankingClocks = 0;    r.maxVerticalBlankingClocks = 8192;
    r.minVerticalSyncOffsetClocks = 0;  r.maxVerticalSyncOffsetClocks = 8192;
    r.minVerticalPulseWidthClocks = 1;  r.maxVerticalPulseWidthClocks = 8192;
    r.maxNumLinks = 1; r.minLink0PixelClock = 25000; r.maxLink0PixelClock = 600000;
    r.supportedPixelEncoding = kIORangePixelEncodingRGB444; r.supportedBitsPerColorComponent = kIORangeBitsPerColorComponent8;
    // o CoreDisplay testa cada formato de sinal contra este range ("fbRange_b"); com EDID CEA-861 (HDMI) os formatos
    // sao BT709: sem BT709 aqui todos eram recusados (0xe00002c7) e o display ficava sem modo. So RGB 8 bits.
    r.supportedColorimetryModes = kIORangeColorimetryNativeRGB | kIORangeColorimetrysRGB | kIORangeColorimetryBT601 | kIORangeColorimetryBT709;
    r.supportedDynamicRangeModes = kIORangeDynamicRangeSDR | kIORangeDynamicRangeTraditionalGammaSDR;
    if (OSData *g = OSData::withBytes(&r, sizeof r)) { setProperty(kIOFBTimingRangeKey, g); g->release(); }

    // o timing para o AGDC (MaxwellAGDC le "NVRMTiming", 13 palavras)
    const IODetailedTimingInformationV2 &d = fNative;
    uint32_t t[NVK5_TIMING_WORDS]; bzero(t, sizeof t);
    const uint32_t htot = d.horizontalActive + d.horizontalBlanking, vtot = d.verticalActive + d.verticalBlanking;
    t[NVK5_T_TAG] = NVK5_TIMING_TAG; t[NVK5_T_PCLK] = (uint32_t)d.pixelClock;
    t[NVK5_T_HVIS] = d.horizontalActive; t[NVK5_T_HSS] = d.horizontalActive + d.horizontalSyncOffset;
    t[NVK5_T_HSE] = t[NVK5_T_HSS] + d.horizontalSyncPulseWidth; t[NVK5_T_HTOT] = htot;
    t[NVK5_T_VVIS] = d.verticalActive; t[NVK5_T_VSS] = d.verticalActive + d.verticalSyncOffset;
    t[NVK5_T_VSE] = t[NVK5_T_VSS] + d.verticalSyncPulseWidth; t[NVK5_T_VTOT] = vtot;
    t[NVK5_T_REFRESH_MHZ] = (uint32_t)((d.pixelClock * 1000ull) / ((uint64_t)htot * vtot));
    t[NVK5_T_HSYNCPOS] = d.horizontalSyncConfig ? 1 : 0; t[NVK5_T_VSYNCPOS] = d.verticalSyncConfig ? 1 : 0;
    if (OSData *k = OSData::withBytes(t, sizeof t)) { setProperty("NVRMTiming", k); k->release(); }

    IOTimingInformation ti; bzero(&ti, sizeof ti);
    ti.appleTimingID = kIOTimingIDInvalid; ti.flags = kIODetailedTimingValid; ti.detailedInfo.v2 = fNative;
    if (OSData *st = OSData::withBytes(&ti, sizeof ti)) { setProperty("startup-timing", st); st->release(); }
}

bool MaxwellFB::start(IOService *provider)
{
    IOService *p = provider;
    for (int up = 0; p && !p->metaCast("IOPCIDevice") && up < 4; up++) p = p->getProvider();
    fPci = p && p->metaCast("IOPCIDevice") ? p : nullptr;
    if (!fPci) { FBLOG("start em %s: sem IOPCIDevice acima", provider->getName()); return false; }
    FBLOG("start em %s (GPU %s)", provider->getName(), fPci->getName());
    OSDictionary *m = IOService::serviceMatching("MaxwellMetal");
    IOService *b = m ? IOService::waitForMatchingService(m, 5ull * 1000 * 1000 * 1000) : nullptr;
    OSSafeReleaseNULL(m);
    if (!b) { FBLOG("sem o servico MaxwellMetal: nao sobe"); return false; }
    fBackend = b;   // referencia do waitForMatchingService
    copyEdid();
    if (!fetchScanout()) { FBLOG("o backend nao deu a superficie home: nao sobe"); return false; }
    fAperture = apertureSub();
    if (!fAperture) { FBLOG("a superficie home (0x%llx) nao esta num BAR da GPU: nao sobe", fPhys); return false; }
    buildNative();
    fVblCall = thread_call_allocate(&MaxwellFB::vblFire, this);
    if (!fVblCall) return false;

    setProperty("IOFBDependentID", (unsigned long long)0x4D58464200000001ull, 64);   // 'MXFB': controlador proprio
    setProperty("IOFBDependentIndex", 0ull, 32);
    setProperty("IOFBMemorySize", 2048ull << 20, 64);
    setProperty("MaxwellFBPhys", fPhys, 64);
    publishCaps();

    if (!IOFramebuffer::start(provider)) { FBLOG("IOFramebuffer::start falhou"); return false; }
    registerPowerDriver(this, kFBPowerStates, 2);
    uint64_t d = 0; clock_get_uptime(&d); fVblNext = d + fVblPeriod;
    thread_call_enter_delayed(fVblCall, fVblNext);
    FBLOG("no ar: %ux%u, EDID %u B, controlador MXFB indice 0", fW, fH, fEdidSize);
    return true;
}

void MaxwellFB::stop(IOService *provider)
{
    OSIncrementAtomic(&fStopping);
    if (fVblCall) { thread_call_cancel(fVblCall); IOSleep(50); }   // vblFire nao rearma com fStopping
    IOFramebuffer::stop(provider);
}

void MaxwellFB::free()
{
    if (fVblCall) {
        OSIncrementAtomic(&fStopping);
        for (int i = 0; i < 20; i++) { thread_call_cancel(fVblCall); if (thread_call_free(fVblCall)) break; IOSleep(20); }
        fVblCall = nullptr;
    }
    OSSafeReleaseNULL(fAperture);
    OSSafeReleaseNULL(fBackend);
    IOFramebuffer::free();
}

void MaxwellFB::vblFire(thread_call_param_t p0, thread_call_param_t)
{
    MaxwellFB *me = (MaxwellFB *)p0;
    if (OSAddAtomic(0, &me->fStopping)) return;
    if (me->fVbl.proc && me->fVbl.enabled) { me->fVbl.proc(me->fVbl.target, me->fVbl.ref); me->fVblCount++; }
    uint64_t now = 0; clock_get_uptime(&now);
    me->fVblNext += me->fVblPeriod;
    if (me->fVblNext <= now) me->fVblNext = now + me->fVblPeriod;
    thread_call_enter_delayed(me->fVblCall, me->fVblNext);
}

IODeviceMemory *MaxwellFB::apertureSub()
{
    for (IOItemCount i = 0; i < fPci->getDeviceMemoryCount(); i++) {
        IODeviceMemory *mem = fPci->getDeviceMemoryWithIndex(i);
        if (!mem) continue;
        const UInt64 pa = mem->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone), len = mem->getLength();
        if (fPhys < pa || fApBytes > len || fPhys - pa > len - fApBytes) continue;
        IOMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(mem, (IOByteCount)(fPhys - pa), (IOByteCount)fApBytes, kIODirectionNone);
        if (sub) FBLOG("abertura: BAR%u (0x%llx, %llu MB) + 0x%llx", (unsigned)i, pa, len >> 20, fPhys - pa);
        return (IODeviceMemory *)sub;
    }
    return nullptr;
}

IODeviceMemory *MaxwellFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !fAperture) return nullptr;
    fAperture->retain();
    return fAperture;
}

IOReturn MaxwellFB::enableController()
{
    FBLOG("enableController: o head ja varre a superficie home");
    return kIOReturnSuccess;
}

IOItemCount MaxwellFB::getDisplayModeCount()
{
    IOItemCount n = 1;
    for (unsigned i = 0; i < fNDT; i++) if ((IODisplayModeID)fDT[i].detailedTimingModeID != kModeId) n++;
    return n;
}

IOReturn MaxwellFB::getDisplayModes(IODisplayModeID *all)
{
    if (!all) return kIOReturnBadArgument;
    unsigned k = 0;
    all[k++] = kModeId;
    for (unsigned i = 0; i < fNDT; i++) if ((IODisplayModeID)fDT[i].detailedTimingModeID != kModeId) all[k++] = (IODisplayModeID)fDT[i].detailedTimingModeID;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (!info) return kIOReturnBadArgument;
    const IODetailedTimingInformationV2 *d = &fNative;
    if (mode != kModeId) { int i = dtIndex(mode); if (i < 0) return kIOReturnBadArgument; d = &fDT[i]; }
    bzero(info, sizeof *info);
    const UInt64 tot = (UInt64)(d->horizontalActive + d->horizontalBlanking) * (d->verticalActive + d->verticalBlanking);
    info->nominalWidth = fW; info->nominalHeight = fH;
    info->refreshRate = tot ? (UInt32)((d->pixelClock << 16) / tot) : (60u << 16);
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeAcceleratorBackedFlag;
    if (mode == kModeId || mode == fCurId) info->flags |= kDisplayModeDefaultFlag | kDisplayModeNativeFlag;
    info->imageWidth = fMmW; info->imageHeight = fMmH;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    if (!info) return kIOReturnUnsupportedMode;
    const IODetailedTimingInformationV2 *d = &fNative;
    if (mode != kModeId) { int i = dtIndex(mode); if (i < 0) return kIOReturnUnsupportedMode; d = &fDT[i]; }
    bzero(info, sizeof *info);
    info->appleTimingID = kIOTimingIDInvalid;
    info->flags = kIODetailedTimingValid;
    info->detailedInfo.v2 = *d;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    if (mode != kModeId && dtIndex(mode) < 0) return kIOReturnUnsupportedMode;
    bzero(pi, sizeof *pi);
    pi->activeWidth = fW; pi->activeHeight = fH; pi->bytesPerRow = fPitch; pi->bytesPerPlane = 0;
    pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels; pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00ff0000; pi->componentMasks[1] = 0x0000ff00; pi->componentMasks[2] = 0x000000ff;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof pi->pixelFormat);
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    if (mode) *mode = fCurId;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    if (depth != 0) return kIOReturnUnsupportedMode;
    if (mode != kModeId) {
        int i = dtIndex(mode);
        if (i < 0 || !rasterMatches(&fDT[i])) { FBLOG("setDisplayMode 0x%x: recusado", (unsigned)mode); return kIOReturnUnsupportedMode; }
    }
    if (mode != fCurId) FBLOG("setDisplayMode 0x%x (era 0x%x): o mesmo raster, sem modeset", (unsigned)mode, (unsigned)fCurId);
    fCurId = mode;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::validateDetailedTiming(void *description, IOByteCount descripSize)
{
    if (!description) return kIOReturnBadArgument;
    IODetailedTimingInformationV2 *d = nullptr;
    if (descripSize == sizeof(IOFBDisplayModeDescription)) d = &((IOFBDisplayModeDescription *)description)->timingInfo.detailedInfo.v2;
    else if (descripSize == sizeof(IODetailedTimingInformationV2)) d = (IODetailedTimingInformationV2 *)description;
    if (!d) return kIOReturnBadArgument;
    const bool ok = rasterMatches(d);
    if (fValidateLog < 40 || ok) {
        fValidateLog++;
        FBLOG("validateDetailedTiming id 0x%x %ux%u%s pclk %llu -> %s", (unsigned)d->detailedTimingModeID,
              (unsigned)d->horizontalActive, (unsigned)d->verticalActive, d->horizontalScaled ? " (escalado)" : "",
              d->pixelClock, ok ? "OK" : "recusado");
    }
    if (!ok) return kIOReturnUnsupportedMode;
    if (!d->minPixelClock) d->minPixelClock = d->pixelClock;
    if (!d->maxPixelClock) d->maxPixelClock = d->pixelClock;
    if (descripSize == sizeof(IOFBDisplayModeDescription)) {
        IODisplayModeInformation *info = &((IOFBDisplayModeDescription *)description)->info;
        const UInt64 tot = (UInt64)(d->horizontalActive + d->horizontalBlanking) * (d->verticalActive + d->verticalBlanking);
        info->maxDepthIndex = 0;
        info->nominalWidth = fW; info->nominalHeight = fH;
        info->refreshRate = tot ? (UInt32)((d->pixelClock << 16) / tot) : 0;
        info->flags |= kDisplayModeAcceleratorBackedFlag;
    }
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::setDetailedTimings(OSArray *array)
{
    if (!array) { fNDT = 0; removeProperty(kIOFBDetailedTimingsKey); return kIOReturnSuccess; }
    unsigned n = 0, kept = 0;
    for (unsigned i = 0; i < array->getCount() && n < kMaxDT; i++) {
        OSData *data = OSDynamicCast(OSData, array->getObject(i));
        if (!data || data->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
        bcopy(data->getBytesNoCopy(), &fDT[n], sizeof(IODetailedTimingInformationV2));
        if (rasterMatches(&fDT[n])) kept++;
        n++;
    }
    fNDT = n;
    setProperty(kIOFBDetailedTimingsKey, array);
    if (fCurId != kModeId && dtIndex(fCurId) < 0) fCurId = kModeId;
    FBLOG("setDetailedTimings: %u timing(s), %u no nosso raster", n, kept);
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::getAttribute(IOSelect attribute, uintptr_t *value)
{
    if (attribute == kIOHardwareCursorAttribute) { if (value) *value = 0; return kIOReturnSuccess; }
    if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
    return IOFramebuffer::getAttribute(attribute, value);
}

IOReturn MaxwellFB::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
    if (attribute == kIOPowerAttribute) {
        FBLOG("power %lu", (unsigned long)value);
        if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn); return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
        handleEvent(kIOFBNotifyWillPowerOff);
        IOReturn r = IOFramebuffer::setAttribute(attribute, value);
        return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
    }
    return IOFramebuffer::setAttribute(attribute, value);
}

IOReturn MaxwellFB::setPowerState(unsigned long ordinal, IOService *)
{
    FBLOG("setPowerState %lu", ordinal);
    handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
    return kIOPMAckImplied;
}

IOReturn MaxwellFB::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
    case kConnectionEnable:
    case kConnectionCheckEnable:               if (value) *value = 1; return kIOReturnSuccess;
    case kConnectionFlags:                     if (value) *value = 0; return kIOReturnSuccess;
    case kConnectionColorModesSupported:       if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerColorDepth:      if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionControllerDepthsSupported: if (value) *value = 0x00000002; return kIOReturnSuccess;
    case kConnectionColorMode:                 if (value) *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionDisplayFlags:              if (value) *value = 0; return kIOReturnSuccess;
    case kConnectionSupportsHLDDCSense:        return fEdidSize >= 128 ? kIOReturnSuccess : kIOReturnUnsupported;
    case kConnectionDisplayParameterCount:     if (value) *value = 0; return kIOReturnSuccess;
    case kConnectionDisplayParameters:         return kIOReturnSuccess;
    case kConnectionSupportsAppleSense:
    case kConnectionSupportsLLDDCSense:        return kIOReturnUnsupported;
    default:                                   return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn MaxwellFB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value)
{
    switch (attribute) {
    case kConnectionPower:
    case kConnectionColorModesSupported:
    case kConnectionColorDepthsSupported:
    case kConnectionControllerColorDepth:
    case kConnectionControllerDepthsSupported:
    case kConnectionColorMode:
    case kConnectionDisplayFlags:
    case kConnectionFlags:
        return kIOReturnSuccess;
    default:
        return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn MaxwellFB::getDDCBlock(IOIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits, UInt8 *data, IOByteCount *length)
{
    if (blockType != kIODDCBlockTypeEDID || blockNumber == 0 || blockNumber * 128 > fEdidSize || !data || !length) return kIOReturnUnsupported;
    if (*length < 128) return kIOReturnBadArgument;
    bcopy(fEdid + (blockNumber - 1) * 128, data, 128);
    *length = 128;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **interruptRef)
{
    if (type == kIOFBVBLInterruptType) {
        fVbl.proc = proc; fVbl.target = target; fVbl.ref = ref; fVbl.enabled = 1;
        if (interruptRef) *interruptRef = &fVbl;
        FBLOG("vblank registrado");
        return kIOReturnSuccess;
    }
    if (type == kIOFBConnectInterruptType) {
        fConnect.proc = proc; fConnect.target = target; fConnect.ref = ref; fConnect.enabled = 1;
        if (interruptRef) *interruptRef = &fConnect;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn MaxwellFB::unregisterInterrupt(void *interruptRef)
{
    MXInterrupt *i = (MXInterrupt *)interruptRef;
    if (i != &fVbl && i != &fConnect) return kIOReturnBadArgument;
    i->enabled = 0; i->proc = nullptr;
    return kIOReturnSuccess;
}

IOReturn MaxwellFB::setInterruptState(void *interruptRef, UInt32 state)
{
    MXInterrupt *i = (MXInterrupt *)interruptRef;
    if (i != &fVbl && i != &fConnect) return kIOReturnBadArgument;
    i->enabled = state;
    return kIOReturnSuccess;
}
