/*
 * sbprobe - roda dentro da App Sandbox (entitlement com.apple.security.app-sandbox) e testa o que o IOKit deixa abrir:
 * NVDA tipo 0x80000000 (NVDAUser, caminho atual da libnvrm387) e cada tipo de user client do nvAccelerator,
 * com o nome da classe que volta. Nao chama metodo nenhum: so abre e fecha.
 */
#include <stdio.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>

static void try_open(const char *svcClass, uint32_t type)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(svcClass));
    if (!s) { printf("  %-14s tipo 0x%08x: servico ausente\n", svcClass, type); return; }
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), type, &c);
    io_name_t cls = "";
    if (kr == KERN_SUCCESS) IOObjectGetClass(c, cls);
    printf("  %-14s tipo 0x%08x: %s 0x%x %s\n", svcClass, type, kr == KERN_SUCCESS ? "ABRIU" : "negado", kr, cls);
    if (c) IOServiceClose(c);
    IOObjectRelease(s);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("HOME=%s\n", getenv("HOME") ? getenv("HOME") : "?");
    printf("caminho atual do NVK:\n");
    try_open("NVDA", 0x80000000u);
    printf("ponte do MaxwellMetal (kext 0.2.0):\n");
    try_open("MaxwellMetal", 0x4D58524Du);
    printf("user clients do acelerador:\n");
    for (uint32_t t = 0; t <= 12; t++) try_open("nvAccelerator", t);
    return 0;
}
