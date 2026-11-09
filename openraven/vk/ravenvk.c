// ravenvk.c - see ravenvk.h. Vulkan is loaded with dlopen, so ravengfx.so still loads without it.
// Shaders are compiled SPIR-V (ravenvk_spv.h, made by build_vk.sh) with every size as a specialization
// constant. Each conv layer is timed in its variants at start-up (autotune) and the fastest is kept.
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#define EXT_MEM  "VK_KHR_external_memory_win32"
#define EXT_SEM  "VK_KHR_external_semaphore_win32"
#define HT_MEM   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define HT_SEM   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
#define dlopen(name, flags) ((void *)LoadLibraryA("vulkan-1.dll"))
#define dlsym(lib, sym)     ((void *)GetProcAddress((HMODULE)(lib), (sym)))
#else
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#define EXT_MEM  "VK_KHR_external_memory_fd"
#define EXT_SEM  "VK_KHR_external_semaphore_fd"
#define HT_MEM   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define HT_SEM   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ravenvk.h"
#include "ravenvk_spv.h"

#define F(n) static PFN_##n n;
#define VK_FUNCS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
  X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
  X(vkGetDeviceProcAddr) X(vkDestroyInstance)
#define DEV_FUNCS(X) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateShaderModule) \
  X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines) \
  X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
  X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
  X(vkResetCommandBuffer) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
  X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) X(vkCmdWriteTimestamp) \
  X(vkCmdResetQueryPool) X(vkCreateQueryPool) X(vkGetQueryPoolResults) X(vkQueueSubmit) X(vkQueueWaitIdle) \
  X(vkCreateSemaphore) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDeviceWaitIdle) X(vkDestroyDevice)
VK_FUNCS(F) DEV_FUNCS(F) static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR;
#ifdef _WIN32
static PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR;
static PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR;
#else
static PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
static PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
#endif

typedef struct { VkBuffer b; VkDeviceMemory m; VkDeviceSize n; } Buf;
typedef struct { int w, h, c; Buf b; } Tensor;
typedef struct { VkPipeline p; uint32_t gx, gy, gz; int acc; } Variant;   // acc: 0 plain kernel, 1 fp32 accumulate, 2 fp16 accumulate
typedef struct { VkPipeline p; VkDescriptorSet ds; uint32_t pc[8]; uint32_t gx, gy, gz; Variant alt[24]; int nalt; } Pass;
int rvk_acc_pref;   // 0 auto (time fp16 and fp32 accumulate, keep the faster per layer), 1 fp16 only, 2 fp32 only

#define MAXL 64
#define MAXP 160
static struct {
    void *lib; VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceMemoryProperties mp; VkPhysicalDeviceProperties props;
    VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkDescriptorPool dpool;
    VkCommandPool cpool; VkCommandBuffer cb, tcb; VkFence fence; VkQueryPool qpool;
    VkSemaphore sem_in, sem_out; int exported, sg;
    Buf in, out, stage; Tensor T[2 * MAXL + 2]; int nt;
    Buf wb[MAXL], bb[MAXL];
    Pass pass[MAXP]; int np;
    VkPipeline pipes[256]; int npipes;
    int W, H; char *sp;   // sp = mapped staging memory
} V;

static void fail(rvk_info *in, const char *why) { if (in) snprintf(in->why, sizeof in->why, "%s", why); }
#define CK(x, msg) do { if ((x) != VK_SUCCESS) { fail(info, msg); return 0; } } while (0)

