/*
 * mtlprobe - o jeito normal de um app achar GPUs: MTLCopyAllDevices(). Lista os dispositivos e roda um
 * kernel MSL no que for NVIDIA. O nome "mtlprobe" esta na lista de processos liberados do plugin.
 * Uso: sudo ./mtlprobe
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int main(void)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("[mtl] MTLCopyAllDevices: %lu dispositivo(s)\n", (unsigned long)all.count);
        id<MTLDevice> nv = nil;
        for (id<MTLDevice> d in all) {
            printf("[mtl]   \"%s\" registryID 0x%llx lowPower %d removable %d\n", d.name.UTF8String,
                   (unsigned long long)d.registryID, d.isLowPower, d.isRemovable);
            if ([d.name rangeOfString:@"GTX 960"].location != NSNotFound) nv = d;
        }
        id<MTLDevice> def = MTLCreateSystemDefaultDevice();
        printf("[mtl] MTLCreateSystemDefaultDevice: \"%s\"\n", def ? def.name.UTF8String : "(nenhum)");
        if (!nv) { printf("RESULTADO: GTX 960 nao apareceu no MTLCopyAllDevices\n"); return 1; }

        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                         "kernel void fill(device uint *d [[buffer(0)]], uint i [[thread_position_in_grid]]) { d[i] = i * 5u + 1u; }\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [nv newLibraryWithSource:src options:nil error:&err];
        id<MTLComputePipelineState> ps = lib ? [nv newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err] : nil;
        if (!ps) { printf("[mtl] pipeline FALHOU: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLBuffer> b = [nv newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [[nv newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps]; [e setBuffer:b offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t *d = b.contents, bad = 0;
        for (uint32_t i = 0; i < 1024; i++) if (d[i] != i * 5u + 1u) bad++;
        printf("[mtl] kernel na GTX 960: status %ld, d[0..3] = %u %u %u %u, errados %u\n", (long)cb.status, d[0], d[1], d[2], d[3], bad);
        printf("RESULTADO: %s\n", (!bad && cb.status == MTLCommandBufferStatusCompleted) ? "ok - Metal do sistema enxerga e usa a GTX 960" : "falhou");
        return bad ? 1 : 0;
    }
}
