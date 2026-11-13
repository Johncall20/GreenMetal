/*
 * vk_triangle - pipeline grafico do NVK na GTX 960: desenha um triangulo RGB numa imagem
 * 64x64 (offscreen, sem janela), copia para um buffer e confere os pixels. Grava tri.ppm.
 *
 * Uso: sudo ./vk_triangle ./libvulkan_nouveau.dylib
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"

#define W 64u
#define H 64u
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_icdGIPA)(VkInstance, const char *);

#define CHECK(call) do { VkResult _r = (call); printf("%-40.40s %d\n", #call, _r); \
                         if (_r != VK_SUCCESS) return 1; } while (0)

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "./libvulkan_nouveau.dylib";
    const int linear = argc > 2 && !strcmp(argv[2], "linear");
    printf("imagem: %s\n", linear ? "LINEAR (pitch)" : "OPTIMAL (block linear)");
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_icdGIPA gipa = (PFN_icdGIPA)dlsym(lib, "vk_icdGetInstanceProcAddr");
    uint32_t icdVer = 5;
    void *neg = dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) ((VkResult (*)(uint32_t *))neg)(&icdVer);

    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_triangle",
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
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define GD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    GD(vkGetDeviceQueue); GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory);
    GD(vkBindImageMemory); GD(vkCreateImageView); GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements);
    GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkUnmapMemory); GD(vkCreateRenderPass); GD(vkCreateFramebuffer);
    GD(vkCreateShaderModule); GD(vkCreatePipelineLayout); GD(vkCreateGraphicsPipelines);
    GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkCmdBeginRenderPass);
    GD(vkCmdBindPipeline); GD(vkCmdDraw); GD(vkCmdEndRenderPass); GD(vkCmdCopyImageToBuffer);
    GD(vkCmdPipelineBarrier); GD(vkEndCommandBuffer); GD(vkCreateFence); GD(vkQueueSubmit); GD(vkWaitForFences);
    GD(vkDestroyFence); GD(vkDestroyCommandPool); GD(vkDestroyPipeline); GD(vkDestroyPipelineLayout);
    GD(vkDestroyShaderModule); GD(vkDestroyFramebuffer); GD(vkDestroyRenderPass); GD(vkDestroyImageView);
    GD(vkDestroyImage); GD(vkDestroyBuffer); GD(vkFreeMemory); GD(vkDestroyDevice);

    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* imagem de cor 64x64 RGBA8 (tiling otimo = block linear da GPU) */
    VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    CHECK(vkCreateImage(dev, &imci, NULL, &img));
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(dev, img, &imr);
    uint32_t imt = find_mem(&mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imt == UINT32_MAX) imt = find_mem(&mp, imr.memoryTypeBits, 0);
    VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imr.size,
                                  .memoryTypeIndex = imt };
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &imai, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view;
    CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

    /* buffer de leitura (CPU) */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer rb;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &rb));
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(dev, rb, &bmr);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
        .memoryTypeIndex = find_mem(&mp, bmr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &bmai, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, rb, bmem, 0));
    volatile uint8_t *px;
    CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
    memset((void *)px, 0x5A, W * H * 4);

    /* render pass: limpa para preto, termina pronta para copia */
    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                    .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                     .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

    /* pipeline grafico */
    VkShaderModuleCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(tri_vert_spv), .pCode = tri_vert_spv };
    VkShaderModuleCreateInfo fsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = sizeof(tri_frag_spv), .pCode = tri_frag_spv };
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &vsci, NULL, &vs));
    CHECK(vkCreateShaderModule(dev, &fsci, NULL, &fs));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                              .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("compilacao dos shaders (NAK, SM 5.2): %.1f ms\n",
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

    /* comandos: render pass + draw(3) + copia da imagem para o buffer */
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                   .framebuffer = fb, .renderArea = sc, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &region);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    printf("vkWaitForFences: %d\n", wr);

    /* conferencia */
#define PX(x, y) (&px[((y) * W + (x)) * 4])
    unsigned covered = 0, untouched = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const volatile uint8_t *p = PX(x, y);
            if (p[0] == 0x5A && p[1] == 0x5A && p[2] == 0x5A && p[3] == 0x5A) untouched++;
            else if (p[0] | p[1] | p[2]) covered++;
        }
    const volatile uint8_t *top = PX(32, 19), *br = PX(45, 46), *bl = PX(18, 46), *corner = PX(2, 2), *mid = PX(32, 38);
    printf("pixels cobertos %u (esperado ~512) | nao escritos %u\n", covered, untouched);
    printf("topo (32,19)  RGBA %3u %3u %3u %3u  (esperado vermelho)\n", top[0], top[1], top[2], top[3]);
    printf("dir. (45,46)  RGBA %3u %3u %3u %3u  (esperado verde)\n", br[0], br[1], br[2], br[3]);
    printf("esq. (18,46)  RGBA %3u %3u %3u %3u  (esperado azul)\n", bl[0], bl[1], bl[2], bl[3]);
    printf("meio (32,38)  RGBA %3u %3u %3u %3u  (mistura)\n", mid[0], mid[1], mid[2], mid[3]);
    printf("canto (2,2)   RGBA %3u %3u %3u %3u  (esperado preto, alfa 255)\n", corner[0], corner[1], corner[2], corner[3]);
    int ok = wr == VK_SUCCESS && untouched == 0 && covered > 420 && covered < 600 &&
             top[0] > 150 && top[1] < 100 && top[2] < 100 &&
             br[1] > 150 && br[0] < 100 && br[2] < 100 &&
             bl[2] > 150 && bl[0] < 100 && bl[1] < 100 &&
             corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255;

    FILE *f = fopen("tri.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", W, H);
        for (uint32_t i = 0; i < W * H; i++) fwrite((const void *)&px[i * 4], 1, 3, f);
        fclose(f);
        printf("imagem gravada em %s\n", linear ? "tri.ppm (linear)" : "tri.ppm");
    }
    if (ok) printf("=> A GTX 960 DESENHOU O TRIANGULO PELO NVK (pipeline grafico ok)\n");

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkUnmapMemory(dev, bmem);
    vkDestroyBuffer(dev, rb, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("RESULTADO: %s\n", ok ? "ok" : "falhou");
    return ok ? 0 : 1;
}
