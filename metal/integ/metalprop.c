/*
 * metalprop - estado e gravacao do MetalPluginName no nvAccelerator (GeForceWeb), pelo user space.
 *   metalprop status   le MetalPluginName / MetalPluginClassName / IOGLBundleName
 *   metalprop probe    tenta gravar uma chave inofensiva (NVMTLProbe) para ver se o acelerador aceita
 *   metalprop on       MetalPluginName=NVMTLDriver, MetalPluginClassName=NVMTLDevice
 *   metalprop off      MetalPluginName=NVMTLDisabled (bundle inexistente: o Metal nao carrega nada)
 * Nada disso sobrevive ao reboot: o WhateverGreen (ngfxgl=1) remove o MetalPluginName no boot.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

static void show(io_service_t s, const char *k)
{
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, NULL, 0);
    char buf[256] = "(ausente)";
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
    else if (v) snprintf(buf, sizeof buf, "(tipo %lu)", (unsigned long)CFGetTypeID(v));
    printf("  %-22s %s\n", k, buf);
    if (v) CFRelease(v);
    CFRelease(key);
}
static kern_return_t setstr(io_service_t s, const char *k, const char *val)
{
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFStringRef v = CFStringCreateWithCString(NULL, val, kCFStringEncodingUTF8);
    kern_return_t kr = IORegistryEntrySetCFProperty(s, key, v);
    printf("  set %-18s = %-14s -> 0x%08x%s\n", k, val, kr, kr ? " (recusado)" : " (aceito)");
    CFRelease(key); CFRelease(v);
    return kr;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("nvAccelerator"));
    if (!s) { printf("nvAccelerator nao encontrado\n"); return 1; }
    if (strcmp(cmd, "status") && geteuid() != 0) { fprintf(stderr, "precisa de root (sudo)\n"); return 2; }
    printf("nvAccelerator:\n");
    if (!strcmp(cmd, "probe")) {
        kern_return_t kr = setstr(s, "NVMTLProbe", "1");
        show(s, "NVMTLProbe");
        printf("=> o acelerador %s propriedades pelo user space\n", kr ? "NAO aceita" : "ACEITA");
    } else if (!strcmp(cmd, "on")) {
        setstr(s, "MetalPluginClassName", "NVMTLDevice");
        setstr(s, "MetalPluginName", "NVMTLDriver");
    } else if (!strcmp(cmd, "off")) {
        setstr(s, "MetalPluginName", "NVMTLDisabled");
    } else if (strcmp(cmd, "status")) {
        fprintf(stderr, "uso: %s [status|probe|on|off]\n", argv[0]); return 2;
    }
    show(s, "MetalPluginName");
    show(s, "MetalPluginClassName");
    show(s, "IOGLBundleName");
    IOObjectRelease(s);
    return 0;
}
