/*
 * MaxwellMetal.kext - habilita o Metal da GTX 960 (Web Driver r387 + plugin NVMTLDriver + NVK/libnvrm387).
 *
 * Sem nenhuma logica de GPU propria:
 *  1. Ouvinte kauth no escopo "com.nvidia.NVDA": libera a acao 0 (NVDA::newUserClient) para qualquer
 *     usuario, para o NVK (pelo user client NVDAUser) funcionar em apps comuns, sem root. Sem isso o
 *     escopo da NVIDIA so libera uid 0. Implicacao: todo processo local pode falar com o RM.
 *  2. sysctl debug.maxwell_metal (escrita so como root): 1 grava MetalPluginName=NVMTLDriver e
 *     MetalPluginClassName=NVMTLDevice no nvAccelerator do GeForceWeb; 0 remove. No boot fica 0, entao o
 *     WindowServer nunca encontra o plugin; arma-se depois do login.
 *  3. Ponte para apps com sandbox: este servico e um IOAccelerator (a App Sandbox deixa abrir qualquer user
 *     client de um acelerador; o NVDAUser ela nega). IOServiceOpen tipo 'MXRM' cria um IOAccelMaxwellRMClient
 *     que, ja no kernel, pede ao NVDA um NVDAUser em nome da task do app e repassa as chamadas para ele. O RM
 *     continua fazendo copyin/copyout na task chamadora, como no NVDAUser aberto direto.
 *
 *  4. Backend de GPU no kernel (MaxwellBackend.cpp, etapa B1 do porte do NVAccel): VRAM, VA da GPU e a tela pelo
 *     canal base EVO, sobre a API NvRm* do NVDAResmanWeb. Aberto so pelo cliente de teste 'MXBK' (root).
 *
 * Interruptor: com "-mmeoff" nos boot-args o kext nao publica nada (start falha).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <sys/kauth.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include "MaxwellBackend.h"
#include <pexpert/pexpert.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define LOG(fmt, ...) IOLog("MaxwellMetal: " fmt "\n", ##__VA_ARGS__)

#define MM_BRIDGE_TYPE   0x4D58524Du   /* 'MXRM' */
#define NVDAUSER_TYPE    0x80000000u
#define MM_SEL_SURFSHARE 100u

/* IOGraphicsFamily: IOAccelerator nao acrescenta membros nem virtuais a IOService (so metaclasse e
 * helpers estaticos de AccelID), entao esta declaracao basta para herdar dele. */
class IOAccelerator : public IOService {
    OSDeclareDefaultStructors(IOAccelerator)
};

class IOAccelMaxwellRMClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelMaxwellRMClient)
    IOUserClient *fInner;
    IOLock *fLock;
    task_t fTask;
public:
    bool initBridge(task_t task, void *securityID, UInt32 type, IOUserClient *inner);
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientClose(void) override;
    IOReturn clientDied(void) override;
    void free(void) override;
private:
    IOUserClient *takeInner(void);
    IOReturn surfShare(IOExternalMethodArguments *args);
};
OSDefineMetaClassAndStructors(IOAccelMaxwellRMClient, IOUserClient)

class MaxwellMetalBridge;

/* 0.6.0: o servico principal e um IOService comum. Como IOAccelerator, o GPUWrangler do Tahoe o registrava como uma
 * segunda GPU sem PCI e o CoreDisplay o associava a tela. A ponte para apps com sandbox virou o MaxwellMetalBridge
 * (IOAccelerator), publicado so sem o acelerador (sem nvaccel=1); com ele, a ponte e o proprio NVAccel. */
class MaxwellMetal : public IOService {
    OSDeclareDefaultStructors(MaxwellMetal)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
    IOReturn newBridgeClient(task_t task, void *securityID, UInt32 type, IOAccelMaxwellRMClient **out);
private:
    void copyPciIds(IOService *nvda);
    bool fIds;
    MaxwellMetalBridge *fBridge;
};
OSDefineMetaClassAndStructors(MaxwellMetal, IOService)

