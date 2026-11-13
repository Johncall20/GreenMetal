/*
 * nvmtlrender - degraus do caminho de render do Metal na GTX 960, um caso por processo
 * (um Xid derruba o backend do processo inteiro).
 *   1 clear RGBA8          2 triangulo RGBA8        3 triangulo BGRA8
 *   4 quad com textura     5 blend alpha            6 textura de IOSurface + BGRA8 (como o Chrome)
 *   7 MSAA 4x + resolve    8 stencil/depth          9 instanced + vertex buffer
 * Uso: ./nvmtlrender <caso>   (sem argumento: todos, um processo por caso)
 * O nome do executavel esta na lista de processos liberados do plugin.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#define W 64
#define H 64

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; float2 uv; float4 c; };\n"
     "vertex V vs_tri(uint vid [[vertex_id]]) {\n"
     "  float2 P[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
     "  V o; o.p = float4(P[vid], 0, 1); o.uv = P[vid] * 0.5 + 0.5; o.c = float4(1, 0.5, 0.25, 1); return o; }\n"
     "vertex V vs_half(uint vid [[vertex_id]]) {\n"
     "  float2 P[3] = { float2(-1,-1), float2(1,-1), float2(-1,1) };\n"
     "  V o; o.p = float4(P[vid], 0.5, 1); o.uv = P[vid] * 0.5 + 0.5; o.c = float4(0, 1, 0, 0.5); return o; }\n"
     "struct VI { float2 pos [[attribute(0)]]; };\n"
     "vertex V vs_inst(VI in [[stage_in]], uint iid [[instance_id]]) {\n"
     "  V o; o.p = float4(in.pos * 0.5 + float2(iid ? 0.5 : -0.5, 0), 0, 1); o.uv = 0; o.c = float4(0, 0, 1, 1); return o; }\n"
     "fragment float4 fs_color(V in [[stage_in]]) { return in.c; }\n"
     "fragment float4 fs_tex(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "  return t.sample(s, in.uv); }\n";

static id<MTLDevice> gDev;
static id<MTLLibrary> gLib;

static id<MTLRenderPipelineState> mkpipe(NSString *vs, NSString *fs, MTLPixelFormat fmt, BOOL blend, NSUInteger samples,
                                       MTLPixelFormat ds, MTLVertexDescriptor *vd)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [gLib newFunctionWithName:vs];
    d.fragmentFunction = [gLib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = fmt;
    d.rasterSampleCount = samples;
    if (ds != MTLPixelFormatInvalid) { d.depthAttachmentPixelFormat = ds; d.stencilAttachmentPixelFormat = ds; }
    if (vd) d.vertexDescriptor = vd;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [gDev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) printf("  pipeline %s/%s FALHOU: %s\n", vs.UTF8String, fs.UTF8String, e.localizedDescription.UTF8String);
    return p;
}

static id<MTLTexture> target(MTLPixelFormat fmt, NSUInteger samples, MTLStorageMode sm)
{
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:W height:H mipmapped:NO];
    t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    t.storageMode = sm;
    if (samples > 1) { t.textureType = MTLTextureType2DMultisample; t.sampleCount = samples; }
    return [gDev newTextureWithDescriptor:t];
}

/* le o pixel (x,y) de uma textura RGBA8/BGRA8 privada via blit para buffer */
static int readback(id<MTLCommandQueue> q, id<MTLTexture> t, uint32_t *out)
{
    id<MTLBuffer> b = [gDev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (!cb) { printf("  commandBuffer nil no readback\n"); return -1; }
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("  readback status %ld %s\n", (long)cb.status, cb.error.localizedDescription.UTF8String ?: ""); return -1; }
    memcpy(out, b.contents, W * H * 4);
    return 0;
}

static int run_pass(id<MTLCommandQueue> q, MTLRenderPassDescriptor *rp, void (^body)(id<MTLRenderCommandEncoder>))
{
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (!cb) { printf("  commandBuffer nil\n"); return -1; }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) { printf("  render encoder nil\n"); return -1; }
    if (body) body(e);
    [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        printf("  render status %ld %s\n", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return -1;
    }
    return 0;
}

