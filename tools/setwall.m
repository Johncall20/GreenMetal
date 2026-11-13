// setwall <imagem>: troca o papel de parede de todas as telas pela API do sistema (NSWorkspace), sem root.
// Uso no hackintosh com composicao por CPU: os papeis de parede dinamicos/Aerial sao video e nao aparecem.
#import <AppKit/AppKit.h>
int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "uso: setwall <imagem>\n"); return 2; }
        NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        if (![[NSFileManager defaultManager] fileExistsAtPath:u.path]) { fprintf(stderr, "nao existe: %s\n", argv[1]); return 1; }
        NSArray<NSScreen *> *screens = [NSScreen screens];
        printf("telas: %lu\n", (unsigned long)screens.count);
        int ok = 0;
        for (NSScreen *s in screens) {
            NSError *e = nil;
            NSDictionary *opt = @{ NSWorkspaceDesktopImageScalingKey: @(NSImageScaleProportionallyUpOrDown),
                                   NSWorkspaceDesktopImageAllowClippingKey: @YES };
            BOOL r = [[NSWorkspace sharedWorkspace] setDesktopImageURL:u forScreen:s options:opt error:&e];
            NSRect f = s.frame;
            printf("  tela %.0fx%.0f em (%.0f,%.0f): %s%s\n", f.size.width, f.size.height, f.origin.x, f.origin.y,
                   r ? "ok" : "FALHOU ", r ? "" : e.localizedDescription.UTF8String);
            ok += r ? 1 : 0;
            NSURL *now = [[NSWorkspace sharedWorkspace] desktopImageURLForScreen:s];
            printf("    agora: %s\n", now.path.UTF8String ?: "(nada)");
        }
        return ok ? 0 : 1;
    }
}