class MaxwellMetalBridge : public IOAccelerator {
    OSDeclareDefaultStructors(MaxwellMetalBridge)
public:
    IOReturn newUserClient(task_t t, void *sid, UInt32 type, IOUserClient **h) override
    {
        MaxwellMetal *m = OSDynamicCast(MaxwellMetal, getProvider());
        return m ? m->newUserClient(t, sid, type, h) : kIOReturnNotAttached;
    }
};
OSDefineMetaClassAndStructors(MaxwellMetalBridge, IOAccelerator)

static kauth_listener_t gListener;
static int gArmed;
static bool gSysctl;
static volatile SInt32 gBridges;

static int nvda_listener(kauth_cred_t cred, void *idata, kauth_action_t action,
                         uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    (void)cred; (void)idata; (void)a0; (void)a1; (void)a2; (void)a3;
    return action == 0 ? KAUTH_RESULT_ALLOW : KAUTH_RESULT_DEFER;
}

static bool boot_arg_off(void)
{
    bool off = false;
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (opts) {
        OSObject *o = opts->getProperty("boot-args");
        OSData *d = OSDynamicCast(OSData, o);
        OSString *s = OSDynamicCast(OSString, o);
        const char *p = nullptr; unsigned len = 0;
        char buf[1024];
        if (d) { p = (const char *)d->getBytesNoCopy(); len = d->getLength(); }
        else if (s) { p = s->getCStringNoCopy(); len = s->getLength(); }
        if (p && len) {
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, p, len); buf[len] = 0;
            static const char k[] = "-mmeoff";
            for (unsigned i = 0; !off && i + sizeof(k) - 1 <= len; i++)
                off = memcmp(buf + i, k, sizeof(k) - 1) == 0 &&
                      (i == 0 || buf[i - 1] == ' ') &&
                      (buf[i + sizeof(k) - 1] == 0 || buf[i + sizeof(k) - 1] == ' ');
        }
        opts->release();
    }
    return off;
}

static IOService *copy_service(const char *cls)
{
    OSDictionary *m = IOService::serviceMatching(cls);
    if (!m) return nullptr;
    IOService *s = IOService::copyMatchingService(m);
    m->release();
    return s;
}

static IOReturn set_plugin(bool on)
{
    IOService *acc = copy_service("nvAccelerator");
    if (!acc) return kIOReturnNotFound;
    if (on) {
        acc->setProperty("MetalPluginClassName", "NVMTLDevice");
        acc->setProperty("MetalPluginName", "NVMTLDriver");
    } else {
        acc->removeProperty("MetalPluginName");
        acc->removeProperty("MetalPluginClassName");
    }
    acc->release();
    return kIOReturnSuccess;
}

static int sysctl_maxwell_metal(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
    (void)arg1; (void)arg2;
    int v = gArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    IOReturn r = set_plugin(v == 1);
    if (r != kIOReturnSuccess) { LOG("nvAccelerator nao encontrado (0x%x)", r); return ENODEV; }
    gArmed = v;
    LOG("MetalPluginName %s", v ? "gravado (NVMTLDriver/NVMTLDevice)" : "removido");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, maxwell_metal, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
            sysctl_maxwell_metal, "I", "1 = MetalPluginName no nvAccelerator (GTX 960), 0 = remove");

SYSCTL_INT(_debug, OID_AUTO, mxflip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gMxFlip, 0,
           "flip do canal base: 1 intervalo 1, 2 zera notifier/semaforo, 4 CSC base identidade, 8 LUT do core, 0x10 imediato, 0x20 CSC do core");
SYSCTL_INT(_debug, OID_AUTO, mxflip_storage, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gMxFlipStorage, 0,
           "SURFACE_SET_STORAGE do flip (0 = pitch da superficie; 0x7804 = block-linear 16 GOBs pitch 0x78, o do core do NVDA)");
