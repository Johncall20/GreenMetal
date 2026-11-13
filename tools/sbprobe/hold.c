#include <stdio.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("nvAccelerator"));
    io_connect_t c[16] = {0};
    for (uint32_t t = 0; t <= 9; t++) if (IOServiceOpen(s, mach_task_self(), t, &c[t])) c[t] = 0;
    printf("pid %d segurando\n", getpid()); fflush(stdout);
    sleep(4);
    return 0;
}