static int memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return -1;
}
static int mkbuf(Buf *r, VkDeviceSize n, int host, int exportable) {
    r->n = n;
    VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = HT_MEM };
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, exportable ? &eb : NULL, .size = n,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    if (vkCreateBuffer(V.dev, &bi, 0, &r->b)) return 0;
    VkMemoryRequirements q; vkGetBufferMemoryRequirements(V.dev, r->b, &q);
    int t = memtype(q.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) return 0;
    VkMemoryDedicatedAllocateInfo dd = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = r->b };
    VkExportMemoryAllocateInfo ex = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dd, .handleTypes = HT_MEM };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, exportable ? &ex : NULL, .allocationSize = q.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(V.dev, &ai, 0, &r->m)) return 0;
    r->n = q.size;
    return vkBindBufferMemory(V.dev, r->b, r->m, 0) == VK_SUCCESS;
}
static int mktensor(Tensor *t, int w, int h, int c) {
    t->w = w; t->h = h; t->c = c;
    return mkbuf(&t->b, ((VkDeviceSize)(w + 2) * (h + 4) * c + 8192) * 2, 0, 0);   // + slack for over-reads
}
static VkPipeline pipeline(const uint32_t *code, size_t n, const int *spec, int nspec) {
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
    VkShaderModule sm; if (vkCreateShaderModule(V.dev, &smi, 0, &sm)) return 0;
    VkSpecializationMapEntry me[8]; for (int i = 0; i < nspec; i++) me[i] = (VkSpecializationMapEntry){ i, i * 4, 4 };
    VkSpecializationInfo si = { nspec, me, nspec * 4, spec };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main",
                   .pSpecializationInfo = nspec ? &si : NULL }, .layout = V.pl };
    VkPipeline p = 0;
    if (vkCreateComputePipelines(V.dev, 0, 1, &cpi, 0, &p)) p = 0;
    vkDestroyShaderModule(V.dev, sm, 0);
    if (p && V.npipes < 256) V.pipes[V.npipes++] = p;
    return p;
}
static Pass *addpass(Buf *b0, Buf *b1, Buf *b2, Buf *b3) {
    Pass *s = &V.pass[V.np++]; memset(s, 0, sizeof *s);
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = V.dpool, .descriptorSetCount = 1, .pSetLayouts = &V.dsl };
    if (vkAllocateDescriptorSets(V.dev, &dai, &s->ds)) return NULL;
    Buf *bs[4] = { b0, b1, b2, b3 }; VkDescriptorBufferInfo dbi[4]; VkWriteDescriptorSet wd[4];
    for (int i = 0; i < 4; i++) {
        Buf *b = bs[i] ? bs[i] : b2;
        dbi[i] = (VkDescriptorBufferInfo){ b->b, 0, VK_WHOLE_SIZE };
        wd[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = i, .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] };
    }
    vkUpdateDescriptorSets(V.dev, 4, wd, 0, 0);
    return s;
}
static const VkMemoryBarrier MB = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT };
static void record(VkCommandBuffer cb, Pass *p, Variant *v, int reps) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v ? v->p : p->p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, V.pl, 0, 1, &p->ds, 0, 0);
    vkCmdPushConstants(cb, V.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, p->pc);
    for (int r = 0; r < reps; r++) {
        if (v) vkCmdDispatch(cb, v->gx, v->gy, v->gz); else vkCmdDispatch(cb, p->gx, p->gy, p->gz);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &MB, 0, 0, 0, 0);
    }
}
static int submit_wait(VkCommandBuffer cb) {
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkResetFences(V.dev, 1, &V.fence);
    if (vkQueueSubmit(V.q, 1, &si, V.fence)) return 0;
    return vkWaitForFences(V.dev, 1, &V.fence, 1, 5000000000ull) == VK_SUCCESS;
}
static double time_variant(Pass *p, Variant *v) {   // ms per dispatch, or 1e9 when timestamps don't work
    if (!V.props.limits.timestampPeriod) return 1e9;
    double ms = 1e9;
    for (int rep = 0; rep < 2; rep++) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkResetCommandBuffer(V.tcb, 0); vkBeginCommandBuffer(V.tcb, &bi);
        vkCmdResetQueryPool(V.tcb, V.qpool, 0, 2);
        vkCmdWriteTimestamp(V.tcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qpool, 0);
        record(V.tcb, p, v, 3);
        vkCmdWriteTimestamp(V.tcb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qpool, 1);
        vkEndCommandBuffer(V.tcb);
        if (!submit_wait(V.tcb)) return 1e9;
        uint64_t ts[2];
        if (vkGetQueryPoolResults(V.dev, V.qpool, 0, 2, 16, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) return 1e9;
        ms = (ts[1] - ts[0]) * V.props.limits.timestampPeriod / 1e6 / 3;
    }
    return ms;
}
static int r16(int c) { return (c + 15) / 16 * 16; }
static int has_ext(const char *name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(V.pd, 0, &n, 0);
    VkExtensionProperties *e = calloc(n, sizeof *e); vkEnumerateDeviceExtensionProperties(V.pd, 0, &n, e);
    int ok = 0; for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) ok = 1;
    free(e); return ok;
}
// 16x16x16 fp16 subgroup cooperative matrix: returns 2 if fp16 accumulate works, 1 if only fp32, 0 none
static int coopmat_level(void) {
    if (!vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR || !has_ext("VK_KHR_cooperative_matrix")) return 0;
    uint32_t c = 0; if (vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(V.pd, &c, 0) || !c) return 0;
    VkCooperativeMatrixPropertiesKHR q[64]; if (c > 64) c = 64;
    for (uint32_t k = 0; k < c; k++) q[k] = (VkCooperativeMatrixPropertiesKHR){ VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR };
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(V.pd, &c, q);
    int lv = 0;
    for (uint32_t k = 0; k < c; k++) {
        if (q[k].MSize != 16 || q[k].NSize != 16 || q[k].KSize != 16 || q[k].scope != VK_SCOPE_SUBGROUP_KHR) continue;
        if (q[k].AType != VK_COMPONENT_TYPE_FLOAT16_KHR || q[k].BType != VK_COMPONENT_TYPE_FLOAT16_KHR) continue;
        if (q[k].CType == VK_COMPONENT_TYPE_FLOAT16_KHR && q[k].ResultType == VK_COMPONENT_TYPE_FLOAT16_KHR) lv = 2;
        else if (q[k].CType == VK_COMPONENT_TYPE_FLOAT32_KHR && lv < 1) lv = 1;
    }
    return lv;
}