SYSCTL_INT(_debug, OID_AUTO, mxflip_core_storage, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipCoreStorage, 0, "HEAD_SET_STORAGE em uso no core do head 0, lido no ultimo flip");
SYSCTL_INT(_debug, OID_AUTO, mxflip_iso, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipIso, 0, "ISO em uso no canal base depois do ultimo flip (0 = nao armou)");
SYSCTL_INT(_debug, OID_AUTO, mxflip_exc, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipExc, 0, "excecao do canal base (tipo 14:12, metodo 11:0)");
SYSCTL_INT(_debug, OID_AUTO, mxflip_code, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipCode, 0, "codigo da excecao do canal base");
SYSCTL_INT(_debug, OID_AUTO, mxflip_count, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipCount, 0, "flips de superficie (fora a home) pedidos ao canal base");
SYSCTL_INT(_debug, OID_AUTO, mxflip_stuck, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxFlipStuck, 0, "flips que o canal base nao consumiu em 200 ms");
SYSCTL_INT(_debug, OID_AUTO, mx_vram_mapped_kb, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxVramMappedKB, 0, "VRAM de superficies mapeada na BAR1, em KB (orcamento 160 MB)");
SYSCTL_INT(_debug, OID_AUTO, mx_vram_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxVramRefused, 0, "pedidos de VRAM recusados por passar do orcamento da BAR1");
SYSCTL_INT(_debug, OID_AUTO, mx_vram_unmapped_kb, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxVramUnmappedKB, 0, "VRAM de superficies sem mapa na BAR1 (linha larga), em KB (orcamento 640 MB)");
SYSCTL_INT(_debug, OID_AUTO, mx_vram_temp_maps, CTLFLAG_RD | CTLFLAG_LOCKED, &gMxVramTempMaps, 0, "mapas temporarios de superficie sem mapa (leitura pela CPU)");

/* ---------------------------------------------------------------------------------------- */
/* ponte                                                                                     */
/* ---------------------------------------------------------------------------------------- */
bool IOAccelMaxwellRMClient::initBridge(task_t task, void *securityID, UInt32 type, IOUserClient *inner)
{
    if (!inner || !IOUserClient::initWithTask(task, securityID, type)) return false;
    fLock = IOLockAlloc();
    if (!fLock) return false;
    fInner = inner;
    fTask = task;
    return true;
}

IOUserClient *IOAccelMaxwellRMClient::takeInner(void)
{
    IOUserClient *in = nullptr;
    if (fLock) IOLockLock(fLock);
    in = fInner; fInner = nullptr;
    if (fLock) IOLockUnlock(fLock);
    return in;
}

IOReturn IOAccelMaxwellRMClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                                IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (selector == MM_SEL_SURFSHARE) return surfShare(args);
    IOUserClient *in = nullptr;
    IOLockLock(fLock);
    in = fInner;
    if (in) in->retain();
    IOLockUnlock(fLock);
    if (!in) return kIOReturnNotAttached;
    IOReturn r = in->externalMethod(selector, args, nullptr, nullptr, nullptr);
    in->release();
    return r;
}

/* Escapes de superficie do NVK (NVRM_ESC_SURF_VRAM 0xE2 / SURF_DIRTY 0xE3), vindos da libnvrm387: vao para o
 * acelerador (MaxwellAccel, classe NVAccel), que acha a memoria do IOSurface e pede ao backend o dup no cliente do NVK. */
IOReturn IOAccelMaxwellRMClient::surfShare(IOExternalMethodArguments *args)
{
    struct SS { uint32_t version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad; uint64_t size; } ss;
    if (!args || args->scalarInputCount != 1 || args->structureInputSize != sizeof(ss) ||
        args->structureOutputSize != sizeof(ss) || !args->structureInput || !args->structureOutput) return kIOReturnBadArgument;
    memcpy(&ss, args->structureInput, sizeof(ss));
    uint32_t cmd = (uint32_t)args->scalarInput[0];
    if (cmd != 0xE2u && cmd != 0xE3u) return kIOReturnBadArgument;
    ss.status = 0x80000002u;
    IOService *acc = copy_service("NVAccel");
    if (acc) {
        const OSSymbol *sym = OSSymbol::withCString("nvSurfVram");
        if (sym) { acc->callPlatformFunction(sym, false, &ss, (void *)fTask, (void *)(uintptr_t)cmd, nullptr); sym->release(); }
        acc->release();
    }
    memcpy(args->structureOutput, &ss, sizeof(ss));
    return kIOReturnSuccess;
}

