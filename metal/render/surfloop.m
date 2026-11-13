// Repete o que o WindowServer faz com video: embrulha a MESMA IOSurface de tela numa MTLTexture nova a cada quadro,
// desenha nela, solta. Mostra a cada bloco: descritores abertos, tempo por volta, primeira falha.
// Uso: surfloop [LARG ALT [VOLTAS [DESENHA_A_CADA]]]     NVMTL_WIDE_TEST=1 para a linha larga fora do WindowServer
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <mach/mach_time.h>
static int fds(void) { int n = 0; for (int i = 0; i < 32768; i++) if (fcntl(i, F_GETFD) != -1) n++; return n; }
static double ms(uint64_t a, uint64_t b) { static mach_timebase_info_data_t t; if (!t.denom) mach_timebase_info(&t); return (double)(b - a) * t.numer / t.denom / 1e6; }
int main(int argc, char **argv)
{
    @autoreleasepool {
        int W = argc > 2 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080, N = argc > 3 ? atoi(argv[3]) : 3000, K = argc > 4 ? atoi(argv[4]) : 1;
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"GTX 960"]) dev = d;
        if (!dev) { printf("sem dispositivo Metal da GTX 960\n"); return 2; }
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{ (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
            (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @(0x42475241) });
        if (!s) { printf("IOSurfaceCreate falhou\n"); return 3; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        printf("IOSurface %u %dx%d, %d voltas, desenha a cada %d; descritores abertos no inicio: %d\n", IOSurfaceGetID(s), W, H, N, K, fds());
        uint64_t t0 = mach_absolute_time(), tb = t0; int i = 0, fail = -1;
        for (i = 0; i < N; i++) {
            @autoreleasepool {
                id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:s plane:0];
                if (!t) { fail = i; break; }
                if (K > 0 && i % K == 0) {
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake((i & 255) / 255.0, 0.5, 0.25, 1);
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
                    [cb commit]; [cb waitUntilCompleted];
                }
            }
            if ((i + 1) % 250 == 0) {
                uint64_t now = mach_absolute_time();
                printf("volta %5d: %.2f ms por volta, descritores abertos %d\n", i + 1, ms(tb, now) / 250, fds()); fflush(stdout);
                tb = now;
            }
        }
        printf("%s na volta %d; %.1f s no total; descritores abertos no fim: %d\n", fail >= 0 ? "FALHOU (textura nil)" : "terminou", fail >= 0 ? fail : i,
               ms(t0, mach_absolute_time()) / 1000, fds());
        return fail >= 0 ? 1 : 0;
    }
}