int rvk_init(const rvk_net *net, int W, int H, const uint8_t *uuid, int mode, int export_gl, rvk_info *info)
{
    memset(info, 0, sizeof *info); info->in_fd = info->out_fd = info->sem_in_fd = info->sem_out_fd = -1;
    memset(&V, 0, sizeof V); V.W = W; V.H = H; V.exported = export_gl;
    if (net->nl > MAXL) { fail(info, "network too big"); return 0; }
    if (!(V.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL))) { fail(info, "no libvulkan.so.1"); return 0; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(V.lib, "vkGetInstanceProcAddr");
    if (!gipa) { fail(info, "no vkGetInstanceProcAddr"); return 0; }
    vkCreateInstance = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ravenvk", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (!vkCreateInstance || vkCreateInstance(&ici, 0, &V.inst)) {
        app.apiVersion = VK_API_VERSION_1_1;   // old loaders / drivers
        if (!vkCreateInstance || vkCreateInstance(&ici, 0, &V.inst)) { fail(info, "vkCreateInstance failed"); return 0; }
    }
#define L(n) n = (PFN_##n)gipa(V.inst, #n); if (!n) { fail(info, "missing " #n); return 0; }
    VK_FUNCS(L)
#undef L
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR = (void *)gipa(V.inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");

    // the GPU: the one GL runs on (UUID), else the first discrete one, else the first
    uint32_t n = 16; VkPhysicalDevice pds[16]; vkEnumeratePhysicalDevices(V.inst, &n, pds);
    const char *want = getenv("RAVENVK_DEVICE");   // tests: pick a GPU by name (e.g. llvmpipe)
    int zero_uuid = 1;
    if (uuid) for (int k = 0; k < 16; k++) if (uuid[k]) zero_uuid = 0;
    for (uint32_t i = 0; i < n && !V.pd; i++) {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (want ? strstr(p2.properties.deviceName, want) != NULL
                 : (uuid && !zero_uuid) ? !memcmp(idp.deviceUUID, uuid, 16) : p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) V.pd = pds[i];
    }
#ifdef _WIN32
    if (!V.pd && !want && uuid && !zero_uuid) { fail(info, "no Vulkan device matches the GL GPU (UUID)"); return 0; }   // never another GPU (e.g. a Tesla)
#endif
    if (!V.pd && !want) {
        // Fallback (e.g. WSL2 Dozen where GL UUID differs from Vulkan UUID): pick discrete GPU, or first non-CPU GPU
        for (uint32_t i = 0; i < n && !V.pd; i++) {
            VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr);
            if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) V.pd = pds[i];
        }
        for (uint32_t i = 0; i < n && !V.pd; i++) {
            VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pds[i], &pr);
            if (pr.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) V.pd = pds[i];
        }
        if (!V.pd && n) V.pd = pds[0];
    }
    if (!V.pd) { fail(info, "the GL GPU has no Vulkan device"); return 0; }
    vkGetPhysicalDeviceProperties(V.pd, &V.props); vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mp);
    snprintf(info->gpu, sizeof info->gpu, "%s", V.props.deviceName);
    VkPhysicalDeviceSubgroupProperties sgp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp };
    vkGetPhysicalDeviceProperties2(V.pd, &p2); V.sg = sgp.subgroupSize;

    // features: matrix cores need fp16 arithmetic + 16-bit storage + the Vulkan memory model
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceVulkanMemoryModelFeatures fmm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, &fcm };
    VkPhysicalDeviceShaderFloat16Int8Features f16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &fmm };
    VkPhysicalDevice16BitStorageFeatures s16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &f16 };
    VkPhysicalDeviceFeatures2 ff = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s16 };
    int cml = V.props.apiVersion >= VK_API_VERSION_1_3 ? coopmat_level() : 0;
    if (cml) vkGetPhysicalDeviceFeatures2(V.pd, &ff);
    int cm = cml && fcm.cooperativeMatrix && f16.shaderFloat16 && s16.storageBuffer16BitAccess && fmm.vulkanMemoryModel && sgp.subgroupSize >= 16;
    if (mode == 1) cm = 0;
    if (mode == 2 && !cm) { fail(info, "no matrix cores (16x16x16 fp16) on this GPU"); return 0; }
    info->coopmat = cm; info->acc16 = cm && cml == 2 && rvk_acc_pref != 2;   // fp16 accumulate is a candidate
    if (export_gl && !(has_ext(EXT_MEM) && has_ext(EXT_SEM))) {
        fail(info, "no " EXT_MEM " / " EXT_SEM); return 0;
    }
    uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = 0; while (V.qf < nq && !(qp[V.qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) V.qf++;
    if (V.qf == nq) { fail(info, "no compute queue"); return 0; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf, .queueCount = 1, .pQueuePriorities = &prio };
    const char *ext[4]; int ne = 0;
    if (export_gl) { ext[ne++] = EXT_MEM; ext[ne++] = EXT_SEM; }
    if (cm) ext[ne++] = "VK_KHR_cooperative_matrix";
    // enable only what the matrix-core path needs (and only when it is used)
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR ecm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, .cooperativeMatrix = 1 };
    VkPhysicalDeviceVulkanMemoryModelFeatures emm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, &ecm, .vulkanMemoryModel = 1 };
    VkPhysicalDeviceShaderFloat16Int8Features e16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &emm, .shaderFloat16 = 1 };
    VkPhysicalDevice16BitStorageFeatures es16 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &e16, .storageBuffer16BitAccess = 1 };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, cm ? &es16 : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = ne, .ppEnabledExtensionNames = ext };
    CK(vkCreateDevice(V.pd, &dci, 0, &V.dev), "vkCreateDevice failed");