static MTLRenderPassDescriptor *rp_for(id<MTLTexture> t, MTLLoadAction la, id<MTLTexture> resolve)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = la;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
    rp.colorAttachments[0].storeAction = resolve ? MTLStoreActionMultisampleResolve : MTLStoreActionStore;
    if (resolve) rp.colorAttachments[0].resolveTexture = resolve;
    return rp;
}

static int check(const char *what, uint32_t got, uint32_t want, int tol)
{
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        int g = (got >> (8 * i)) & 0xff, w = (want >> (8 * i)) & 0xff;
        if (abs(g - w) > tol) ok = 0;
    }
    printf("  %s: 0x%08x (esperado 0x%08x) %s\n", what, got, want, ok ? "ok" : "ERRADO");
    return ok;
}

/* pixel em memoria little-endian: RGBA8 -> 0xAABBGGRR, BGRA8 -> 0xAARRGGBB */
static int caso(int n)
{
    id<MTLCommandQueue> q = [gDev newCommandQueue];
    uint32_t px[W * H];
    MTLPixelFormat rgba = MTLPixelFormatRGBA8Unorm, bgra = MTLPixelFormatBGRA8Unorm;
    switch (n) {
    case 1: {
        id<MTLTexture> t = target(rgba, 1, MTLStorageModePrivate);
        if (run_pass(q, rp_for(t, MTLLoadActionClear, nil), nil) || readback(q, t, px)) return 1;
        return !check("clear azul", px[0], 0xffff0000, 1);
    }
    case 2: case 3: {
        MTLPixelFormat f = n == 2 ? rgba : bgra;
        id<MTLRenderPipelineState> p = mkpipe(@"vs_tri", @"fs_color", f, NO, 1, MTLPixelFormatInvalid, nil);
        id<MTLTexture> t = target(f, 1, MTLStorageModePrivate);
        if (!p || run_pass(q, rp_for(t, MTLLoadActionClear, nil), ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }) ||
            readback(q, t, px)) return 1;
        return !check("triangulo (1,.5,.25)", px[W * 10 + 10], n == 2 ? 0xff4080ff : 0xffff8040, 2);
    }
    case 4: case 6: {
        id<MTLTexture> src;
        if (n == 4) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:rgba width:W height:H mipmapped:NO];
            src = [gDev newTextureWithDescriptor:d];
            for (int i = 0; i < W * H; i++) px[i] = 0xff00ff00;
            [src replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
        } else {
            NSDictionary *props = @{ (id)kIOSurfaceWidth: @W, (id)kIOSurfaceHeight: @H, (id)kIOSurfaceBytesPerElement: @4,
                                     (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
            IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
            IOSurfaceLock(s, 0, NULL);
            uint32_t *b = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s) / 4;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) b[y * row + x] = 0xff00ff00;
            IOSurfaceUnlock(s, 0, NULL);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bgra width:W height:H mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            src = [gDev newTextureWithDescriptor:d iosurface:s plane:0];
        }
        if (!src) { printf("  textura fonte nil\n"); return 1; }
        MTLPixelFormat f = n == 4 ? rgba : bgra;
        id<MTLRenderPipelineState> p = mkpipe(@"vs_tri", @"fs_tex", f, NO, 1, MTLPixelFormatInvalid, nil);
        id<MTLSamplerState> smp = [gDev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        id<MTLTexture> t = target(f, 1, MTLStorageModePrivate);
        if (!p || run_pass(q, rp_for(t, MTLLoadActionClear, nil), ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e setFragmentTexture:src atIndex:0]; [e setFragmentSamplerState:smp atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }) ||
            readback(q, t, px)) return 1;
        return !check("textura verde", px[W * 10 + 10], 0xff00ff00, 1);
    }
    case 5: {
        id<MTLRenderPipelineState> p = mkpipe(@"vs_half", @"fs_color", rgba, YES, 1, MTLPixelFormatInvalid, nil);
        id<MTLTexture> t = target(rgba, 1, MTLStorageModePrivate);
        if (!p || run_pass(q, rp_for(t, MTLLoadActionClear, nil), ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }) ||
            readback(q, t, px)) return 1;
        return !check("verde 50% sobre azul", px[W * 60 + 2], 0xff7f8000, 2);
    }
    case 7: {
        id<MTLRenderPipelineState> p = mkpipe(@"vs_tri", @"fs_color", rgba, NO, 4, MTLPixelFormatInvalid, nil);
        id<MTLTexture> ms = target(rgba, 4, MTLStorageModePrivate), t = target(rgba, 1, MTLStorageModePrivate);
        if (!p || run_pass(q, rp_for(ms, MTLLoadActionClear, t), ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }) ||
            readback(q, t, px)) return 1;
        return !check("MSAA resolvido", px[W * 10 + 10], 0xff4080ff, 2);
    }
    case 8: {
        MTLPixelFormat ds = MTLPixelFormatDepth32Float_Stencil8;
        id<MTLRenderPipelineState> p = mkpipe(@"vs_tri", @"fs_color", rgba, NO, 1, ds, nil);
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ds width:W height:H mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> dt = [gDev newTextureWithDescriptor:dd], t = target(rgba, 1, MTLStorageModePrivate);
        MTLRenderPassDescriptor *rp = rp_for(t, MTLLoadActionClear, nil);
        rp.depthAttachment.texture = dt; rp.depthAttachment.clearDepth = 1.0; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
        rp.stencilAttachment.texture = dt; rp.stencilAttachment.clearStencil = 0; rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
        MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionLess; dsd.depthWriteEnabled = YES;
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways; sd.depthStencilPassOperation = MTLStencilOperationIncrementClamp;
        dsd.frontFaceStencil = sd; dsd.backFaceStencil = sd;
        id<MTLDepthStencilState> dss = [gDev newDepthStencilStateWithDescriptor:dsd];
        if (!p || run_pass(q, rp, ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e setDepthStencilState:dss]; [e setStencilReferenceValue:1];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }) ||
            readback(q, t, px)) return 1;
        return !check("triangulo com depth/stencil", px[W * 10 + 10], 0xff4080ff, 2);
    }
    case 9: {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 8;
        id<MTLRenderPipelineState> p = mkpipe(@"vs_inst", @"fs_color", rgba, NO, 1, MTLPixelFormatInvalid, vd);
        float v[6] = { -1, -1, 1, -1, -1, 1 };
        id<MTLBuffer> vb = [gDev newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
        id<MTLTexture> t = target(rgba, 1, MTLStorageModePrivate);
        if (!p || run_pass(q, rp_for(t, MTLLoadActionClear, nil), ^(id<MTLRenderCommandEncoder> e) {
                [e setRenderPipelineState:p]; [e setVertexBuffer:vb offset:0 atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2]; }) ||
            readback(q, t, px)) return 1;
        return !check("instancia 1 (azul escuro)", px[W * 20 + 40], 0xffff0000, 2);
    }
    }
    printf("caso %d desconhecido\n", n);
    return 2;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc < 2) {
            int bad = 0;
            for (int n = 1; n <= 9; n++) {
                pid_t p = fork();
                if (p == 0) { char a[4]; snprintf(a, sizeof a, "%d", n); execl(argv[0], argv[0], a, (char *)NULL); _exit(127); }
                int st = 0; waitpid(p, &st, 0);
                int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                printf("CASO %d: %s\n\n", n, ok ? "ok" : "FALHOU");
                bad += !ok;
            }
            printf("RESUMO: %d de 9 falharam\n", bad);
            return bad != 0;
        }
        int n = atoi(argv[1]);
        gDev = MTLCreateSystemDefaultDevice();
        if (!gDev || [gDev.name rangeOfString:@"GTX 960"].location == NSNotFound) { printf("sem MTLDevice da GTX 960 (Metal armado?)\n"); return 3; }
        NSError *e = nil;
        gLib = [gDev newLibraryWithSource:kSrc options:nil error:&e];
        if (!gLib) { printf("MSL: %s\n", e.localizedDescription.UTF8String); return 1; }
        static const char *nome[] = { "", "clear RGBA8", "triangulo RGBA8", "triangulo BGRA8", "textura", "blend",
                                      "IOSurface BGRA8", "MSAA 4x", "depth/stencil", "instanced + vertex buffer" };
        printf("== caso %d: %s\n", n, n >= 1 && n <= 9 ? nome[n] : "?");
        return caso(n);
    }
}
