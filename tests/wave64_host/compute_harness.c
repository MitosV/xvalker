/*
 * compute_harness.c - run a 64-lane compute shader on the host GPU and print its
 * storage buffer as 64 decimal u32 values. The shader under test is the HLSL form
 * emitted by XCL for a logical wave64 running as two 32-lane halves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "Vulkan error %d at line %d\n", r_, __LINE__); return 2; } } while (0)

static uint32_t *load_spv(const char *path, size_t *words) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint32_t *code = malloc((size_t)n);
    if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(code); return NULL; }
    fclose(f);
    *words = (size_t)n / 4;
    return code;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <shader.spv>\n", argv[0]); return 2; }
    size_t words = 0;
    uint32_t *code = load_spv(argv[1], &words);
    if (!code) return 2;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t ngpu = 0;
    vkEnumeratePhysicalDevices(inst, &ngpu, NULL);
    VkPhysicalDevice gpus[8];
    if (ngpu > 8) ngpu = 8;
    CHECK(vkEnumeratePhysicalDevices(inst, &ngpu, gpus));
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(gpu, &p2);
    fprintf(stderr, "GPU %s subgroup=%u\n", p2.properties.deviceName, sg.subgroupSize);
    if (sg.subgroupSize != 32) {
        fprintf(stderr, "SKIP: host subgroup is not 32 lanes\n");
        free(code);
        vkDestroyInstance(inst, NULL);
        return 3;
    }

    uint32_t qfam = 0, nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL);
    VkQueueFamilyProperties qp[8];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qp);
    for (uint32_t i = 0; i < nq && i < 8; ++i) {
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));

    VkShaderModuleCreateInfo sci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    sci.codeSize = words * 4;
    sci.pCode = code;
    VkShaderModule mod;
    CHECK(vkCreateShaderModule(dev, &sci, NULL, &mod));
    free(code);

    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings = &bind;
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = 64 * sizeof(uint32_t);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (req.memoryTypeBits & (1u << i) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mai.memoryTypeIndex = i;
            break;
        }
    }
    VkDeviceMemory memobj;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &memobj));
    CHECK(vkBindBufferMemory(dev, buf, memobj, 0));
    void *mapped = NULL;
    CHECK(vkMapMemory(dev, memobj, 0, req.size, 0, &mapped));
    memset(mapped, 0xCD, 64 * sizeof(uint32_t));

    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorBufferInfo dbi = { buf, 0, 64 * sizeof(uint32_t) };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

    VkQueue queue;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cPCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cPCI.queueFamilyIndex = qfam;
    VkCommandPool cmdpool;
    CHECK(vkCreateCommandPool(dev, &cPCI, NULL, &cmdpool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cmdpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, 2, 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &sub, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    vkInvalidateMappedMemoryRanges(dev, 1, &(VkMappedMemoryRange){ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, VK_NULL_HANDLE, memobj, 0, req.size });

    uint32_t *out = mapped;
    for (int i = 0; i < 64; ++i) printf("%u\n", out[i]);
    return 0;
}