#define L(n) n = (PFN_##n)vkGetDeviceProcAddr(V.dev, #n); if (!n) { fail(info, "missing " #n); return 0; }
    DEV_FUNCS(L)
#undef L
    if (export_gl) {
#ifdef _WIN32
        vkGetMemoryWin32HandleKHR = (void *)vkGetDeviceProcAddr(V.dev, "vkGetMemoryWin32HandleKHR");
        vkGetSemaphoreWin32HandleKHR = (void *)vkGetDeviceProcAddr(V.dev, "vkGetSemaphoreWin32HandleKHR");
        if (!vkGetMemoryWin32HandleKHR || !vkGetSemaphoreWin32HandleKHR) { fail(info, "missing win32 export functions"); return 0; }
#else
        vkGetMemoryFdKHR = (void *)vkGetDeviceProcAddr(V.dev, "vkGetMemoryFdKHR");
        vkGetSemaphoreFdKHR = (void *)vkGetDeviceProcAddr(V.dev, "vkGetSemaphoreFdKHR");
        if (!vkGetMemoryFdKHR || !vkGetSemaphoreFdKHR) { fail(info, "missing fd export functions"); return 0; }
#endif
    }
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);

    VkDescriptorSetLayoutBinding lb[4];
    for (int i = 0; i < 4; i++) lb[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = lb };
    CK(vkCreateDescriptorSetLayout(V.dev, &dli, 0, &V.dsl), "descriptor layout");
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &V.dsl,
                                       .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    CK(vkCreatePipelineLayout(V.dev, &pli, 0, &V.pl), "pipeline layout");
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * MAXP };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = MAXP, .poolSizeCount = 1, .pPoolSizes = &ps };
    CK(vkCreateDescriptorPool(V.dev, &dpi, 0, &V.dpool), "descriptor pool");
    VkCommandPoolCreateInfo cpo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf };
    CK(vkCreateCommandPool(V.dev, &cpo, 0, &V.cpool), "command pool");
    VkCommandBuffer cbs[2];
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V.cpool, .commandBufferCount = 2 };
    CK(vkAllocateCommandBuffers(V.dev, &cai, cbs), "command buffers"); V.cb = cbs[0]; V.tcb = cbs[1];
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CK(vkCreateFence(V.dev, &fci, 0, &V.fence), "fence");
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    CK(vkCreateQueryPool(V.dev, &qpi, 0, &V.qpool), "query pool");
    if (!qp[V.qf].timestampValidBits) V.props.limits.timestampPeriod = 0;   // no autotune: first variant wins
    if (export_gl) {
        VkExportSemaphoreCreateInfo es = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = HT_SEM };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &es };
        CK(vkCreateSemaphore(V.dev, &sci, 0, &V.sem_in), "semaphore"); CK(vkCreateSemaphore(V.dev, &sci, 0, &V.sem_out), "semaphore");
    }

    // frame buffers (RGBA16F) and the input tensor
    size_t npx = (size_t)W * H;
    if (!mkbuf(&V.in, npx * 8, 0, export_gl) || !mkbuf(&V.out, npx * 8, 0, export_gl)) { fail(info, "out of GPU memory (frame buffers)"); return 0; }
    info->in_size = V.in.n; info->out_size = V.out.n;
    VkDeviceSize wtotal = 0;
    for (int l = 0; l < net->nl; l++) wtotal += 9 * (VkDeviceSize)r16(net->cin[l]) * r16(net->cout[l]) * 2 + 256 + 512;
    if (!mkbuf(&V.stage, wtotal + (export_gl ? 0 : npx * 8), 1, 0)) { fail(info, "out of memory (staging)"); return 0; }
    CK(vkMapMemory(V.dev, V.stage.m, 0, V.stage.n, 0, (void **)&V.sp), "map"); char *sp = V.sp;
    if (!mktensor(&V.T[0], W / 2, H / 2, 16)) { fail(info, "out of GPU memory"); return 0; }
    static const int s1[] = { 0 };
    VkPipeline unshuf = pipeline(spv_misc1, sizeof spv_misc1, s1, 0), upcat = pipeline(spv_misc2, sizeof spv_misc2, s1, 0),
               shuf = pipeline(spv_misc3, sizeof spv_misc3, s1, 0);
    if (!unshuf || !upcat || !shuf) { fail(info, "helper pipelines failed"); return 0; }
    Pass *s = addpass(&V.in, 0, &V.T[0].b, 0);
    s->p = unshuf; s->pc[0] = W; s->pc[1] = H; s->pc[6] = W / 2 + 2; s->gx = (W / 2 + 15) / 16; s->gy = (H / 2 + 15) / 16; s->gz = 1;

    VkBufferCopy cp[2 * MAXL]; Buf *cpd[2 * MAXL]; int ncp = 0; size_t sofs = 0;
    int spare = MAXL + 1;
    for (int L = 0; L < net->nl; L++) {
        int s0 = net->src0[L], m0 = net->mode0[L], s1i = net->src1[L], cin = net->cin[L], cout = net->cout[L];
        Tensor *in = &V.T[s0]; int ow = in->w, oh = in->h, stride = 1;
        if (s1i >= 0) {
            Tensor *k = &V.T[s1i]; ow = k->w; oh = k->h; Tensor *cat = &V.T[spare++];
            if (in->c + k->c != r16(cin) || !mktensor(cat, ow, oh, in->c + k->c)) { fail(info, "concat setup failed"); return 0; }
            Pass *q = addpass(&in->b, &k->b, &cat->b, 0); q->p = upcat;
            uint32_t pc[] = { ow, oh, in->c, k->c, in->w + 2, k->w + 2, ow + 2 }; memcpy(q->pc, pc, sizeof pc);
            q->gx = ((size_t)ow * oh * (cat->c / 8) + 63) / 64; q->gy = q->gz = 1;
            in = cat;
        } else if (m0 == 1) { ow = (in->w + 1) / 2; oh = (in->h + 1) / 2; stride = 2; }
        int CI = in->c, CO = r16(cout);
        if (CI != r16(cin) || !mktensor(&V.T[L + 1], ow, oh, CO)) { fail(info, "layer setup failed"); return 0; }
        // weights [tap][kc][nb][16][16] fp16 (zero padded) + bias fp32, via staging
        int KC = CI / 16, NB = CO / 16; size_t nw = 9 * (size_t)CI * CO;
        _Float16 *wt = (_Float16 *)(sp + sofs); memset(wt, 0, nw * 2);
        const float *w = net->w[L];
        for (int t = 0; t < 9; t++) for (int ci = 0; ci < cin; ci++) for (int co = 0; co < cout; co++)
            wt[(((size_t)t * KC + ci / 16) * NB + co / 16) * 256 + (ci % 16) * 16 + co % 16] = (_Float16)w[((size_t)co * cin + ci) * 9 + t];
        if (!mkbuf(&V.wb[L], nw * 2, 0, 0) || !mkbuf(&V.bb[L], CO * 4, 0, 0)) { fail(info, "out of GPU memory (weights)"); return 0; }
        cp[ncp] = (VkBufferCopy){ sofs, 0, nw * 2 }; cpd[ncp++] = &V.wb[L]; sofs += (nw * 2 + 255) & ~(size_t)255;
        float *bp = (float *)(sp + sofs); memset(bp, 0, CO * 4); memcpy(bp, net->b[L], cout * 4);
        cp[ncp] = (VkBufferCopy){ sofs, 0, CO * 4 }; cpd[ncp++] = &V.bb[L]; sofs += 512;
        Pass *q = addpass(&in->b, &V.wb[L], &V.bb[L], &V.T[L + 1].b);
        uint32_t pc[] = { ow, oh, in->w + 2, ow + 2 }; memcpy(q->pc, pc, sizeof pc);
        int relu = L < net->nl - 1;
        // matrix cores: fp16 accumulate (if the GPU has it) and fp32 accumulate both go into the autotune, because
        // which is faster depends on the card (GeForce caps fp32 accumulate at half rate unless unlocked).
        if (cm) for (int acc = 2; acc >= 1; acc--) {
            if (acc == 2 ? !info->acc16 : rvk_acc_pref == 1 && info->acc16) continue;
            for (int MT = 1; MT <= 4; MT *= 2) for (int NBS = NB; NBS >= 1 && NBS * 2 >= NB; NBS /= 2) {
                if (MT * NBS > 8 || NB % NBS || q->nalt >= 21) continue;
                int sp_[] = { CI, CO, stride, MT, NBS, relu, V.sg };
                Variant *v = &q->alt[q->nalt];
                v->p = acc == 2 ? pipeline(spv_cm_a1, sizeof spv_cm_a1, sp_, 7) : pipeline(spv_cm_a0, sizeof spv_cm_a0, sp_, 7);
                if (!v->p) continue;
                v->gx = (ow + 16 * MT - 1) / (16 * MT); v->gy = (oh + 3) / 4; v->gz = NB / NBS; v->acc = acc; q->nalt++;
            }
        }
        for (int cob = 32; cob >= 8; cob /= 2) {     // plain kernels: always there (the fallback)
            if (CO % cob || q->nalt >= 24) continue;
            int sp_[] = { CI, CO, stride, cob, relu };
            Variant *v = &q->alt[q->nalt];
            if (!(v->p = pipeline(spv_plain, sizeof spv_plain, sp_, 5))) continue;
            v->gx = (ow + 15) / 16; v->gy = (oh + 7) / 8; v->gz = CO / cob; v->acc = 0; q->nalt++;
            if (!V.props.limits.timestampPeriod) break;
        }
        if (!q->nalt) { fail(info, "no conv pipeline could be built"); return 0; }
    }
    Tensor *R = &V.T[net->nl];
    s = addpass(&R->b, &V.in, &V.out, 0); s->p = shuf;
    s->pc[0] = W; s->pc[1] = H; s->pc[4] = R->w + 2; s->gx = (W + 15) / 16; s->gy = (H + 15) / 16; s->gz = 1;

    // upload weights, zero the tensors (borders must stay 0)
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(V.tcb, &cbi);
    for (int i = 0; i < ncp; i++) vkCmdCopyBuffer(V.tcb, V.stage.b, cpd[i]->b, 1, &cp[i]);
    for (int i = 0; i < 2 * MAXL + 2; i++) if (V.T[i].b.b) vkCmdFillBuffer(V.tcb, V.T[i].b.b, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(V.tcb, V.in.b, 0, VK_WHOLE_SIZE, 0);
    vkCmdPipelineBarrier(V.tcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &MB, 0, 0, 0, 0);
    vkEndCommandBuffer(V.tcb);
    if (!submit_wait(V.tcb)) { fail(info, "upload failed"); return 0; }

    // autotune: fastest variant per conv
    for (int i = 0; i < V.np; i++) {
        Pass *q = &V.pass[i]; if (!q->nalt) continue;
        double best = 1e18; int bi = 0;
        for (int a = 0; a < q->nalt; a++) { double ms = time_variant(q, &q->alt[a]); if (ms < best) { best = ms; bi = a; } }
        q->p = q->alt[bi].p; q->gx = q->alt[bi].gx; q->gy = q->alt[bi].gy; q->gz = q->alt[bi].gz;
        info->n_conv++; if (q->alt[bi].acc == 2) info->n_acc16++; else if (q->alt[bi].acc == 1) info->n_acc32++;
    }
    // the per-frame command buffer
    vkBeginCommandBuffer(V.cb, &cbi);
    for (int i = 0; i < V.np; i++) record(V.cb, &V.pass[i], NULL, 1);
    vkEndCommandBuffer(V.cb);
    // one timed run of the whole net
    if (V.props.limits.timestampPeriod) {
        vkResetCommandBuffer(V.tcb, 0); vkBeginCommandBuffer(V.tcb, &cbi);
        vkCmdResetQueryPool(V.tcb, V.qpool, 0, 2);
        vkCmdWriteTimestamp(V.tcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.qpool, 0);
        for (int i = 0; i < V.np; i++) record(V.tcb, &V.pass[i], NULL, 1);
        vkCmdWriteTimestamp(V.tcb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.qpool, 1);
        vkEndCommandBuffer(V.tcb);
        uint64_t ts[2];
        if (submit_wait(V.tcb) && !vkGetQueryPoolResults(V.dev, V.qpool, 0, 2, 16, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT))
            info->ms = (ts[1] - ts[0]) * V.props.limits.timestampPeriod / 1e6;
    }
    if (export_gl) {
#ifdef _WIN32
        VkMemoryGetWin32HandleInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, .memory = V.in.m, .handleType = HT_MEM };
        CK(vkGetMemoryWin32HandleKHR(V.dev, &gi, (HANDLE *)&info->in_h), "export memory");
        gi.memory = V.out.m; CK(vkGetMemoryWin32HandleKHR(V.dev, &gi, (HANDLE *)&info->out_h), "export memory");
        VkSemaphoreGetWin32HandleInfoKHR sg = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, .semaphore = V.sem_in, .handleType = HT_SEM };
        CK(vkGetSemaphoreWin32HandleKHR(V.dev, &sg, (HANDLE *)&info->sem_in_h), "export semaphore");
        sg.semaphore = V.sem_out; CK(vkGetSemaphoreWin32HandleKHR(V.dev, &sg, (HANDLE *)&info->sem_out_h), "export semaphore");
