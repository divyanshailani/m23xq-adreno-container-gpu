// Headless Vulkan compute benchmark: measure Adreno 619 GFLOPS via a flops-heavy shader
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define WG 256
#define NBYTES (96u*1024u*1024u)
#define N (NBYTES/4u)
#define ITERS 200

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static uint32_t qf;

static void die(const char* m, VkResult r) {
    if (r == VK_SUCCESS) return;
    fprintf(stderr, "FATAL: %s (VkResult=%d)\n", m, r);
    exit(1);
}

int main(void) {
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gpubench", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    die("instance", vkCreateInstance(&ici, NULL, &inst));

    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) die("no devices", VK_SUCCESS);
    VkPhysicalDevice* devs = malloc(nd * sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &nd, devs);
    pd = devs[0];
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        fprintf(stderr, "GPU%u: %s\n", i, p.deviceName);
        if (p.vendorID == 0x5143) { pd = devs[i]; break; }
    }

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties* qfp = malloc(nq * sizeof(*qfp));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qfp);
    for (uint32_t i = 0; i < nq; i++)
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }

    const float prio = 0.f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    die("device", vkCreateDevice(pd, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &q);

    // Compute shader (GLSL):
    // #version 450
    // layout(local_size_x=256) in;
    // layout(binding=0) buffer A { float a[]; };
    // layout(binding=1) buffer B { float b[]; };
    // layout(binding=2) buffer O { float o[]; };
    // void main() {
    //   uint i = gl_GlobalInvocationID.x;
    //   float x = a[i];
    //   float acc = 0.0;
    //   for (int k = 0; k < 256; k++) { acc = acc * 0.999f + x * 0.001f; x = x * 1.0001f; }
    //   o[i] = acc + b[i];
    // }
    // compiled to SPIR-V offline; words below (add shader, 2 FMAs per inner iter x256 iters)
    static const uint32_t spv[] = {
        #include "flops_spv.inc"
    };
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(spv), .pCode = spv };
    VkShaderModule mod;
    die("shader", vkCreateShaderModule(dev, &smci, NULL, &mod));

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = NBYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer bufs[3];
    for (int i = 0; i < 3; i++)
        die("buffer", vkCreateBuffer(dev, &bci, NULL, &bufs[i]));

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, bufs[0], &mr);
    uint32_t mtype = 0xffffffff;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mtype = i; break;
        }
    if (mtype == 0xffffffff)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                mtype = i; break;
            }
    fprintf(stderr, "mem type: %u (device-local preferred)\n", mtype);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mtype };
    VkDeviceMemory mems[3];
    for (int i = 0; i < 3; i++)
        die("alloc", vkAllocateMemory(dev, &mai, NULL, &mems[i]));
    for (int i = 0; i < 3; i++)
        die("bind", vkBindBufferMemory(dev, bufs[i], mems[i], 0));

    // fill inputs via host-visible mapping fallback OR via staging — try direct map first
    for (int i = 0; i < 2; i++) {
        void* p = NULL;
        if (vkMapMemory(dev, mems[i], 0, NBYTES, 0, &p) == VK_SUCCESS) {
            float* f = p;
            for (uint32_t j = 0; j < N; j++) f[j] = (float)(i + 1);
            VkMappedMemoryRange rng = {
                .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = mems[i], .offset = 0, .size = NBYTES };
            vkFlushMappedMemoryRanges(dev, 1, &rng);
            vkUnmapMemory(dev, mems[i]);
        }
    }

    VkDescriptorSetLayoutBinding lbs[3];
    for (int i = 0; i < 3; i++)
        lbs[i] = (VkDescriptorSetLayoutBinding){
            .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dsli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = lbs };
    VkDescriptorSetLayout dsl;
    die("dsl", vkCreateDescriptorSetLayout(dev, &dsli, NULL, &dsl));

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    die("pool", vkCreateDescriptorPool(dev, &dpci, NULL, &dp));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds;
    die("set alloc", vkAllocateDescriptorSets(dev, &dsai, &ds));

    VkDescriptorBufferInfo dbi[3];
    for (int i = 0; i < 3; i++)
        dbi[i] = (VkDescriptorBufferInfo){ .buffer = bufs[i], .offset = 0, .range = NBYTES };
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++)
        wr[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] };
    vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);

    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl;
    die("pipe layout", vkCreatePipelineLayout(dev, &pli, NULL, &pl));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = mod, .pName = "main" },
        .layout = pl };
    VkPipeline pipe;
    die("pipeline", vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkCommandPoolCreateInfo cpi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qf };
    VkCommandPool cp;
    die("cmdpool", vkCreateCommandPool(dev, &cpi, NULL, &cp));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    die("cmdbuf", vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    die("begin", vkBeginCommandBuffer(cb, &bbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / WG, 1, 1);
    die("end", vkEndCommandBuffer(cb));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb };

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int it = 0; it < ITERS; it++)
        die("submit", vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    die("submit fence", vkQueueSubmit(q, 1, &si, fence));
    die("wait", vkWaitForFences(dev, 1, &fence, VK_TRUE, ~(uint64_t)0));
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    // shader inner loop: 256 iterations x 2 FLOPs (mul-add) per work item, x256 loop overhead mul
    double flops = (double)N * ITERS * 256.0 * 2.0;
    printf("device: see stderr\n");
    printf("time: %.3f s\niterations: %d\nbytes: %u MB\n", secs, ITERS, NBYTES / (1024 * 1024));
    printf("GFLOPS: %.2f\n", flops / secs / 1e9);
    printf("GB/s effective (in+out): %.2f\n",
           (double)NBYTES * 3.0 * ITERS / secs / 1e9);
    return 0;
}
