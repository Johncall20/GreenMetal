/* icontest - desenha o icone de um app num bitmap (como o Chrome no NSDockTile) e mostra a excecao, se houver. */
#import <AppKit/AppKit.h>
int main(int argc, char **argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString *app = argc > 1 ? @(argv[1]) : @"/Applications/Google Chrome.app";
        NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:app];
        printf("icone de %s: %s, %d representacoes\n", app.UTF8String, icon ? "ok" : "nil", (int)icon.representations.count);
        for (NSNumber *sz in @[@32, @128, @512]) {
            CGFloat s = sz.doubleValue;
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)s pixelsHigh:(NSInteger)s
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            @try {
                [NSGraphicsContext saveGraphicsState];
                [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
                [icon drawInRect:NSMakeRect(0, 0, s, s) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
                [NSGraphicsContext restoreGraphicsState];
                printf("  %4.0f px: desenhou\n", s);
            } @catch (NSException *e) {
                [NSGraphicsContext restoreGraphicsState];
                printf("  %4.0f px: EXCECAO %s: %s\n", s, e.name.UTF8String, e.reason.UTF8String);
            }
        }
        @try {
            NSDockTile *t = NSApp.dockTile;
            NSImageView *v = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)];
            v.image = icon; t.contentView = v; [t display];
            printf("NSDockTile display: ok\n");
        } @catch (NSException *e) { printf("NSDockTile display: EXCECAO %s: %s\n", e.name.UTF8String, e.reason.UTF8String); }
    }
    return 0;
}
