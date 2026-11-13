/*
 * vk_compute - primeiro trabalho real do NVK na GTX 960: um compute shader preenche um buffer
 * (data[i] = 2*i + 1, 1024 elementos) e a CPU confere cada valor.
 * Carrega o libvulkan_nouveau.dylib direto (sem loader).
 *
 * Uso: sudo ./vk_compute ./libvulkan_nouveau.dylib
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "fill_spv.h"

#define N 1024u
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGIPA)(VkInstance, const char *);

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define CHECK(call) do { VkResult _r = (call); printf("%-34s %d\n", #call, _r); \
                         if (_r != VK_SUCCESS) return 1; } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "./libvulkan_nouveau.dylib";
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_icdGIPA gipa = (PFN_icdGIPA)dlsym(h, "vk_icdGetInstanceProcAddr");
    uint32_t icdVer = 5;
    void *neg = dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) ((VkResult (*)(uint32_t *))neg)(&icdVer);

    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_compute",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceMemoryProperties); GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr); GI(vkDestroyInstance); GI(vkGetPhysicalDeviceProperties);

    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

#define GD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    GD(vkGetDeviceQueue); GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory);
    GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateShaderModule); GD(vkCreateDescriptorSetLayout);
    GD(vkCreatePipelineLayout); GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool);
    GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkCmdBindPipeline);
    GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch); GD(vkCmdPipelineBarrier); GD(vkEndCommandBuffer);
    GD(vkCreateFence); GD(vkQueueSubmit); GD(vkWaitForFences); GD(vkDestroyDevice);
    GD(vkDestroyPipeline); GD(vkDestroyShaderModule); GD(vkDestroyPipelineLayout);
    GD(vkDestroyDescriptorSetLayout); GD(vkDestroyDescriptorPool); GD(vkDestroyCommandPool);
    GD(vkDestroyFence); GD(vkDestroyBuffer); GD(vkFreeMemory); GD(vkUnmapMemory);

    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* buffer de N uint em memoria visivel pela CPU */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
    if (mt == UINT32_MAX) { printf("sem tipo de memoria HOST_VISIBLE|COHERENT\n"); return 1; }
    printf("memoria: tipo %u (flags 0x%x, heap %u), %llu bytes\n", mt, mp.memoryTypes[mt].propertyFlags,
           mp.memoryTypes[mt].heapIndex, (unsigned long long)mr.size);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mt };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    volatile uint32_t *data;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&data));
    for (uint32_t i = 0; i < N; i++) data[i] = 0xDEADBEEF;

    /* pipeline de compute */
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(fill_spv), .pCode = fill_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 1, .pBindings = &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = sm, .pName = "main" },
        .layout = pl };
    VkPipeline pipe;
    double t0 = now_ms();
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    printf("compilacao do shader (NAK, SM 5.2): %.1f ms\n", now_ms() - t0);

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                        .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    /* command buffer: dispatch de N/64 grupos + barreira para a CPU ler */
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    t0 = now_ms();
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    printf("vkWaitForFences: %d (%.2f ms)\n", wr, now_ms() - t0);

    uint32_t bad = 0, firstBad = UINT32_MAX;
    for (uint32_t i = 0; i < N; i++)
        if (data[i] != i * 2u + 1u) { if (firstBad == UINT32_MAX) firstBad = i; bad++; }
    printf("data[0..7]: %u %u %u %u %u %u %u %u   data[1023] = %u\n", data[0], data[1], data[2], data[3],
           data[4], data[5], data[6], data[7], data[1023]);
    if (bad) printf("ERRADOS: %u de %u (primeiro em %u: 0x%08x)\n", bad, N, firstBad, data[firstBad]);
    else printf("=> OS %u VALORES CONFEREM: A GTX 960 RODOU UM COMPUTE SHADER PELO NVK\n", N);

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("RESULTADO: %s\n", (wr == VK_SUCCESS && !bad) ? "ok" : "falhou");
    return (wr == VK_SUCCESS && !bad) ? 0 : 1;
}
