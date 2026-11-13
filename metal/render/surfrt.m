/*
 * surfrt - desenha numa IOSurface (alvo de render) e le de volta pela CPU, como o WindowServer faz com a tela.
 * Exercita: importacao da VRAM da familia, marca "mais novo na VRAM" e o page-off no IOSurfaceLock.
 * Compilar no hackintosh com o nome mtlprobe (liberado na lista do plugin):
 *   clang -fobjc-arc -o mtlprobe surfrt.m -framework Metal -framework IOSurface -framework Foundation
 * Uso: ./mtlprobe [largura altura [quadros]]
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <mach/mach_time.h>

static double ms(uint64_t a, uint64_t b) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (double)(b - a) * t.numer / t.denom / 1e6; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        int W = argc > 2 ? atoi(argv[1]) : 256, H = argc > 2 ? atoi(argv[2]) : 256, N = argc > 3 ? atoi(argv[3]) : 4;
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"GTX"] || [d.name containsString:@"NVK"]) dev = d;
        if (!dev) { printf("sem dispositivo Metal da GTX 960\n"); return 2; }
        printf("dispositivo: %s\n", dev.name.UTF8String);
        NSDictionary *props = @{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H), (id)kIOSurfaceBytesPerElement: @4,
                                 (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        printf("IOSurface %u: %dx%d linha %zu bytes, %zu B\n", IOSurfaceGetID(s), W, H, IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s));
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td iosurface:s plane:0];
        if (!tex) { printf("newTextureWithDescriptor:iosurface: falhou\n"); return 3; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        for (int i = 0; i < N; i++) {
            double r = (i & 1) ? 1.0 : 0.0, g = (i & 2) ? 1.0 : 0.0, b = (i & 4) ? 1.0 : 0.25;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);
            uint64_t t0 = mach_absolute_time();
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint64_t t1 = mach_absolute_time();
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            uint64_t t2 = mach_absolute_time();
            const uint8_t *base = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s);
            uint32_t first = *(const uint32_t *)base, mid = *(const uint32_t *)(base + (size_t)(H / 2) * row + (size_t)(W / 2) * 4),
                     last = *(const uint32_t *)(base + (size_t)(H - 1) * row + (size_t)(W - 1) * 4);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            uint32_t want = 0xff000000u | ((uint32_t)(r * 255 + .5) << 16) | ((uint32_t)(g * 255 + .5) << 8) | (uint32_t)(b * 255 + .5);
            int ok = first == want && mid == want && last == want;
            bad += !ok;
            printf("quadro %d: esperado 0x%08x lido 0x%08x 0x%08x 0x%08x %s | render %.2f ms, lock %.2f ms\n", i, want, first, mid, last,
                   ok ? "ok" : "ERRADO", ms(t0, t1), ms(t1, t2));
        }
        printf("%s\n", bad ? "FALHOU" : "PASSOU");
        return bad ? 1 : 0;
    }
}