#else
        VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = V.in.m, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        CK(vkGetMemoryFdKHR(V.dev, &gi, &info->in_fd), "export memory");
        gi.memory = V.out.m; CK(vkGetMemoryFdKHR(V.dev, &gi, &info->out_fd), "export memory");
        VkSemaphoreGetFdInfoKHR sg = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = V.sem_in, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
        CK(vkGetSemaphoreFdKHR(V.dev, &sg, &info->sem_in_fd), "export semaphore");
        sg.semaphore = V.sem_out; CK(vkGetSemaphoreFdKHR(V.dev, &sg, &info->sem_out_fd), "export semaphore");
#endif
    }
    info->ok = 1;
    return 1;
}

int rvk_run(void)
{
    if (!V.dev) return 0;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &V.cb };
    if (V.exported) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &V.sem_in; si.pWaitDstStageMask = &ws;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &V.sem_out;
        // the previous frame's run must be finished before the command buffer is submitted again
        vkWaitForFences(V.dev, 1, &V.fence, 1, 1000000000ull);
        vkResetFences(V.dev, 1, &V.fence);
        return vkQueueSubmit(V.q, 1, &si, V.fence) == VK_SUCCESS;
    }
    vkResetFences(V.dev, 1, &V.fence);
    if (vkQueueSubmit(V.q, 1, &si, V.fence)) return 0;
    return vkWaitForFences(V.dev, 1, &V.fence, 1, 5000000000ull) == VK_SUCCESS;
}