IOReturn IOAccelMaxwellRMClient::clientClose(void)
{
    IOUserClient *in = takeInner();
    if (in) { in->clientClose(); in->release(); OSDecrementAtomic(&gBridges); }
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn IOAccelMaxwellRMClient::clientDied(void)
{
    IOUserClient *in = takeInner();
    if (in) { in->clientDied(); in->release(); OSDecrementAtomic(&gBridges); }
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void IOAccelMaxwellRMClient::free(void)
{
    IOUserClient *in = takeInner();
    if (in) { in->clientClose(); in->release(); OSDecrementAtomic(&gBridges); }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    IOUserClient::free();
}

void MaxwellMetal::copyPciIds(IOService *nvda)
{
    if (fIds) return;
    static const char *const keys[] = { "vendor-id", "device-id" };
    for (const char *k : keys) {
        OSObject *o = nvda->copyProperty(k);
        if (o) { setProperty(k, o); o->release(); }
    }
    fIds = true;
}

IOReturn MaxwellMetal::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (type == MM_BACKEND_TEST_TYPE) {
        IOUserClient *t = mmBackendNewTestClient(this, owningTask, securityID, type);
        if (!t) return kIOReturnNotPrivileged;
        *handler = t;
        return kIOReturnSuccess;
    }
    if (type != MM_BRIDGE_TYPE) return kIOReturnUnsupported;
    IOAccelMaxwellRMClient *uc = nullptr;
    IOReturn r = newBridgeClient(owningTask, securityID, type, &uc);
    if (r != kIOReturnSuccess) return r;
    if (!uc->attach(this)) { uc->release(); return kIOReturnInternalError; }
    if (!uc->start(this)) { uc->detach(this); uc->release(); return kIOReturnInternalError; }
    *handler = uc;
    return kIOReturnSuccess;
}

IOReturn MaxwellMetal::newBridgeClient(task_t owningTask, void *securityID, UInt32 type, IOAccelMaxwellRMClient **out)
{
    *out = nullptr;
    IOService *nvda = copy_service("NVDA");
    if (!nvda) { LOG("ponte: servico NVDA ausente"); return kIOReturnNotFound; }
    copyPciIds(nvda);
    IOUserClient *inner = nullptr;
    IOReturn r = nvda->newUserClient(owningTask, securityID, NVDAUSER_TYPE, &inner);
    nvda->release();
    if (r != kIOReturnSuccess || !inner) {
        LOG("ponte: NVDA::newUserClient(0x%x) recusou 0x%x", NVDAUSER_TYPE, r);
        if (inner) { inner->clientClose(); inner->release(); }
        return r ? r : kIOReturnNoResources;
    }
    IOAccelMaxwellRMClient *uc = OSTypeAlloc(IOAccelMaxwellRMClient);
    if (!uc || !uc->initBridge(owningTask, securityID, type, inner)) {
        if (uc) uc->release(); else { inner->clientClose(); inner->release(); }
        return kIOReturnNoMemory;
    }
    SInt32 n = OSIncrementAtomic(&gBridges) + 1;
    static volatile SInt32 logged;
    if (OSIncrementAtomic(&logged) < 8) LOG("ponte: cliente RM aberto pela ponte (%d ativos)", (int)n);
    *out = uc;
    return kIOReturnSuccess;
}

/* ABI do backend que o acelerador portado (MaxwellAccel, NVAccel do NullMoth) chama. nvNewAccelClient fica aqui
 * porque e a ponte: o acelerador pede um cliente RM para a task do Metal e o anexa ele mesmo. */
IOReturn MaxwellMetal::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo("nvNewAccelClient")) {
        IOUserClient **out = (IOUserClient **)p3;
        if (!p1 || !out) return kIOReturnBadArgument;
        IOAccelMaxwellRMClient *uc = nullptr;
        IOReturn r = newBridgeClient((task_t)p1, nullptr, MM_BRIDGE_TYPE, &uc);
        *out = uc;
        return r;
    }
    IOReturn r = mmBackendPlatformFunction(fn, p1, p2);
    if (r != kIOReturnUnsupported) return r;
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

