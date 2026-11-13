/*
 * nvmtltest - harness do Metal na GTX 960 sem mexer no sistema:
 *   carrega o plugin NVMTLDriver (NullMoth, compilado aqui) por dlopen, cria o MTLDevice com
 *   -[NVMTLDevice initWithAcceleratorPort:] sobre o nvAccelerator do GeForceWeb (o acelerador da
 *   IOAcceleratorFamily2 que ja esta carregado) e roda um kernel MSL pela API Metal normal.
 * O plugin usa o NVK (libvulkan_nouveau.dylib) sobre a libnvrm387; por isso roda como root.
 * O nome do executavel (nvmtltest) esta na lista de processos liberados do plugin.
 *
 * Uso: sudo ./nvmtltest [caminho do NVMTLDriver]
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

@interface NSObject (NVMTLInit)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

#define N 1024u
static double ms_since(NSDate *t0) { return -[t0 timeIntervalSinceNow] * 1000.0; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
        const char *bundle = argc > 1 ? argv[1] : "/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver";

        void *h = dlopen(bundle, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("[metal] dlopen do plugin: %s\n", dlerror()); return 1; }
        Class cls = objc_getClass("NVMTLDevice");
        printf("[metal] plugin carregado; classe NVMTLDevice %s\n", cls ? "presente" : "AUSENTE");
        if (!cls) return 1;

        io_service_t acc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("nvAccelerator"));
        printf("[metal] nvAccelerator (GeForceWeb): %s\n", acc ? "encontrado" : "AUSENTE");
        if (!acc) return 1;

        NSDate *t0 = [NSDate date];
        id<MTLDevice> dev = [[cls alloc] initWithAcceleratorPort:acc];
        printf("[metal] initWithAcceleratorPort: %s (%.0f ms)\n", dev ? "OK" : "nil (ver /tmp/nvmtl.log)", ms_since(t0));
        if (!dev) return 1;
        printf("[metal] MTLDevice: \"%s\"  registryID 0x%llx  maxThreadsPerThreadgroup %lu  unifiedMemory %d\n",
               dev.name.UTF8String, (unsigned long long)dev.registryID,
               (unsigned long)dev.maxThreadsPerThreadgroup.width, dev.hasUnifiedMemory);

        NSString *src =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "kernel void fill(device uint *d [[buffer(0)]], uint i [[thread_position_in_grid]])\n"
             "{\n"
             "    d[i] = i * 3u + 7u;\n"
             "}\n";
        NSError *err = nil;
        t0 = [NSDate date];
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        printf("[metal] newLibraryWithSource (MSL): %s (%.0f ms)%s%s\n", lib ? "OK" : "FALHOU", ms_since(t0),
               err ? " - " : "", err ? err.localizedDescription.UTF8String : "");
        if (!lib) return 1;
        id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
        printf("[metal] newFunctionWithName(fill): %s\n", fn ? "OK" : "FALHOU");
        if (!fn) return 1;
        t0 = [NSDate date];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        printf("[metal] newComputePipelineState (AIR -> SPIR-V -> NAK SM 5.2): %s (%.0f ms)%s%s\n",
               ps ? "OK" : "FALHOU", ms_since(t0), err ? " - " : "", err ? err.localizedDescription.UTF8String : "");
        if (!ps) return 1;

        id<MTLBuffer> buf = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
        printf("[metal] newBuffer %u bytes (shared): %s\n", N * 4, buf ? "OK" : "FALHOU");
        if (!buf) return 1;
        uint32_t *d = (uint32_t *)buf.contents;
        for (uint32_t i = 0; i < N; i++) d[i] = 0xDEADBEEF;

        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        t0 = [NSDate date];
        [cb commit];
        [cb waitUntilCompleted];
        printf("[metal] commit + waitUntilCompleted: status %ld (%.2f ms)%s%s\n", (long)cb.status, ms_since(t0),
               cb.error ? " - " : "", cb.error ? cb.error.localizedDescription.UTF8String : "");

        uint32_t bad = 0, first = UINT32_MAX;
        for (uint32_t i = 0; i < N; i++) if (d[i] != i * 3u + 7u) { if (first == UINT32_MAX) first = i; bad++; }
        printf("[metal] d[0..5] = %u %u %u %u %u %u   d[1023] = %u\n", d[0], d[1], d[2], d[3], d[4], d[5], d[1023]);
        if (bad) printf("[metal] ERRADOS: %u de %u (primeiro em %u: 0x%08x)\n", bad, N, first, d[first]);
        else printf("[metal] => OS %u VALORES CONFEREM: A GTX 960 RODOU UM KERNEL METAL\n", N);
        printf("RESULTADO: %s\n", (cb.status == MTLCommandBufferStatusCompleted && !bad) ? "ok" : "falhou");
        return bad ? 1 : 0;
    }
}