static int copy(Buf *dst, Buf *src, size_t n) {
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkResetCommandBuffer(V.tcb, 0); vkBeginCommandBuffer(V.tcb, &bi);
    vkCmdCopyBuffer(V.tcb, src->b, dst->b, 1, &(VkBufferCopy){ src == &V.stage ? V.stage.n - (size_t)V.W * V.H * 8 : 0,
                                                                dst == &V.stage ? V.stage.n - (size_t)V.W * V.H * 8 : 0, n });
    vkEndCommandBuffer(V.tcb);
    return submit_wait(V.tcb);
}
int rvk_upload(const void *p) {
    if (V.exported) return 0;
    size_t n = (size_t)V.W * V.H * 8;
    memcpy(V.sp + V.stage.n - n, p, n);
    return copy(&V.in, &V.stage, n);
}
int rvk_download(void *p) {
    if (V.exported) return 0;
    size_t n = (size_t)V.W * V.H * 8;
    if (!copy(&V.stage, &V.out, n)) return 0;
    memcpy(p, V.sp + V.stage.n - n, n);
    return 1;
}
void rvk_destroy(void)
{
    // the device owns everything; destroying it frees the memory (GL keeps its imported copies alive)
    if (V.dev) { vkDeviceWaitIdle(V.dev); vkDestroyDevice(V.dev, 0); }
    if (V.inst) vkDestroyInstance(V.inst, 0);
    memset(&V, 0, sizeof V);
}
