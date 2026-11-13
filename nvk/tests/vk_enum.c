/*
 * vk_enum - carrega o libvulkan_nouveau.dylib direto (sem loader do Vulkan) e enumera a GPU.
 * Uso: sudo ./vk_enum ./libvulkan_nouveau.dylib [device]
 *   sem "device": instancia + enumeracao + propriedades
 *   com "device": tambem cria VkDevice + fila e espera ociosidade (vkQueueWaitIdle)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGIPA)(VkInstance, const char *);

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "./libvulkan_nouveau.dylib";
    int wantDevice = argc > 2 && !strcmp(argv[2], "device");
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_icdGIPA gipa = (PFN_icdGIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "sem vk_icdGetInstanceProcAddr\n"); return 1; }
    uint32_t icdVer = 5;
    PFN_vkVoidFunction neg = (PFN_vkVoidFunction)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) ((VkResult (*)(uint32_t *))neg)(&icdVer);
    printf("driver carregado, interface ICD %u\n", icdVer);

    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_enum",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = createInstance(&ici, NULL, &inst);
    printf("vkCreateInstance: %d\n", r);
    if (r) return 1;

#define GET(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GET(vkEnumeratePhysicalDevices); GET(vkGetPhysicalDeviceProperties);
    GET(vkGetPhysicalDeviceMemoryProperties); GET(vkGetPhysicalDeviceQueueFamilyProperties);
    GET(vkCreateDevice); GET(vkDestroyDevice); GET(vkGetDeviceQueue); GET(vkDestroyInstance);
    GET(vkGetDeviceProcAddr);

    uint32_t n = 0;
    r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    printf("vkEnumeratePhysicalDevices: %d, %u dispositivo(s)\n", r, n);
    if (r || !n) return 1;
    VkPhysicalDevice pd[4];
    if (n > 4) n = 4;
    vkEnumeratePhysicalDevices(inst, &n, pd);

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd[0], &p);
    printf("  nome: %s\n  Vulkan %u.%u.%u  driver 0x%x  vendor 0x%04x device 0x%04x  tipo %d\n",
           p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
           VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID, p.deviceType);
    printf("  maxComputeWorkGroupInvocations %u  maxImageDimension2D %u\n",
           p.limits.maxComputeWorkGroupInvocations, p.limits.maxImageDimension2D);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd[0], &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        printf("  heap %u: %llu MiB flags 0x%x\n", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
               mp.memoryHeaps[i].flags);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &nq, NULL);
    VkQueueFamilyProperties qf[8];
    if (nq > 8) nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &nq, qf);
    for (uint32_t i = 0; i < nq; i++)
        printf("  fila %u: flags 0x%x count %u\n", i, qf[i].queueFlags, qf[i].queueCount);

    if (wantDevice) {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                        .queueCount = 1, .pQueuePriorities = &prio };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                   .pQueueCreateInfos = &qci };
        VkDevice dev;
        r = vkCreateDevice(pd[0], &dci, NULL, &dev);
        printf("vkCreateDevice: %d\n", r);
        if (r) return 1;
        VkQueue q;
        vkGetDeviceQueue(dev, 0, 0, &q);
        PFN_vkQueueWaitIdle waitIdle = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(dev, "vkQueueWaitIdle");
        r = waitIdle(q);
        printf("vkQueueWaitIdle: %d\n", r);
        vkDestroyDevice(dev, NULL);
    }
    vkDestroyInstance(inst, NULL);
    printf("RESULTADO: ok\n");
    return 0;
}