bool MaxwellMetal::start(IOService *provider)
{
    if (boot_arg_off()) {
        LOG("-mmeoff nos boot-args: nada publicado");
        return false;
    }
    if (!IOService::start(provider)) return false;
    gListener = kauth_listen_scope("com.nvidia.NVDA", nvda_listener, nullptr);
    LOG("ouvinte kauth em com.nvidia.NVDA %s", gListener ? "ativo (NVDAUser liberado sem root)" : "FALHOU");
    sysctl_register_oid(&sysctl__debug_maxwell_metal);
    sysctl_register_oid(&sysctl__debug_mxflip); sysctl_register_oid(&sysctl__debug_mxflip_iso);
    sysctl_register_oid(&sysctl__debug_mxflip_storage); sysctl_register_oid(&sysctl__debug_mxflip_core_storage);
    sysctl_register_oid(&sysctl__debug_mxflip_exc); sysctl_register_oid(&sysctl__debug_mxflip_code);
    sysctl_register_oid(&sysctl__debug_mxflip_count); sysctl_register_oid(&sysctl__debug_mxflip_stuck);
    sysctl_register_oid(&sysctl__debug_mx_vram_mapped_kb); sysctl_register_oid(&sysctl__debug_mx_vram_refused);
    sysctl_register_oid(&sysctl__debug_mx_vram_unmapped_kb); sysctl_register_oid(&sysctl__debug_mx_vram_temp_maps);
    gSysctl = true;
    LOG("sysctl debug.maxwell_metal pronto (0 = desarmado); ponte RM para sandbox: IOServiceOpen tipo 'MXRM'");
    mmBackendInit();
    registerService();
    int acc = 0;
    if (!PE_parse_boot_argn("nvaccel", &acc, sizeof acc) || !acc) {
        fBridge = new MaxwellMetalBridge;
        if (fBridge && fBridge->init() && fBridge->attach(this)) { fBridge->registerService(); LOG("ponte para sandbox: MaxwellMetalBridge publicado"); }
        else if (fBridge) { fBridge->release(); fBridge = nullptr; }
    } else LOG("nvaccel=%d: sem MaxwellMetalBridge (a ponte e o NVAccel)", acc);
    return true;
}

void MaxwellMetal::stop(IOService *provider)
{
    if (gArmed) { set_plugin(false); gArmed = 0; }
    if (gSysctl) {
        sysctl_unregister_oid(&sysctl__debug_maxwell_metal);
        sysctl_unregister_oid(&sysctl__debug_mxflip); sysctl_unregister_oid(&sysctl__debug_mxflip_iso);
        sysctl_unregister_oid(&sysctl__debug_mxflip_storage); sysctl_unregister_oid(&sysctl__debug_mxflip_core_storage);
        sysctl_unregister_oid(&sysctl__debug_mxflip_exc); sysctl_unregister_oid(&sysctl__debug_mxflip_code);
        sysctl_unregister_oid(&sysctl__debug_mxflip_count); sysctl_unregister_oid(&sysctl__debug_mxflip_stuck);
        sysctl_unregister_oid(&sysctl__debug_mx_vram_mapped_kb); sysctl_unregister_oid(&sysctl__debug_mx_vram_refused);
        sysctl_unregister_oid(&sysctl__debug_mx_vram_unmapped_kb); sysctl_unregister_oid(&sysctl__debug_mx_vram_temp_maps);
        gSysctl = false;
    }
    if (gListener) { kauth_unlisten_scope(gListener); gListener = nullptr; }
    if (fBridge) { fBridge->terminate(); fBridge->detach(this); fBridge->release(); fBridge = nullptr; }
    mmBackendShutdown();
    LOG("parado");
    IOService::stop(provider);
}
