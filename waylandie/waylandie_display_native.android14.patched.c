#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <android/surface_control_jni.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include "ahb_vk_3d_shaders.h"

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeStatus(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return (*env)->NewStringUTF(env, "native-ok-arm64");
}

static jstring make_status(JNIEnv *env, const char *format, ...) {
    char status[192];
    va_list args;
    va_start(args, format);
    vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    return (*env)->NewStringUTF(env, status);
}

static int has_extension_name(
        const VkExtensionProperties *extensions,
        uint32_t extension_count,
        const char *name) {
    for (uint32_t i = 0; i < extension_count; i++) {
        if (strcmp(extensions[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void format_vulkan_api_version(uint32_t version, char *text, size_t text_size) {
    uint32_t major = VK_VERSION_MAJOR(version);
    uint32_t minor = VK_VERSION_MINOR(version);
    uint32_t patch = VK_VERSION_PATCH(version);
    if (patch > 0) {
        snprintf(text, text_size, "%u.%u.%u", major, minor, patch);
    } else {
        snprintf(text, text_size, "%u.%u", major, minor);
    }
}

static void format_bridge_token(const char *source, char *token, size_t token_size) {
    size_t write_index = 0;
    if (token_size == 0) {
        return;
    }
    if (source == NULL || source[0] == '\0') {
        snprintf(token, token_size, "unknown");
        return;
    }
    for (size_t i = 0; source[i] != '\0' && write_index + 1 < token_size; i++) {
        char c = source[i];
        token[write_index++] = (c <= 0x20 || c == '"' || c == '\'') ? '_' : c;
    }
    token[write_index] = '\0';
    if (write_index == 0) {
        snprintf(token, token_size, "unknown");
    }
}

static const char *vk_result_token(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "success";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "out-of-host-memory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "out-of-device-memory";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "init-failed";
        case VK_ERROR_DEVICE_LOST:
            return "device-lost";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "memory-map-failed";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "missing-extension";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "missing-feature";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "too-many-objects";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE:
            return "invalid-external-handle";
        default:
            return "vk-error";
    }
}

#define WAYLANDIE_DRM_FOURCC_CODE(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#define WAYLANDIE_DRM_FORMAT_XRGB8888 WAYLANDIE_DRM_FOURCC_CODE('X', 'R', '2', '4')
#define WAYLANDIE_DRM_FORMAT_ARGB8888 WAYLANDIE_DRM_FOURCC_CODE('A', 'R', '2', '4')
#define WAYLANDIE_DRM_FORMAT_XBGR8888 WAYLANDIE_DRM_FOURCC_CODE('X', 'B', '2', '4')
#define WAYLANDIE_DRM_FORMAT_ABGR8888 WAYLANDIE_DRM_FOURCC_CODE('A', 'B', '2', '4')
#define WAYLANDIE_PRESENT_COMPOSITION_NUDGE_ALPHA 0.999f

#define WAYLANDIE_KGSL_IOC_TYPE 0x09
#define WAYLANDIE_KGSL_USER_MEM_TYPE_DMABUF 0x00000003

struct waylandie_kgsl_gpuobj_import {
    uint64_t priv;
    uint64_t priv_len;
    uint64_t flags;
    unsigned int type;
    unsigned int id;
};

struct waylandie_kgsl_gpuobj_import_dma_buf {
    int fd;
};

struct waylandie_kgsl_gpuobj_info {
    uint64_t gpuaddr;
    uint64_t flags;
    uint64_t size;
    uint64_t va_len;
    uint64_t va_addr;
    unsigned int id;
};

struct waylandie_kgsl_gpuobj_free {
    uint64_t flags;
    uint64_t priv;
    unsigned int id;
    unsigned int type;
    unsigned int len;
};

#define WAYLANDIE_IOCTL_KGSL_GPUOBJ_FREE \
    _IOW(WAYLANDIE_KGSL_IOC_TYPE, 0x46, struct waylandie_kgsl_gpuobj_free)
#define WAYLANDIE_IOCTL_KGSL_GPUOBJ_INFO \
    _IOWR(WAYLANDIE_KGSL_IOC_TYPE, 0x47, struct waylandie_kgsl_gpuobj_info)
#define WAYLANDIE_IOCTL_KGSL_GPUOBJ_IMPORT \
    _IOWR(WAYLANDIE_KGSL_IOC_TYPE, 0x48, struct waylandie_kgsl_gpuobj_import)

static const char *errno_token(int value) {
    switch (value) {
        case 0:
            return "none";
        case EACCES:
            return "eacces";
        case EPERM:
            return "eperm";
        case EINVAL:
            return "einval";
        case ENODEV:
            return "enodev";
        case ENOENT:
            return "enoent";
        case ENOMEM:
            return "enomem";
#if defined(ENOTSUP) && (!defined(EOPNOTSUPP) || ENOTSUP != EOPNOTSUPP)
        case ENOTSUP:
            return "enotsup";
#endif
#ifdef EOPNOTSUPP
        case EOPNOTSUPP:
            return "eopnotsupp";
#endif
        case EFAULT:
            return "efault";
        default:
            return "errno";
    }
}

static VkFormat map_drm_format_to_vk_format(uint32_t drm_format) {
    switch (drm_format) {
        case WAYLANDIE_DRM_FORMAT_XRGB8888:
        case WAYLANDIE_DRM_FORMAT_ARGB8888:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case WAYLANDIE_DRM_FORMAT_XBGR8888:
        case WAYLANDIE_DRM_FORMAT_ABGR8888:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeProbeKgslDmaBufImport(
        JNIEnv *env,
        jclass clazz,
        jint fd) {
    (void)clazz;
    int kgsl_fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgsl_fd < 0) {
        int saved_errno = errno;
        return make_status(
                env,
                "probe=kgsl-dmabuf-import stage=open-kgsl result=-1 errno=%d reason=%s status=fail",
                saved_errno,
                errno_token(saved_errno));
    }

    struct waylandie_kgsl_gpuobj_import_dma_buf import_dmabuf = {
            .fd = fd,
    };
    struct waylandie_kgsl_gpuobj_import import_req = {
            .priv = (uint64_t)(uintptr_t)&import_dmabuf,
            .priv_len = sizeof(import_dmabuf),
            .flags = 0,
            .type = WAYLANDIE_KGSL_USER_MEM_TYPE_DMABUF,
            .id = 0,
    };
    int result = ioctl(kgsl_fd, WAYLANDIE_IOCTL_KGSL_GPUOBJ_IMPORT, &import_req);
    if (result != 0) {
        int saved_errno = errno;
        close(kgsl_fd);
        return make_status(
                env,
                "probe=kgsl-dmabuf-import stage=gpuobj-import result=-1 errno=%d reason=%s status=fail",
                saved_errno,
                errno_token(saved_errno));
    }

    struct waylandie_kgsl_gpuobj_info info = {
            .id = import_req.id,
    };
    result = ioctl(kgsl_fd, WAYLANDIE_IOCTL_KGSL_GPUOBJ_INFO, &info);
    int info_errno = result == 0 ? 0 : errno;

    struct waylandie_kgsl_gpuobj_free free_req = {
            .id = import_req.id,
    };
    int free_result = ioctl(kgsl_fd, WAYLANDIE_IOCTL_KGSL_GPUOBJ_FREE, &free_req);
    int free_errno = free_result == 0 ? 0 : errno;
    close(kgsl_fd);

    if (result != 0) {
        return make_status(
                env,
                "probe=kgsl-dmabuf-import stage=gpuobj-info id=%u result=-1 errno=%d reason=%s free-result=%d free-errno=%d status=fail",
                import_req.id,
                info_errno,
                errno_token(info_errno),
                free_result,
                free_errno);
    }
    return make_status(
            env,
            "probe=kgsl-dmabuf-import stage=gpuobj-import id=%u gpuaddr=0x%llx size=%llu flags=0x%llx va=0x%llx va-len=%llu free-result=%d free-errno=%d status=pass",
            import_req.id,
            (unsigned long long)info.gpuaddr,
            (unsigned long long)info.size,
            (unsigned long long)info.flags,
            (unsigned long long)info.va_addr,
            (unsigned long long)info.va_len,
            free_result,
            free_errno);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeProbeDmaBufImport(
        JNIEnv *env,
        jclass clazz,
        jint fd,
        jlong width,
        jlong height,
        jlong drm_format,
        jlong modifier,
        jint planes,
        jlong stride0,
        jlong offset0,
        jlong size,
        jstring loader_string,
        jstring tmp_dir_string,
        jstring hook_lib_dir_string,
        jstring driver_dir_string,
        jstring driver_name_string) {
    (void)clazz;

    char status[1536] = {0};
    char loader_token[32] = "system";
    char adrenotools_path[512] = {0};
    char loader_driver_dir[512] = {0};
    char driver_path[512] = {0};
    void *vulkan = NULL;
    void *adrenotools = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkExtensionProperties *extensions = NULL;
    VkQueueFamilyProperties *queue_families = NULL;
    PFN_vkDestroyInstance vkDestroyInstance = NULL;
    PFN_vkDestroyDevice vkDestroyDevice = NULL;
    const char *loader = NULL;
    const char *tmp_dir = NULL;
    const char *hook_lib_dir = NULL;
    const char *driver_dir = NULL;
    const char *driver_name = NULL;

    if (fd < 0) {
        return make_status(
                env,
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-fd");
    }
    if (width <= 0 || width > UINT32_MAX || height <= 0 || height > UINT32_MAX) {
        return make_status(
                env,
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-size");
    }
    if (drm_format < 0 || drm_format > UINT32_MAX) {
        return make_status(
                env,
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-format");
    }
    if (modifier < 0 || size <= 0 || stride0 <= 0 || offset0 < 0) {
        return make_status(
                env,
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-layout");
    }
    if (planes != 1) {
        return make_status(
                env,
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=multi-plane-future");
    }

    if (loader_string != NULL) {
        loader = (*env)->GetStringUTFChars(env, loader_string, NULL);
    }
    if (tmp_dir_string != NULL) {
        tmp_dir = (*env)->GetStringUTFChars(env, tmp_dir_string, NULL);
    }
    if (hook_lib_dir_string != NULL) {
        hook_lib_dir = (*env)->GetStringUTFChars(env, hook_lib_dir_string, NULL);
    }
    if (driver_dir_string != NULL) {
        driver_dir = (*env)->GetStringUTFChars(env, driver_dir_string, NULL);
    }
    if (driver_name_string != NULL) {
        driver_name = (*env)->GetStringUTFChars(env, driver_name_string, NULL);
    }
    if (loader_string != NULL && loader == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=loader-string-oom");
        goto cleanup;
    }
    if (loader == NULL || loader[0] == '\0' || strcmp(loader, "system") == 0) {
        snprintf(loader_token, sizeof(loader_token), "system");
        vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (vulkan == NULL) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=system api=vkGetMemoryFdPropertiesKHR status=unsupported reason=no-loader");
            goto cleanup;
        }
    } else if (strcmp(loader, "adrenotools") == 0) {
        enum {
            ADRENOTOOLS_DRIVER_CUSTOM_FLAG = 1 << 0,
        };
        typedef void *(*PFN_adrenotools_open_libvulkan)(
                int dlopenMode,
                int featureFlags,
                const char *tmpLibDir,
                const char *hookLibDir,
                const char *customDriverDir,
                const char *customDriverName,
                const char *fileRedirectDir,
                void **userMappingHandle);
        snprintf(loader_token, sizeof(loader_token), "adrenotools");
        if (tmp_dir == NULL
                || hook_lib_dir == NULL
                || driver_dir == NULL
                || driver_name == NULL) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR status=unsupported reason=bad-loader-args");
            goto cleanup;
        }
        if (driver_name[0] == '\0' || strchr(driver_name, '/') != NULL) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-driver-name driver=%s",
                    driver_name[0] == '\0' ? "empty" : driver_name);
            goto cleanup;
        }
        size_t driver_dir_len = strlen(driver_dir);
        if (driver_dir_len > 0 && driver_dir[driver_dir_len - 1] == '/') {
            snprintf(loader_driver_dir, sizeof(loader_driver_dir), "%s", driver_dir);
        } else {
            snprintf(loader_driver_dir, sizeof(loader_driver_dir), "%s/", driver_dir);
        }
        mkdir(tmp_dir, 0700);
        snprintf(driver_path, sizeof(driver_path), "%s%s", loader_driver_dir, driver_name);
        if (access(driver_path, R_OK) != 0) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR status=unsupported reason=driver-missing driver=%s driver-dir=%s expected=%s",
                    driver_name,
                    loader_driver_dir,
                    driver_path);
            goto cleanup;
        }

        snprintf(adrenotools_path, sizeof(adrenotools_path), "%s/libadrenotools.so", hook_lib_dir);
        adrenotools = dlopen(adrenotools_path, RTLD_NOW | RTLD_LOCAL);
        if (adrenotools == NULL) {
            adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (adrenotools == NULL) {
            const char *error = dlerror();
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR status=unsupported reason=no-libadrenotools hook-dir=%s dlerror=%s",
                    hook_lib_dir,
                    error == NULL ? "none" : error);
            goto cleanup;
        }
        PFN_adrenotools_open_libvulkan adrenotools_open_libvulkan =
                (PFN_adrenotools_open_libvulkan)dlsym(
                        adrenotools,
                        "adrenotools_open_libvulkan");
        if (adrenotools_open_libvulkan == NULL) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR status=unsupported reason=missing-open-symbol");
            goto cleanup;
        }
        vulkan = adrenotools_open_libvulkan(
                RTLD_NOW | RTLD_LOCAL,
                ADRENOTOOLS_DRIVER_CUSTOM_FLAG,
                tmp_dir,
                hook_lib_dir,
                loader_driver_dir,
                driver_name,
                NULL,
                NULL);
        if (vulkan == NULL) {
            const char *error = dlerror();
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=adrenotools api=vkGetMemoryFdPropertiesKHR driver=%s status=fail reason=open-libvulkan dlerror=%s",
                    driver_name,
                    error == NULL ? "none" : error);
            goto cleanup;
        }
    } else {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR status=fail reason=bad-loader",
                loader);
        goto cleanup;
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    if (vkGetInstanceProcAddr == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=no-gip");
        goto cleanup;
    }

    PFN_vkCreateInstance vkCreateInstance =
            (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (vkCreateInstance == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=no-create-instance");
        goto cleanup;
    }

    VkApplicationInfo app_info = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "WayLandIEDmaBufImportProbe",
            .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
            .pEngineName = "none",
            .engineVersion = VK_MAKE_VERSION(0, 0, 0),
            .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
    };
    VkResult result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=create-instance result=%d",
                result);
        goto cleanup;
    }

    vkDestroyInstance =
            (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
            (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
            (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice vkCreateDevice =
            (PFN_vkCreateDevice)vkGetInstanceProcAddr(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr =
            (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
    if (vkDestroyInstance == NULL
            || vkEnumeratePhysicalDevices == NULL
            || vkGetPhysicalDeviceProperties == NULL
            || vkEnumerateDeviceExtensionProperties == NULL
            || vkGetPhysicalDeviceQueueFamilyProperties == NULL
            || vkCreateDevice == NULL
            || vkGetDeviceProcAddr == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=missing-vulkan-symbol");
        goto cleanup;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=no-device result=%d count=%u",
                result,
                device_count);
        goto cleanup;
    }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t one_device = 1;
    result = vkEnumeratePhysicalDevices(instance, &one_device, &physical_device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE)
            || physical_device == VK_NULL_HANDLE) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=enum-device result=%d",
                result);
        goto cleanup;
    }

    VkPhysicalDeviceProperties device_properties;
    char gpu_token[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    memset(&device_properties, 0, sizeof(device_properties));
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);
    format_bridge_token(device_properties.deviceName, gpu_token, sizeof(gpu_token));

    uint32_t extension_count = 0;
    result = vkEnumerateDeviceExtensionProperties(
            physical_device,
            NULL,
            &extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=dev-ext-count result=%d",
                gpu_token,
                result);
        goto cleanup;
    }
    if (extension_count > 0) {
        extensions = (VkExtensionProperties *)calloc(extension_count, sizeof(VkExtensionProperties));
        if (extensions == NULL) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=oom-ext",
                    gpu_token);
            goto cleanup;
        }
        result = vkEnumerateDeviceExtensionProperties(
                physical_device,
                NULL,
                &extension_count,
                extensions);
        if (result != VK_SUCCESS) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=dev-ext result=%d",
                    gpu_token,
                    result);
            goto cleanup;
        }
    }

    int has_external_memory = has_extension_name(
            extensions,
            extension_count,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    int has_external_memory_fd = has_extension_name(
            extensions,
            extension_count,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    int has_dma_buf = has_extension_name(
            extensions,
            extension_count,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    int has_drm_modifier_ext = has_extension_name(
            extensions,
            extension_count,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    if (!has_external_memory || !has_external_memory_fd || !has_dma_buf) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=%s fd-ext=%s dma-buf-ext=%s status=unsupported reason=missing-extension",
                gpu_token,
                has_external_memory ? "yes" : "no",
                has_external_memory_fd ? "yes" : "no",
                has_dma_buf ? "yes" : "no");
        goto cleanup;
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, NULL);
    if (queue_family_count == 0) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=no-queue",
                gpu_token);
        goto cleanup;
    }
    queue_families = (VkQueueFamilyProperties *)calloc(
            queue_family_count,
            sizeof(VkQueueFamilyProperties));
    if (queue_families == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=oom-queue",
                gpu_token);
        goto cleanup;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(
            physical_device,
            &queue_family_count,
            queue_families);
    uint32_t queue_family_index = UINT32_MAX;
    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueCount > 0) {
            queue_family_index = i;
            break;
        }
    }
    if (queue_family_index == UINT32_MAX) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=no-usable-queue",
                gpu_token);
        goto cleanup;
    }

    VkFormat vk_format = map_drm_format_to_vk_format((uint32_t)drm_format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=unmapped-drm-format drm-format=0x%llx",
                gpu_token,
                (unsigned long long)drm_format);
        goto cleanup;
    }
    if (modifier != 0 && !has_drm_modifier_ext) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=missing-drm-modifier-ext modifier=0x%llx",
                gpu_token,
                (unsigned long long)modifier);
        goto cleanup;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
    };
    const char *enabled_extensions[4] = {
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            NULL,
    };
    uint32_t enabled_extension_count = 3;
    if (modifier != 0 && has_drm_modifier_ext) {
        enabled_extensions[enabled_extension_count++] =
                VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
    }
    VkDeviceCreateInfo device_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = enabled_extension_count,
            .ppEnabledExtensionNames = enabled_extensions,
    };
    result = vkCreateDevice(physical_device, &device_info, NULL, &device);
    if (result != VK_SUCCESS || device == VK_NULL_HANDLE) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes status=unsupported reason=create-device result=%d",
                gpu_token,
                result);
        goto cleanup;
    }

    vkDestroyDevice = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(device, "vkDestroyDevice");
    PFN_vkCreateImage vkCreateImage =
            (PFN_vkCreateImage)vkGetDeviceProcAddr(device, "vkCreateImage");
    PFN_vkDestroyImage vkDestroyImage =
            (PFN_vkDestroyImage)vkGetDeviceProcAddr(device, "vkDestroyImage");
    PFN_vkGetImageMemoryRequirements2 vkGetImageMemoryRequirements2 =
            (PFN_vkGetImageMemoryRequirements2)vkGetDeviceProcAddr(
                    device,
                    "vkGetImageMemoryRequirements2");
    PFN_vkAllocateMemory vkAllocateMemory =
            (PFN_vkAllocateMemory)vkGetDeviceProcAddr(device, "vkAllocateMemory");
    PFN_vkFreeMemory vkFreeMemory =
            (PFN_vkFreeMemory)vkGetDeviceProcAddr(device, "vkFreeMemory");
    PFN_vkBindImageMemory vkBindImageMemory =
            (PFN_vkBindImageMemory)vkGetDeviceProcAddr(device, "vkBindImageMemory");
    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR =
            (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
                    device,
                    "vkGetMemoryFdPropertiesKHR");
    if (vkDestroyDevice == NULL
            || vkCreateImage == NULL
            || vkDestroyImage == NULL
            || vkGetImageMemoryRequirements2 == NULL
            || vkAllocateMemory == NULL
            || vkFreeMemory == NULL
            || vkBindImageMemory == NULL
            || vkGetMemoryFdPropertiesKHR == NULL) {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR gpu=%s status=unsupported reason=missing-device-proc",
                gpu_token);
        goto cleanup;
    }

    VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
            .memoryTypeBits = 0,
    };
    result = vkGetMemoryFdPropertiesKHR(
            device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            (int)fd,
            &fd_properties);
    if (result == VK_SUCCESS) {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        int image_import_fd = -1;
        VkExternalMemoryImageCreateInfo external_image = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkSubresourceLayout plane_layout = {
                .offset = (VkDeviceSize)offset0,
                .size = (VkDeviceSize)size,
                .rowPitch = (VkDeviceSize)stride0,
                .arrayPitch = (VkDeviceSize)size,
                .depthPitch = (VkDeviceSize)size,
        };
        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                .pNext = &external_image,
                .drmFormatModifier = (uint64_t)modifier,
                .drmFormatModifierPlaneCount = 1,
                .pPlaneLayouts = &plane_layout,
        };
        int use_explicit_modifier = has_drm_modifier_ext;
        VkImageCreateInfo image_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = use_explicit_modifier
                        ? (const void *)&modifier_info
                        : (const void *)&external_image,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = vk_format,
                .extent = {
                        .width = (uint32_t)width,
                        .height = (uint32_t)height,
                        .depth = 1,
                },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = use_explicit_modifier
                        ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                        : VK_IMAGE_TILING_LINEAR,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                        | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        result = vkCreateImage(device, &image_info, NULL, &image);
        if (result != VK_SUCCESS) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes drm-modifier-ext=%s path=vk-image-bind stage=create-image result=%d reason=%s vk-format=%d modifier=0x%llx tiling=%s memory-type-bits=0x%x fd-size=%llu status=fail",
                    loader_token,
                    gpu_token,
                    has_drm_modifier_ext ? "yes" : "no",
                    result,
                    vk_result_token(result),
                    (int)vk_format,
                    (unsigned long long)modifier,
                    use_explicit_modifier ? "drm-modifier" : "linear",
                    fd_properties.memoryTypeBits,
                    (unsigned long long)size);
            goto cleanup;
        }

        VkMemoryDedicatedRequirements image_dedicated_requirements = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        };
        VkMemoryRequirements2 image_memory_requirements = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                .pNext = &image_dedicated_requirements,
        };
        VkImageMemoryRequirementsInfo2 image_requirements_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .image = image,
        };
        vkGetImageMemoryRequirements2(
                device,
                &image_requirements_info,
                &image_memory_requirements);

        uint32_t image_compatible_type_bits = fd_properties.memoryTypeBits
                & image_memory_requirements.memoryRequirements.memoryTypeBits;
        VkResult last_image_allocate_result = VK_SUCCESS;
        int selected_image_memory_type_index = -1;
        int selected_image_dedicated_allocation = 0;
        int last_image_dedicated_allocation = 0;
        VkDeviceSize selected_image_allocation_size = 0;
        VkDeviceSize last_image_allocation_size = 0;
        for (int candidate = 0; candidate < 32; candidate++) {
            if ((image_compatible_type_bits & (1u << candidate)) == 0) {
                continue;
            }
            int use_dedicated_allocation = image_dedicated_requirements.requiresDedicatedAllocation
                    || image_dedicated_requirements.prefersDedicatedAllocation;
            int attempt_count = use_dedicated_allocation ? 2 : 1;
            VkDeviceSize image_requirement_size =
                    image_memory_requirements.memoryRequirements.size;
            VkDeviceSize image_fd_size = (VkDeviceSize)size;
            VkDeviceSize image_allocation_sizes[2] = {
                    image_fd_size,
                    image_requirement_size,
            };
            int allocation_size_attempt_count =
                    image_fd_size == image_requirement_size ? 1 : 2;
            for (int size_attempt = 0;
                    size_attempt < allocation_size_attempt_count;
                    size_attempt++) {
                VkDeviceSize image_allocation_size =
                        image_allocation_sizes[size_attempt];
                if (image_allocation_size == 0) {
                    continue;
                }
                for (int attempt = 0; attempt < attempt_count; attempt++) {
                    int attempt_dedicated = use_dedicated_allocation && attempt == 0;
                image_import_fd = dup((int)fd);
                if (image_import_fd < 0) {
                    snprintf(status, sizeof(status),
                            "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes drm-modifier-ext=%s path=vk-image-bind stage=dup-fd status=fail",
                            loader_token,
                            gpu_token,
                            has_drm_modifier_ext ? "yes" : "no");
                    vkDestroyImage(device, image, NULL);
                    goto cleanup;
                }

                VkMemoryDedicatedAllocateInfo image_dedicated_allocate = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                        .image = attempt_dedicated ? image : VK_NULL_HANDLE,
                        .buffer = VK_NULL_HANDLE,
                };
                VkImportMemoryFdInfoKHR image_import_info = {
                        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                        .pNext = attempt_dedicated ? &image_dedicated_allocate : NULL,
                        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                        .fd = image_import_fd,
                };
                VkMemoryAllocateInfo image_allocate_info = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                        .pNext = &image_import_info,
                        .allocationSize = image_allocation_size,
                        .memoryTypeIndex = (uint32_t)candidate,
                };
                result = vkAllocateMemory(
                        device,
                        &image_allocate_info,
                        NULL,
                        &image_memory);
                last_image_dedicated_allocation = attempt_dedicated;
                last_image_allocation_size = image_allocation_size;
                if (result == VK_SUCCESS) {
                    image_import_fd = -1;
                    selected_image_memory_type_index = candidate;
                    selected_image_dedicated_allocation = attempt_dedicated;
                    selected_image_allocation_size = image_allocation_size;
                    break;
                }
                last_image_allocate_result = result;
                close(image_import_fd);
                image_import_fd = -1;
                }
                if (selected_image_memory_type_index >= 0) {
                    break;
                }
            }
            if (selected_image_memory_type_index >= 0) {
                break;
            }
        }

        if (selected_image_memory_type_index < 0) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes drm-modifier-ext=%s path=vk-image-bind stage=allocate-memory result=%d reason=%s memory-type-bits=0x%x image-memory-type-bits=0x%x compatible-memory-type-bits=0x%x fd-size=%llu req-size=%llu req-align=%llu dedicated-prefers=%s dedicated-requires=%s last-dedicated=%s last-alloc-size=%llu status=fail",
                    loader_token,
                    gpu_token,
                    has_drm_modifier_ext ? "yes" : "no",
                    last_image_allocate_result,
                    vk_result_token(last_image_allocate_result),
                    fd_properties.memoryTypeBits,
                    image_memory_requirements.memoryRequirements.memoryTypeBits,
                    image_compatible_type_bits,
                    (unsigned long long)size,
                    (unsigned long long)image_memory_requirements.memoryRequirements.size,
                    (unsigned long long)image_memory_requirements.memoryRequirements.alignment,
                    image_dedicated_requirements.prefersDedicatedAllocation ? "yes" : "no",
                    image_dedicated_requirements.requiresDedicatedAllocation ? "yes" : "no",
                    last_image_dedicated_allocation ? "yes" : "no",
                    (unsigned long long)last_image_allocation_size);
            vkDestroyImage(device, image, NULL);
            goto cleanup;
        }

        result = vkBindImageMemory(device, image, image_memory, 0);
        if (result != VK_SUCCESS) {
            snprintf(status, sizeof(status),
                    "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes drm-modifier-ext=%s path=vk-image-bind stage=bind-image result=%d reason=%s memory-type-bits=0x%x image-memory-type-bits=0x%x compatible-memory-type-bits=0x%x selected-memory-type=%d dedicated=%s alloc-size=%llu fd-size=%llu req-size=%llu status=fail",
                    loader_token,
                    gpu_token,
                    has_drm_modifier_ext ? "yes" : "no",
                    result,
                    vk_result_token(result),
                    fd_properties.memoryTypeBits,
                    image_memory_requirements.memoryRequirements.memoryTypeBits,
                    image_compatible_type_bits,
                    selected_image_memory_type_index,
                    selected_image_dedicated_allocation ? "yes" : "no",
                    (unsigned long long)selected_image_allocation_size,
                    (unsigned long long)size,
                    (unsigned long long)image_memory_requirements.memoryRequirements.size);
            vkFreeMemory(device, image_memory, NULL);
            vkDestroyImage(device, image, NULL);
            goto cleanup;
        }

        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes drm-modifier-ext=%s path=vk-image-bind vk-format=%d modifier=0x%llx tiling=%s memory-type-bits=0x%x image-memory-type-bits=0x%x compatible-memory-type-bits=0x%x selected-memory-type=%d dedicated=%s alloc-size=%llu fd-size=%llu req-size=%llu req-align=%llu dedicated-prefers=%s dedicated-requires=%s bind=pass status=pass",
                loader_token,
                gpu_token,
                has_drm_modifier_ext ? "yes" : "no",
                (int)vk_format,
                (unsigned long long)modifier,
                use_explicit_modifier ? "drm-modifier" : "linear",
                fd_properties.memoryTypeBits,
                image_memory_requirements.memoryRequirements.memoryTypeBits,
                image_compatible_type_bits,
                selected_image_memory_type_index,
                selected_image_dedicated_allocation ? "yes" : "no",
                (unsigned long long)selected_image_allocation_size,
                (unsigned long long)size,
                (unsigned long long)image_memory_requirements.memoryRequirements.size,
                (unsigned long long)image_memory_requirements.memoryRequirements.alignment,
                image_dedicated_requirements.prefersDedicatedAllocation ? "yes" : "no",
                image_dedicated_requirements.requiresDedicatedAllocation ? "yes" : "no");
        vkFreeMemory(device, image_memory, NULL);
        vkDestroyImage(device, image, NULL);
        goto cleanup;

    } else {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import loader=%s api=vkGetMemoryFdPropertiesKHR gpu=%s ext-mem=yes fd-ext=yes dma-buf-ext=yes result=%d reason=%s status=fail",
                loader_token,
                gpu_token,
                result,
                vk_result_token(result));
    }

cleanup:
    free(queue_families);
    free(extensions);
    if (device != VK_NULL_HANDLE && vkDestroyDevice != NULL) {
        vkDestroyDevice(device, NULL);
    }
    if (instance != VK_NULL_HANDLE && vkDestroyInstance != NULL) {
        vkDestroyInstance(instance, NULL);
    }
    if (vulkan != NULL) {
        dlclose(vulkan);
    }
    if (adrenotools != NULL) {
        dlclose(adrenotools);
    }
    if (loader != NULL) {
        (*env)->ReleaseStringUTFChars(env, loader_string, loader);
    }
    if (tmp_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
    }
    if (hook_lib_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
    }
    if (driver_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
    }
    if (driver_name != NULL) {
        (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
    }
    if (status[0] == '\0') {
        snprintf(status, sizeof(status),
                "probe=vulkan-dmabuf-import api=vkGetMemoryFdPropertiesKHR status=unsupported reason=unknown");
    }
    return (*env)->NewStringUTF(env, status);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeProbeAhbExport(JNIEnv *env, jclass clazz, jint socket_fd) {
    (void)clazz;

    AHardwareBuffer_Desc desc = {
            .width = 64,
            .height = 64,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
                    | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                    | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
    };
    AHardwareBuffer *buffer = NULL;
    AHardwareBuffer_Desc actual_desc;
    uint64_t buffer_id = 0;

    if (socket_fd < 0) {
        return make_status(
                env,
                "probe=ahb-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=bad-socket-fd");
    }
    if (!AHardwareBuffer_isSupported(&desc)) {
        return make_status(
                env,
                "probe=ahb-export api=AHardwareBuffer_sendHandleToUnixSocket status=unsupported reason=unsupported-desc width=%u height=%u format=%u usage=0x%llx",
                desc.width,
                desc.height,
                desc.format,
                (unsigned long long)desc.usage);
    }

    int allocate_result = AHardwareBuffer_allocate(&desc, &buffer);
    if (allocate_result != 0 || buffer == NULL) {
        return make_status(
                env,
                "probe=ahb-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=allocate result=%d",
                allocate_result);
    }

    AHardwareBuffer_describe(buffer, &actual_desc);
    (void)AHardwareBuffer_getId(buffer, &buffer_id);
    errno = 0;
    int send_result = AHardwareBuffer_sendHandleToUnixSocket(buffer, socket_fd);
    int send_errno = errno;
    AHardwareBuffer_release(buffer);

    if (send_result != 0) {
        return make_status(
                env,
                "probe=ahb-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=send-handle result=%d errno=%d width=%u height=%u format=%u layers=%u usage=0x%llx stride=%u id=%llu",
                send_result,
                send_errno,
                actual_desc.width,
                actual_desc.height,
                actual_desc.format,
                actual_desc.layers,
                (unsigned long long)actual_desc.usage,
                actual_desc.stride,
                (unsigned long long)buffer_id);
    }

    return make_status(
            env,
            "probe=ahb-export api=AHardwareBuffer_sendHandleToUnixSocket status=pass width=%u height=%u format=%u layers=%u usage=0x%llx stride=%u id=%llu",
            actual_desc.width,
            actual_desc.height,
            actual_desc.format,
            actual_desc.layers,
            (unsigned long long)actual_desc.usage,
            actual_desc.stride,
            (unsigned long long)buffer_id);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeVulkanProbe(JNIEnv *env, jclass clazz) {
    (void)clazz;

    char status[512];
    void *vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (vulkan == NULL) {
        const char *error = dlerror();
        return make_status(
                env,
                "vulkan: unavailable loader %s",
                error == NULL ? "dlopen-failed" : error);
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    if (vkGetInstanceProcAddr == NULL) {
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable missing vkGetInstanceProcAddr");
    }

    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion =
            (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
                    VK_NULL_HANDLE,
                    "vkEnumerateInstanceVersion");
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties =
            (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(
                    VK_NULL_HANDLE,
                    "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance vkCreateInstance =
            (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");

    if (vkEnumerateInstanceExtensionProperties == NULL || vkCreateInstance == NULL) {
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable missing instance symbols");
    }

    uint32_t api_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != NULL) {
        VkResult result = vkEnumerateInstanceVersion(&api_version);
        if (result != VK_SUCCESS) {
            dlclose(vulkan);
            return make_status(env, "vulkan: unavailable api-version %d", result);
        }
    }

    uint32_t instance_extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(
            NULL,
            &instance_extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable inst-ext-count %d", result);
    }

    VkExtensionProperties *instance_extensions = NULL;
    if (instance_extension_count > 0) {
        instance_extensions = (VkExtensionProperties *)calloc(
                instance_extension_count,
                sizeof(VkExtensionProperties));
        if (instance_extensions == NULL) {
            dlclose(vulkan);
            return make_status(env, "vulkan: unavailable oom inst-ext");
        }
        result = vkEnumerateInstanceExtensionProperties(
                NULL,
                &instance_extension_count,
                instance_extensions);
        if (result != VK_SUCCESS) {
            free(instance_extensions);
            dlclose(vulkan);
            return make_status(env, "vulkan: unavailable inst-ext %d", result);
        }
    }

    int has_properties2 = has_extension_name(
            instance_extensions,
            instance_extension_count,
            "VK_KHR_get_physical_device_properties2");
    free(instance_extensions);

    VkApplicationInfo app_info = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "WayLandIEDisplayProbe",
            .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
            .pEngineName = "none",
            .engineVersion = VK_MAKE_VERSION(0, 0, 0),
            .apiVersion = api_version,
    };
    VkInstanceCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&create_info, NULL, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        char api_text[24];
        format_vulkan_api_version(api_version, api_text, sizeof(api_text));
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable api %s create %d", api_text, result);
    }

    PFN_vkDestroyInstance vkDestroyInstance =
            (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
            (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
            (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumerateDeviceExtensionProperties");

    if (vkDestroyInstance == NULL
            || vkEnumeratePhysicalDevices == NULL
            || vkGetPhysicalDeviceProperties == NULL
            || vkEnumerateDeviceExtensionProperties == NULL) {
        if (vkDestroyInstance != NULL) {
            vkDestroyInstance(instance, NULL);
        }
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable missing device symbols");
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable gpu-count %d", result);
    }
    if (device_count == 0) {
        char api_text[24];
        format_vulkan_api_version(api_version, api_text, sizeof(api_text));
        vkDestroyInstance(instance, NULL);
        dlclose(vulkan);
        return make_status(
                env,
                "vulkan: api %s gpu none inst-p2 %s",
                api_text,
                has_properties2 ? "yes" : "no");
    }

    uint32_t total_device_count = device_count;
    uint32_t first_device_count = 1;
    VkPhysicalDevice first_device = VK_NULL_HANDLE;
    result = vkEnumeratePhysicalDevices(instance, &first_device_count, &first_device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || first_device == VK_NULL_HANDLE) {
        vkDestroyInstance(instance, NULL);
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable gpu-enum %d", result);
    }

    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(first_device, &device_properties);

    uint32_t device_extension_count = 0;
    result = vkEnumerateDeviceExtensionProperties(
            first_device,
            NULL,
            &device_extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        dlclose(vulkan);
        return make_status(env, "vulkan: unavailable dev-ext-count %d", result);
    }

    VkExtensionProperties *device_extensions = NULL;
    if (device_extension_count > 0) {
        device_extensions = (VkExtensionProperties *)calloc(
                device_extension_count,
                sizeof(VkExtensionProperties));
        if (device_extensions == NULL) {
            vkDestroyInstance(instance, NULL);
            dlclose(vulkan);
            return make_status(env, "vulkan: unavailable oom dev-ext");
        }
        result = vkEnumerateDeviceExtensionProperties(
                first_device,
                NULL,
                &device_extension_count,
                device_extensions);
        if (result != VK_SUCCESS) {
            free(device_extensions);
            vkDestroyInstance(instance, NULL);
            dlclose(vulkan);
            return make_status(env, "vulkan: unavailable dev-ext %d", result);
        }
    }

    int has_ahb = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_ANDROID_external_memory_android_hardware_buffer");
    int has_external_memory = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_KHR_external_memory");
    int has_external_semaphore = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_KHR_external_semaphore");
    int has_external_fence = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_KHR_external_fence");
    int has_external_memory_fd = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_KHR_external_memory_fd");
    int has_external_dma_buf = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_EXT_external_memory_dma_buf");
    int has_queue_family_foreign = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_EXT_queue_family_foreign");
    int has_drm_format_modifier = has_extension_name(
            device_extensions,
            device_extension_count,
            "VK_EXT_image_drm_format_modifier");
    free(device_extensions);

    char api_text[24];
    char gpu_name[49];
    format_vulkan_api_version(api_version, api_text, sizeof(api_text));
    snprintf(gpu_name, sizeof(gpu_name), "%s", device_properties.deviceName);
    snprintf(
            status,
            sizeof(status),
            "vulkan: api %s gpu %s devs %u inst-p2 %s ahb %s mem %s mem-fd %s dma-buf %s drm-mod %s foreign-q %s sem %s fence %s",
            api_text,
            gpu_name,
            total_device_count,
            has_properties2 ? "yes" : "no",
            has_ahb ? "yes" : "no",
            has_external_memory ? "yes" : "no",
            has_external_memory_fd ? "yes" : "no",
            has_external_dma_buf ? "yes" : "no",
            has_drm_format_modifier ? "yes" : "no",
            has_queue_family_foreign ? "yes" : "no",
            has_external_semaphore ? "yes" : "no",
            has_external_fence ? "yes" : "no");

    vkDestroyInstance(instance, NULL);
    dlclose(vulkan);
    return (*env)->NewStringUTF(env, status);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeProbeAdrenoTools(
        JNIEnv *env,
        jclass clazz,
        jstring tmp_dir_string,
        jstring hook_lib_dir_string,
        jstring driver_dir_string,
        jstring driver_name_string) {
    (void)clazz;

    enum {
        ADRENOTOOLS_DRIVER_CUSTOM_FLAG = 1 << 0,
    };
    typedef void *(*PFN_adrenotools_open_libvulkan)(
            int dlopenMode,
            int featureFlags,
            const char *tmpLibDir,
            const char *hookLibDir,
            const char *customDriverDir,
            const char *customDriverName,
            const char *fileRedirectDir,
            void **userMappingHandle);

    char status[1536] = {0};
    char adrenotools_path[512] = {0};
    char loader_driver_dir[512] = {0};
    char driver_path[512] = {0};
    const char *tmp_dir = NULL;
    const char *hook_lib_dir = NULL;
    const char *driver_dir = NULL;
    const char *driver_name = NULL;
    void *adrenotools = NULL;
    void *vulkan = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance vkDestroyInstance = NULL;

    if (tmp_dir_string == NULL
            || hook_lib_dir_string == NULL
            || driver_dir_string == NULL
            || driver_name_string == NULL) {
        return make_status(
                env,
                "probe=adrenotools-loader status=unsupported reason=bad-args");
    }

    tmp_dir = (*env)->GetStringUTFChars(env, tmp_dir_string, NULL);
    hook_lib_dir = (*env)->GetStringUTFChars(env, hook_lib_dir_string, NULL);
    driver_dir = (*env)->GetStringUTFChars(env, driver_dir_string, NULL);
    driver_name = (*env)->GetStringUTFChars(env, driver_name_string, NULL);
    if (tmp_dir == NULL || hook_lib_dir == NULL || driver_dir == NULL || driver_name == NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=unsupported reason=string-oom");
        goto cleanup;
    }
    if (driver_name[0] == '\0' || strchr(driver_name, '/') != NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=fail reason=bad-driver-name driver=%s",
                driver_name[0] == '\0' ? "empty" : driver_name);
        goto cleanup;
    }

    size_t driver_dir_len = strlen(driver_dir);
    if (driver_dir_len > 0 && driver_dir[driver_dir_len - 1] == '/') {
        snprintf(loader_driver_dir, sizeof(loader_driver_dir), "%s", driver_dir);
    } else {
        snprintf(loader_driver_dir, sizeof(loader_driver_dir), "%s/", driver_dir);
    }

    mkdir(tmp_dir, 0700);
    snprintf(driver_path, sizeof(driver_path), "%s%s", loader_driver_dir, driver_name);
    if (access(driver_path, R_OK) != 0) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=unsupported reason=driver-missing driver=%s driver-dir=%s expected=%s",
                driver_name,
                loader_driver_dir,
                driver_path);
        goto cleanup;
    }

    snprintf(adrenotools_path, sizeof(adrenotools_path), "%s/libadrenotools.so", hook_lib_dir);
    adrenotools = dlopen(adrenotools_path, RTLD_NOW | RTLD_LOCAL);
    if (adrenotools == NULL) {
        adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (adrenotools == NULL) {
        const char *error = dlerror();
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=unsupported reason=no-libadrenotools hook-dir=%s dlerror=%s",
                hook_lib_dir,
                error == NULL ? "none" : error);
        goto cleanup;
    }

    PFN_adrenotools_open_libvulkan adrenotools_open_libvulkan =
            (PFN_adrenotools_open_libvulkan)dlsym(
                    adrenotools,
                    "adrenotools_open_libvulkan");
    if (adrenotools_open_libvulkan == NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=unsupported reason=missing-open-symbol");
        goto cleanup;
    }

    vulkan = adrenotools_open_libvulkan(
            RTLD_NOW | RTLD_LOCAL,
            ADRENOTOOLS_DRIVER_CUSTOM_FLAG,
            tmp_dir,
            hook_lib_dir,
            loader_driver_dir,
            driver_name,
            NULL,
            NULL);
    if (vulkan == NULL) {
        const char *error = dlerror();
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=fail reason=open-libvulkan dlerror=%s",
                driver_name,
                error == NULL ? "none" : error);
        goto cleanup;
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr");
    if (vkGetInstanceProcAddr == NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=no-gip",
                driver_name);
        goto cleanup;
    }

    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties =
            (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(
                    VK_NULL_HANDLE,
                    "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance vkCreateInstance =
            (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (vkEnumerateInstanceExtensionProperties == NULL || vkCreateInstance == NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=missing-instance-symbol",
                driver_name);
        goto cleanup;
    }

    uint32_t instance_extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(
            NULL,
            &instance_extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=instance-ext-count result=%d",
                driver_name,
                result);
        goto cleanup;
    }
    VkExtensionProperties *instance_extensions = NULL;
    if (instance_extension_count > 0) {
        instance_extensions = (VkExtensionProperties *)calloc(
                instance_extension_count,
                sizeof(VkExtensionProperties));
        if (instance_extensions == NULL) {
            snprintf(status, sizeof(status),
                    "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=oom-instance-ext",
                    driver_name);
            goto cleanup;
        }
        result = vkEnumerateInstanceExtensionProperties(
                NULL,
                &instance_extension_count,
                instance_extensions);
        if (result != VK_SUCCESS) {
            free(instance_extensions);
            snprintf(status, sizeof(status),
                    "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=instance-ext result=%d",
                    driver_name,
                    result);
            goto cleanup;
        }
    }
    int has_android_surface = has_extension_name(
            instance_extensions,
            instance_extension_count,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    free(instance_extensions);

    VkApplicationInfo app_info = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "WayLandIEAdrenoToolsProbe",
            .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
            .pEngineName = "none",
            .engineVersion = VK_MAKE_VERSION(0, 0, 0),
            .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo instance_info = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
    };
    result = vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s android-surface=%s status=unsupported reason=create-instance result=%d",
                driver_name,
                has_android_surface ? "yes" : "no",
                result);
        goto cleanup;
    }

    vkDestroyInstance =
            (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
            (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
            (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)vkGetInstanceProcAddr(
                    instance,
                    "vkEnumerateDeviceExtensionProperties");
    if (vkDestroyInstance == NULL
            || vkEnumeratePhysicalDevices == NULL
            || vkGetPhysicalDeviceProperties == NULL
            || vkEnumerateDeviceExtensionProperties == NULL) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=missing-device-symbol",
                driver_name);
        goto cleanup;
    }

    uint32_t device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (result != VK_SUCCESS || device_count == 0) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s android-surface=%s status=unsupported reason=no-device result=%d count=%u",
                driver_name,
                has_android_surface ? "yes" : "no",
                result,
                device_count);
        goto cleanup;
    }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t one_device = 1;
    result = vkEnumeratePhysicalDevices(instance, &one_device, &physical_device);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE)
            || physical_device == VK_NULL_HANDLE) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s status=unsupported reason=enum-device result=%d",
                driver_name,
                result);
        goto cleanup;
    }

    VkPhysicalDeviceProperties device_properties;
    char gpu_token[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    memset(&device_properties, 0, sizeof(device_properties));
    vkGetPhysicalDeviceProperties(physical_device, &device_properties);
    format_bridge_token(device_properties.deviceName, gpu_token, sizeof(gpu_token));

    uint32_t device_extension_count = 0;
    result = vkEnumerateDeviceExtensionProperties(
            physical_device,
            NULL,
            &device_extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader loader=adrenotools driver=%s gpu=%s status=unsupported reason=device-ext-count result=%d",
                driver_name,
                gpu_token,
                result);
        goto cleanup;
    }

    VkExtensionProperties *device_extensions = NULL;
    if (device_extension_count > 0) {
        device_extensions = (VkExtensionProperties *)calloc(
                device_extension_count,
                sizeof(VkExtensionProperties));
        if (device_extensions == NULL) {
            snprintf(status, sizeof(status),
                    "probe=adrenotools-loader loader=adrenotools driver=%s gpu=%s status=unsupported reason=oom-device-ext",
                    driver_name,
                    gpu_token);
            goto cleanup;
        }
        result = vkEnumerateDeviceExtensionProperties(
                physical_device,
                NULL,
                &device_extension_count,
                device_extensions);
        if (result != VK_SUCCESS) {
            free(device_extensions);
            snprintf(status, sizeof(status),
                    "probe=adrenotools-loader loader=adrenotools driver=%s gpu=%s status=unsupported reason=device-ext result=%d",
                    driver_name,
                    gpu_token,
                    result);
            goto cleanup;
        }
    }

    int has_external_memory_fd = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    int has_external_dma_buf = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    int has_drm_format_modifier = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    int has_android_hardware_buffer = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    int has_external_semaphore_fd = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    int has_external_fence_fd = has_extension_name(
            device_extensions,
            device_extension_count,
            VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
    free(device_extensions);

    const char *gate_status = has_external_dma_buf
                    && has_external_memory_fd
                    && has_android_hardware_buffer
            ? "pass"
            : "unsupported";
    const char *reason = has_external_dma_buf
                    && has_external_memory_fd
                    && has_android_hardware_buffer
            ? "ready-for-dmabuf-and-ahb-import"
            : "missing-dmabuf-or-ahb-extension";
    snprintf(status, sizeof(status),
            "probe=adrenotools-loader loader=adrenotools driver=%s driver-dir=%s gpu=%s android-surface=%s ahb=%s mem-fd=%s dma-buf=%s drm-mod=%s sem-fd=%s fence-fd=%s status=%s reason=%s",
            driver_name,
            loader_driver_dir,
            gpu_token,
            has_android_surface ? "yes" : "no",
            has_android_hardware_buffer ? "yes" : "no",
            has_external_memory_fd ? "yes" : "no",
            has_external_dma_buf ? "yes" : "no",
            has_drm_format_modifier ? "yes" : "no",
            has_external_semaphore_fd ? "yes" : "no",
            has_external_fence_fd ? "yes" : "no",
            gate_status,
            reason);

cleanup:
    if (instance != VK_NULL_HANDLE && vkDestroyInstance != NULL) {
        vkDestroyInstance(instance, NULL);
    }
    if (vulkan != NULL) {
        dlclose(vulkan);
    }
    if (adrenotools != NULL) {
        dlclose(adrenotools);
    }
    if (tmp_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
    }
    if (hook_lib_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
    }
    if (driver_dir != NULL) {
        (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
    }
    if (driver_name != NULL) {
        (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
    }
    if (status[0] == '\0') {
        snprintf(status, sizeof(status),
                "probe=adrenotools-loader status=unsupported reason=unknown");
    }
    return (*env)->NewStringUTF(env, status);
}

#define AHB_CPU_RING_SIZE 8
#define AHB_CPU_FORMAT AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
#define AHB_CPU_PRIMARY_USAGE \
    ((uint64_t)AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
     (uint64_t)AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | \
     (uint64_t)AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)
#define AHB_CPU_FALLBACK_USAGE \
    ((uint64_t)AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
     (uint64_t)AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)
#define AHB_VK_PRIMARY_USAGE \
    ((uint64_t)AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | \
     (uint64_t)AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | \
     (uint64_t)AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)
#define AHB_VK_FALLBACK_USAGE \
    ((uint64_t)AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | \
     (uint64_t)AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)
#define AHB_VK_FENCE_TIMEOUT_NS 50000000ULL
#define AHB_VK_SLOT_WAIT_TIMEOUT_NS 2000000000ULL
#define AHB_VK_SCENE_VERTEX_COUNT 18
#define AHB_VK_SOURCE_CACHE_SIZE 64

typedef struct AhbCpuSlot {
    AHardwareBuffer *buffer;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t usage;
    uint64_t buffer_id;
    int in_use_by_surface_control;
    long long last_frame_index;
    long long generation;
} AhbCpuSlot;

typedef enum AhbVkSlotState {
    AHB_VK_SLOT_FREE = 0,
    AHB_VK_SLOT_RENDERING = 1,
    AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL = 2,
    AHB_VK_SLOT_FAILED = 3,
} AhbVkSlotState;

typedef struct ImportedDmaBufImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageLayout layout;
    VkFormat format;
    int width;
    int height;
} ImportedDmaBufImage;

typedef struct CachedDmaBufSource {
    int in_use;
    dev_t dev;
    ino_t ino;
    uint64_t file_size;
    int width;
    int height;
    uint32_t drm_format;
    uint64_t modifier;
    int planes;
    uint64_t stride0;
    uint64_t offset0;
    uint64_t size;
    uint64_t last_used;
    ImportedDmaBufImage image;
} CachedDmaBufSource;

typedef struct AhbVkSlot {
    AHardwareBuffer *buffer;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView image_view;
    VkFramebuffer framebuffer;
    VkCommandBuffer command_buffer;
    VkFence render_fence;
    VkImageLayout layout;
    uint64_t buffer_id;
    uint32_t format;
    uint64_t usage;
    long long last_frame_index;
    long long generation;
    AhbVkSlotState state;
    ImportedDmaBufImage *pending_source;
} AhbVkSlot;

typedef struct VulkanDispatch {
    void *library;
    void *adrenotools_library;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
    PFN_vkEnumerateInstanceVersion enumerate_instance_version;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extension_properties;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties;
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_physical_device_queue_family_properties;
    PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties;
    PFN_vkCreateDevice create_device;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkFreeCommandBuffers free_command_buffers;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkWaitForFences wait_for_fences;
    PFN_vkGetFenceFdKHR get_fence_fd;
    PFN_vkCreateRenderPass create_render_pass;
    PFN_vkDestroyRenderPass destroy_render_pass;
    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreatePipelineLayout create_pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements2 get_image_memory_requirements2;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkCreateImageView create_image_view;
    PFN_vkDestroyImageView destroy_image_view;
    PFN_vkCreateFramebuffer create_framebuffer;
    PFN_vkDestroyFramebuffer destroy_framebuffer;
    PFN_vkResetCommandBuffer reset_command_buffer;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;
    PFN_vkCmdBlitImage cmd_blit_image;
    PFN_vkCmdCopyImage cmd_copy_image;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass;
    PFN_vkCmdSetViewport cmd_set_viewport;
    PFN_vkCmdSetScissor cmd_set_scissor;
    PFN_vkCmdClearAttachments cmd_clear_attachments;
    PFN_vkCmdEndRenderPass cmd_end_render_pass;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdDraw cmd_draw;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkDeviceWaitIdle device_wait_idle;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_properties_android;
} VulkanDispatch;

typedef struct AhbVkRenderer {
    int configured;
    int width;
    int height;
    int next_slot;
    uint64_t usage;
    long long generation;
    uint32_t api_version;
    uint32_t queue_family_index;
    char tmp_dir[512];
    char hook_lib_dir[512];
    char driver_dir[512];
    char driver_name[128];
    char loader_driver_dir[512];
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceProperties device_properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkDevice device;
    VkQueue queue;
    VkCommandPool command_pool;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline graphics_pipeline;
    VkFormat image_format;
    int supports_external_fence;
    int supports_external_fence_fd;
    uint64_t source_cache_clock;
    VulkanDispatch vk;
    AhbVkSlot slots[AHB_CPU_RING_SIZE];
    CachedDmaBufSource source_cache[AHB_VK_SOURCE_CACHE_SIZE];
} AhbVkRenderer;

static pthread_mutex_t g_ahb_cpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static AhbCpuSlot g_ahb_cpu_slots[AHB_CPU_RING_SIZE];
static int g_ahb_cpu_configured;
static int g_ahb_cpu_width;
static int g_ahb_cpu_height;
static int g_ahb_cpu_next_slot;
static uint64_t g_ahb_cpu_usage;
static long long g_ahb_cpu_generation = 1;
static pthread_mutex_t g_ahb_vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ahb_vk_cond = PTHREAD_COND_INITIALIZER;
static AhbVkRenderer g_ahb_vk_renderer = {
        .generation = 1,
};

static long long elapsed_us(struct timespec start, struct timespec end) {
    long long seconds = (long long)(end.tv_sec - start.tv_sec);
    long long nanos = (long long)(end.tv_nsec - start.tv_nsec);
    return (seconds * 1000000LL) + (nanos / 1000LL);
}

static void destroy_slot_pending_source_locked(AhbVkSlot *slot);
static void destroy_source_cache_locked(void);

static struct timespec realtime_deadline_after_ns(uint64_t timeout_ns) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    deadline.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static uint8_t clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint32_t rgba_pixel(int red, int green, int blue) {
    return ((uint32_t)clamp_byte(red))
            | ((uint32_t)clamp_byte(green) << 8U)
            | ((uint32_t)clamp_byte(blue) << 16U)
            | 0xff000000U;
}

static void fill_rgba_pixels(uint32_t *pixels, int count, uint32_t color) {
    for (int i = 0; i < count; i++) {
        pixels[i] = color;
    }
}

static void fill_rgba_rect(
        uint8_t *base,
        uint32_t stride_pixels,
        int x,
        int y,
        int width,
        int height,
        uint32_t color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int row_index = 0; row_index < height; row_index++) {
        uint32_t *row = (uint32_t *)(base
                + ((size_t)(y + row_index) * (size_t)stride_pixels * 4U));
        fill_rgba_pixels(row + x, width, color);
    }
}

static void write_rgba_pattern(
        uint8_t *base,
        uint32_t stride_pixels,
        int width,
        int height,
        long long frame_index,
        long long frame_time_nanos,
        int ahb_cpu_palette) {
    int bar_x = (int)((frame_index * (ahb_cpu_palette ? 23LL : 19LL)) % width);
    int bar_width = width / (ahb_cpu_palette ? 10 : 12);
    int pulse = (int)((frame_time_nanos / 16666666LL) % 120LL);
    int checker_phase = (int)(frame_index / 8LL);
    int progress_height = height < 44 ? height : 44;
    int progress_y = height - progress_height;
    int progress = (int)((frame_index % 240LL) * width / 240LL);
    int bar_fill_width;
    uint32_t checker_even;
    uint32_t checker_odd;
    uint32_t bar_color;
    uint32_t progress_color;
    uint32_t progress_back_color = rgba_pixel(16, 24, 38);

    if (bar_width < 8) {
        bar_width = 8;
    }
    bar_fill_width = bar_width < width ? bar_width : width;

    if (ahb_cpu_palette) {
        checker_even = rgba_pixel(14, 84, 218);
        checker_odd = rgba_pixel(92, 24, 172);
        bar_color = rgba_pixel(238, 255, 46 + (pulse / 2));
        progress_color = rgba_pixel(0, 255, 92);
    } else {
        checker_even = rgba_pixel(8, 38, 76);
        checker_odd = rgba_pixel(24, 110, 190);
        bar_color = rgba_pixel(255, 110 + pulse, 24);
        progress_color = rgba_pixel(0, 226, 172);
    }

    for (int y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(base + ((size_t)y * (size_t)stride_pixels * 4U));
        int checker_y = (y / 96) + checker_phase;
        int x = 0;
        while (x < width) {
            int next_tile_x = ((x / 96) + 1) * 96;
            int run_width = next_tile_x - x;
            if (run_width > width - x) {
                run_width = width - x;
            }
            fill_rgba_pixels(
                    row + x,
                    run_width,
                    (((x / 96) + checker_y) & 1) ? checker_odd : checker_even);
            x += run_width;
        }
    }

    if (bar_x + bar_fill_width <= width) {
        fill_rgba_rect(base, stride_pixels, bar_x, 0, bar_fill_width, height, bar_color);
    } else {
        int first_width = width - bar_x;
        fill_rgba_rect(base, stride_pixels, bar_x, 0, first_width, height, bar_color);
        fill_rgba_rect(base, stride_pixels, 0, 0, bar_fill_width - first_width, height, bar_color);
    }

    if (ahb_cpu_palette) {
        uint32_t sparkle_color = rgba_pixel(255, 255, 255);
        int sparkle_offset = (int)((frame_index * 11LL) % 211LL);
        for (int y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)(base + ((size_t)y * (size_t)stride_pixels * 4U));
            int first = 211 - ((y + sparkle_offset) % 211);
            if (first == 211) {
                first = 0;
            }
            for (int x = first; x < width; x += 211) {
                int sparkle_width = width - x < 5 ? width - x : 5;
                fill_rgba_pixels(row + x, sparkle_width, sparkle_color);
            }
        }
    }

    fill_rgba_rect(base, stride_pixels, 0, progress_y, width, progress_height, progress_back_color);
    if (progress >= 0) {
        int progress_width = progress + 1;
        if (progress_width > width) {
            progress_width = width;
        }
        fill_rgba_rect(
                base,
                stride_pixels,
                0,
                progress_y,
                progress_width,
                progress_height,
                progress_color);
    }
}

static void write_pattern(
        ANativeWindow_Buffer *buffer,
        int width,
        int height,
        long long frame_index,
        long long frame_time_nanos) {
    write_rgba_pattern(
            (uint8_t *)buffer->bits,
            (uint32_t)buffer->stride,
            width,
            height,
            frame_index,
            frame_time_nanos,
            0);
}

static jobject make_ahb_cpu_frame(
        JNIEnv *env,
        jobject hardware_buffer,
        jint slot_index,
        jlong generation,
        const char *status) {
    jclass frame_class = (*env)->FindClass(
            env,
            "io/waylandie/display/MainActivity$AhbCpuFrame");
    if (frame_class == NULL) {
        return NULL;
    }

    jmethodID constructor = (*env)->GetMethodID(
            env,
            frame_class,
            "<init>",
            "(Landroid/hardware/HardwareBuffer;IJLjava/lang/String;)V");
    if (constructor == NULL) {
        (*env)->DeleteLocalRef(env, frame_class);
        return NULL;
    }

    jstring status_string = (*env)->NewStringUTF(env, status);
    if (status_string == NULL) {
        (*env)->DeleteLocalRef(env, frame_class);
        return NULL;
    }

    jobject frame = (*env)->NewObject(
            env,
            frame_class,
            constructor,
            hardware_buffer,
            slot_index,
            generation,
            status_string);
    (*env)->DeleteLocalRef(env, status_string);
    (*env)->DeleteLocalRef(env, frame_class);
    return frame;
}

static void reset_ahb_cpu_ring_locked(void) {
    for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
        if (g_ahb_cpu_slots[i].buffer != NULL) {
            AHardwareBuffer_release(g_ahb_cpu_slots[i].buffer);
        }
        g_ahb_cpu_slots[i] = (AhbCpuSlot){0};
    }

    g_ahb_cpu_configured = 0;
    g_ahb_cpu_width = 0;
    g_ahb_cpu_height = 0;
    g_ahb_cpu_next_slot = 0;
    g_ahb_cpu_usage = 0;
    g_ahb_cpu_generation++;
    if (g_ahb_cpu_generation <= 0) {
        g_ahb_cpu_generation = 1;
    }
}

static int configure_ahb_cpu_ring_locked(int width, int height, char *status, size_t status_size) {
    const uint64_t usage_options[] = {
            AHB_CPU_PRIMARY_USAGE,
            AHB_CPU_FALLBACK_USAGE,
    };
    const int usage_option_count = (int)(sizeof(usage_options) / sizeof(usage_options[0]));

    if (g_ahb_cpu_configured && g_ahb_cpu_width == width && g_ahb_cpu_height == height) {
        return 0;
    }

    reset_ahb_cpu_ring_locked();

    for (int usage_index = 0; usage_index < usage_option_count; usage_index++) {
        uint64_t usage = usage_options[usage_index];
        AHardwareBuffer_Desc desc = {
                .width = (uint32_t)width,
                .height = (uint32_t)height,
                .layers = 1,
                .format = AHB_CPU_FORMAT,
                .usage = usage,
                .stride = 0,
                .rfu0 = 0,
                .rfu1 = 0,
        };

        if (!AHardwareBuffer_isSupported(&desc)) {
            continue;
        }

        int allocation_failed = 0;
        for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
            AHardwareBuffer *buffer = NULL;
            int allocate_result = AHardwareBuffer_allocate(&desc, &buffer);
            if (allocate_result != 0 || buffer == NULL) {
                allocation_failed = allocate_result != 0 ? allocate_result : -1;
                break;
            }

            AHardwareBuffer_Desc actual_desc;
            AHardwareBuffer_describe(buffer, &actual_desc);
            g_ahb_cpu_slots[i].buffer = buffer;
            g_ahb_cpu_slots[i].width = actual_desc.width;
            g_ahb_cpu_slots[i].height = actual_desc.height;
            g_ahb_cpu_slots[i].stride = actual_desc.stride;
            g_ahb_cpu_slots[i].format = actual_desc.format;
            g_ahb_cpu_slots[i].usage = actual_desc.usage;
            g_ahb_cpu_slots[i].generation = g_ahb_cpu_generation;
            if (AHardwareBuffer_getId(buffer, &g_ahb_cpu_slots[i].buffer_id) != 0) {
                g_ahb_cpu_slots[i].buffer_id = 0;
            }
        }

        if (allocation_failed != 0) {
            for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
                if (g_ahb_cpu_slots[i].buffer != NULL) {
                    AHardwareBuffer_release(g_ahb_cpu_slots[i].buffer);
                }
                g_ahb_cpu_slots[i] = (AhbCpuSlot){0};
            }
            continue;
        }

        g_ahb_cpu_configured = 1;
        g_ahb_cpu_width = width;
        g_ahb_cpu_height = height;
        g_ahb_cpu_usage = usage;
        return 0;
    }

    snprintf(
            status,
            status_size,
            "producer: ahb-cpu fallback unsupported %dx%d usage 0x%llx/0x%llx",
            width,
            height,
            (unsigned long long)AHB_CPU_PRIMARY_USAGE,
            (unsigned long long)AHB_CPU_FALLBACK_USAGE);
    return -1;
}

static int find_free_ahb_cpu_slot_locked(void) {
    for (int attempt = 0; attempt < AHB_CPU_RING_SIZE; attempt++) {
        int slot_index = (g_ahb_cpu_next_slot + attempt) % AHB_CPU_RING_SIZE;
        if (!g_ahb_cpu_slots[slot_index].in_use_by_surface_control) {
            return slot_index;
        }
    }
    return -1;
}

static int ahb_vk_has_device_extension(
        VkPhysicalDevice physical_device,
        const char *name,
        char *status,
        size_t status_size) {
    uint32_t extension_count = 0;
    VkResult result = g_ahb_vk_renderer.vk.enumerate_device_extension_properties(
            physical_device,
            NULL,
            &extension_count,
            NULL);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error dev-ext-count %d", result);
        return 0;
    }

    VkExtensionProperties *extensions = NULL;
    if (extension_count > 0) {
        extensions = (VkExtensionProperties *)calloc(extension_count, sizeof(VkExtensionProperties));
        if (extensions == NULL) {
            snprintf(status, status_size, "producer: ahb-vk error oom dev-ext");
            return 0;
        }
        result = g_ahb_vk_renderer.vk.enumerate_device_extension_properties(
                physical_device,
                NULL,
                &extension_count,
                extensions);
        if (result != VK_SUCCESS) {
            free(extensions);
            snprintf(status, status_size, "producer: ahb-vk error dev-ext %d", result);
            return 0;
        }
    }

    int found = has_extension_name(extensions, extension_count, name);
    free(extensions);
    return found;
}

static PFN_vkVoidFunction ahb_vk_get_instance_proc(
        VkInstance instance,
        const char *name,
        char *status,
        size_t status_size) {
    PFN_vkVoidFunction function = g_ahb_vk_renderer.vk.get_instance_proc_addr(instance, name);
    if (function == NULL) {
        snprintf(status, status_size, "producer: ahb-vk error missing %s", name);
    }
    return function;
}

static PFN_vkVoidFunction ahb_vk_get_device_proc(
        VkDevice device,
        const char *name,
        char *status,
        size_t status_size) {
    PFN_vkVoidFunction function = g_ahb_vk_renderer.vk.get_device_proc_addr(device, name);
    if (function == NULL) {
        snprintf(status, status_size, "producer: ahb-vk error missing %s", name);
    }
    return function;
}

static int ahb_vk_load_global_dispatch_locked(char *status, size_t status_size) {
    VulkanDispatch *vk = &g_ahb_vk_renderer.vk;
    enum {
        ADRENOTOOLS_DRIVER_CUSTOM_FLAG = 1 << 0,
    };
    typedef void *(*PFN_adrenotools_open_libvulkan)(
            int dlopenMode,
            int featureFlags,
            const char *tmpLibDir,
            const char *hookLibDir,
            const char *customDriverDir,
            const char *customDriverName,
            const char *fileRedirectDir,
            void **userMappingHandle);

    if (g_ahb_vk_renderer.tmp_dir[0] == '\0'
            || g_ahb_vk_renderer.hook_lib_dir[0] == '\0'
            || g_ahb_vk_renderer.driver_dir[0] == '\0'
            || g_ahb_vk_renderer.driver_name[0] == '\0') {
        snprintf(status, status_size, "producer: ahb-vk unsupported missing-adrenotools-paths");
        return -1;
    }
    if (strchr(g_ahb_vk_renderer.driver_name, '/') != NULL
            || strchr(g_ahb_vk_renderer.driver_name, '\\') != NULL) {
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported bad-driver-name %s",
                g_ahb_vk_renderer.driver_name);
        return -1;
    }

    size_t driver_dir_len = strlen(g_ahb_vk_renderer.driver_dir);
    if (driver_dir_len > 0 && g_ahb_vk_renderer.driver_dir[driver_dir_len - 1] == '/') {
        snprintf(
                g_ahb_vk_renderer.loader_driver_dir,
                sizeof(g_ahb_vk_renderer.loader_driver_dir),
                "%s",
                g_ahb_vk_renderer.driver_dir);
    } else {
        snprintf(
                g_ahb_vk_renderer.loader_driver_dir,
                sizeof(g_ahb_vk_renderer.loader_driver_dir),
                "%s/",
                g_ahb_vk_renderer.driver_dir);
    }
    char driver_path[640];
    snprintf(
            driver_path,
            sizeof(driver_path),
            "%s%s",
            g_ahb_vk_renderer.loader_driver_dir,
            g_ahb_vk_renderer.driver_name);
    mkdir(g_ahb_vk_renderer.tmp_dir, 0700);
    if (access(driver_path, R_OK) != 0) {
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported driver-missing %s",
                driver_path);
        return -1;
    }

    char adrenotools_path[640];
    snprintf(
            adrenotools_path,
            sizeof(adrenotools_path),
            "%s/libadrenotools.so",
            g_ahb_vk_renderer.hook_lib_dir);
    vk->adrenotools_library = dlopen(adrenotools_path, RTLD_NOW | RTLD_LOCAL);
    if (vk->adrenotools_library == NULL) {
        vk->adrenotools_library = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (vk->adrenotools_library == NULL) {
        const char *error = dlerror();
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported no-libadrenotools %s",
                error == NULL ? "dlopen-failed" : error);
        return -1;
    }
    PFN_adrenotools_open_libvulkan adrenotools_open_libvulkan =
            (PFN_adrenotools_open_libvulkan)dlsym(
                    vk->adrenotools_library,
                    "adrenotools_open_libvulkan");
    if (adrenotools_open_libvulkan == NULL) {
        snprintf(status, status_size, "producer: ahb-vk unsupported missing-adrenotools-open");
        return -1;
    }

    vk->library = adrenotools_open_libvulkan(
            RTLD_NOW | RTLD_LOCAL,
            ADRENOTOOLS_DRIVER_CUSTOM_FLAG,
            g_ahb_vk_renderer.tmp_dir,
            g_ahb_vk_renderer.hook_lib_dir,
            g_ahb_vk_renderer.loader_driver_dir,
            g_ahb_vk_renderer.driver_name,
            NULL,
            NULL);
    if (vk->library == NULL) {
        const char *error = dlerror();
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported adrenotools-loader %s",
                error == NULL ? "dlopen-failed" : error);
        return -1;
    }

    vk->get_instance_proc_addr =
            (PFN_vkGetInstanceProcAddr)dlsym(vk->library, "vkGetInstanceProcAddr");
    if (vk->get_instance_proc_addr == NULL) {
        snprintf(status, status_size, "producer: ahb-vk unsupported missing gip");
        return -1;
    }

    vk->enumerate_instance_version =
            (PFN_vkEnumerateInstanceVersion)vk->get_instance_proc_addr(
                    VK_NULL_HANDLE,
                    "vkEnumerateInstanceVersion");
    vk->enumerate_instance_extension_properties =
            (PFN_vkEnumerateInstanceExtensionProperties)vk->get_instance_proc_addr(
                    VK_NULL_HANDLE,
                    "vkEnumerateInstanceExtensionProperties");
    vk->create_instance =
            (PFN_vkCreateInstance)vk->get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance");
    if (vk->enumerate_instance_extension_properties == NULL || vk->create_instance == NULL) {
        snprintf(status, status_size, "producer: ahb-vk unsupported missing instance symbols");
        return -1;
    }
    return 0;
}

static int ahb_vk_load_instance_dispatch_locked(char *status, size_t status_size) {
    VulkanDispatch *vk = &g_ahb_vk_renderer.vk;
    VkInstance instance = g_ahb_vk_renderer.instance;

    vk->destroy_instance = (PFN_vkDestroyInstance)ahb_vk_get_instance_proc(
            instance,
            "vkDestroyInstance",
            status,
            status_size);
    vk->enumerate_physical_devices = (PFN_vkEnumeratePhysicalDevices)ahb_vk_get_instance_proc(
            instance,
            "vkEnumeratePhysicalDevices",
            status,
            status_size);
    vk->get_physical_device_properties = (PFN_vkGetPhysicalDeviceProperties)ahb_vk_get_instance_proc(
            instance,
            "vkGetPhysicalDeviceProperties",
            status,
            status_size);
    vk->get_physical_device_memory_properties =
            (PFN_vkGetPhysicalDeviceMemoryProperties)ahb_vk_get_instance_proc(
                    instance,
                    "vkGetPhysicalDeviceMemoryProperties",
                    status,
                    status_size);
    vk->get_physical_device_queue_family_properties =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)ahb_vk_get_instance_proc(
                    instance,
                    "vkGetPhysicalDeviceQueueFamilyProperties",
                    status,
                    status_size);
    vk->enumerate_device_extension_properties =
            (PFN_vkEnumerateDeviceExtensionProperties)ahb_vk_get_instance_proc(
                    instance,
                    "vkEnumerateDeviceExtensionProperties",
                    status,
                    status_size);
    vk->create_device = (PFN_vkCreateDevice)ahb_vk_get_instance_proc(
            instance,
            "vkCreateDevice",
            status,
            status_size);
    vk->get_device_proc_addr = (PFN_vkGetDeviceProcAddr)ahb_vk_get_instance_proc(
            instance,
            "vkGetDeviceProcAddr",
            status,
            status_size);

    return vk->destroy_instance != NULL
            && vk->enumerate_physical_devices != NULL
            && vk->get_physical_device_properties != NULL
            && vk->get_physical_device_memory_properties != NULL
            && vk->get_physical_device_queue_family_properties != NULL
            && vk->enumerate_device_extension_properties != NULL
            && vk->create_device != NULL
            && vk->get_device_proc_addr != NULL
            ? 0
            : -1;
}

static int ahb_vk_load_device_dispatch_locked(char *status, size_t status_size) {
    VulkanDispatch *vk = &g_ahb_vk_renderer.vk;
    VkDevice device = g_ahb_vk_renderer.device;

    vk->destroy_device = (PFN_vkDestroyDevice)ahb_vk_get_device_proc(
            device, "vkDestroyDevice", status, status_size);
    vk->get_device_queue = (PFN_vkGetDeviceQueue)ahb_vk_get_device_proc(
            device, "vkGetDeviceQueue", status, status_size);
    vk->create_command_pool = (PFN_vkCreateCommandPool)ahb_vk_get_device_proc(
            device, "vkCreateCommandPool", status, status_size);
    vk->destroy_command_pool = (PFN_vkDestroyCommandPool)ahb_vk_get_device_proc(
            device, "vkDestroyCommandPool", status, status_size);
    vk->allocate_command_buffers = (PFN_vkAllocateCommandBuffers)ahb_vk_get_device_proc(
            device, "vkAllocateCommandBuffers", status, status_size);
    vk->free_command_buffers = (PFN_vkFreeCommandBuffers)ahb_vk_get_device_proc(
            device, "vkFreeCommandBuffers", status, status_size);
    vk->create_fence = (PFN_vkCreateFence)ahb_vk_get_device_proc(
            device, "vkCreateFence", status, status_size);
    vk->destroy_fence = (PFN_vkDestroyFence)ahb_vk_get_device_proc(
            device, "vkDestroyFence", status, status_size);
    vk->reset_fences = (PFN_vkResetFences)ahb_vk_get_device_proc(
            device, "vkResetFences", status, status_size);
    vk->wait_for_fences = (PFN_vkWaitForFences)ahb_vk_get_device_proc(
            device, "vkWaitForFences", status, status_size);
    vk->get_fence_fd = (PFN_vkGetFenceFdKHR)ahb_vk_get_device_proc(
            device, "vkGetFenceFdKHR", status, status_size);
    vk->create_render_pass = (PFN_vkCreateRenderPass)ahb_vk_get_device_proc(
            device, "vkCreateRenderPass", status, status_size);
    vk->destroy_render_pass = (PFN_vkDestroyRenderPass)ahb_vk_get_device_proc(
            device, "vkDestroyRenderPass", status, status_size);
    vk->create_shader_module = (PFN_vkCreateShaderModule)ahb_vk_get_device_proc(
            device, "vkCreateShaderModule", status, status_size);
    vk->destroy_shader_module = (PFN_vkDestroyShaderModule)ahb_vk_get_device_proc(
            device, "vkDestroyShaderModule", status, status_size);
    vk->create_pipeline_layout = (PFN_vkCreatePipelineLayout)ahb_vk_get_device_proc(
            device, "vkCreatePipelineLayout", status, status_size);
    vk->destroy_pipeline_layout = (PFN_vkDestroyPipelineLayout)ahb_vk_get_device_proc(
            device, "vkDestroyPipelineLayout", status, status_size);
    vk->create_graphics_pipelines = (PFN_vkCreateGraphicsPipelines)ahb_vk_get_device_proc(
            device, "vkCreateGraphicsPipelines", status, status_size);
    vk->destroy_pipeline = (PFN_vkDestroyPipeline)ahb_vk_get_device_proc(
            device, "vkDestroyPipeline", status, status_size);
    vk->create_image = (PFN_vkCreateImage)ahb_vk_get_device_proc(
            device, "vkCreateImage", status, status_size);
    vk->destroy_image = (PFN_vkDestroyImage)ahb_vk_get_device_proc(
            device, "vkDestroyImage", status, status_size);
    vk->get_image_memory_requirements2 = (PFN_vkGetImageMemoryRequirements2)ahb_vk_get_device_proc(
            device, "vkGetImageMemoryRequirements2", status, status_size);
    vk->allocate_memory = (PFN_vkAllocateMemory)ahb_vk_get_device_proc(
            device, "vkAllocateMemory", status, status_size);
    vk->free_memory = (PFN_vkFreeMemory)ahb_vk_get_device_proc(
            device, "vkFreeMemory", status, status_size);
    vk->bind_image_memory = (PFN_vkBindImageMemory)ahb_vk_get_device_proc(
            device, "vkBindImageMemory", status, status_size);
    vk->get_memory_fd_properties =
            (PFN_vkGetMemoryFdPropertiesKHR)ahb_vk_get_device_proc(
                    device,
                    "vkGetMemoryFdPropertiesKHR",
                    status,
                    status_size);
    vk->create_image_view = (PFN_vkCreateImageView)ahb_vk_get_device_proc(
            device, "vkCreateImageView", status, status_size);
    vk->destroy_image_view = (PFN_vkDestroyImageView)ahb_vk_get_device_proc(
            device, "vkDestroyImageView", status, status_size);
    vk->create_framebuffer = (PFN_vkCreateFramebuffer)ahb_vk_get_device_proc(
            device, "vkCreateFramebuffer", status, status_size);
    vk->destroy_framebuffer = (PFN_vkDestroyFramebuffer)ahb_vk_get_device_proc(
            device, "vkDestroyFramebuffer", status, status_size);
    vk->reset_command_buffer = (PFN_vkResetCommandBuffer)ahb_vk_get_device_proc(
            device, "vkResetCommandBuffer", status, status_size);
    vk->begin_command_buffer = (PFN_vkBeginCommandBuffer)ahb_vk_get_device_proc(
            device, "vkBeginCommandBuffer", status, status_size);
    vk->end_command_buffer = (PFN_vkEndCommandBuffer)ahb_vk_get_device_proc(
            device, "vkEndCommandBuffer", status, status_size);
    vk->cmd_pipeline_barrier = (PFN_vkCmdPipelineBarrier)ahb_vk_get_device_proc(
            device, "vkCmdPipelineBarrier", status, status_size);
    vk->cmd_blit_image = (PFN_vkCmdBlitImage)ahb_vk_get_device_proc(
            device, "vkCmdBlitImage", status, status_size);
    vk->cmd_copy_image = (PFN_vkCmdCopyImage)ahb_vk_get_device_proc(
            device, "vkCmdCopyImage", status, status_size);
    vk->cmd_begin_render_pass = (PFN_vkCmdBeginRenderPass)ahb_vk_get_device_proc(
            device, "vkCmdBeginRenderPass", status, status_size);
    vk->cmd_clear_attachments = (PFN_vkCmdClearAttachments)ahb_vk_get_device_proc(
            device, "vkCmdClearAttachments", status, status_size);
    vk->cmd_end_render_pass = (PFN_vkCmdEndRenderPass)ahb_vk_get_device_proc(
            device, "vkCmdEndRenderPass", status, status_size);
    vk->cmd_bind_pipeline = (PFN_vkCmdBindPipeline)ahb_vk_get_device_proc(
            device, "vkCmdBindPipeline", status, status_size);
    vk->cmd_push_constants = (PFN_vkCmdPushConstants)ahb_vk_get_device_proc(
            device, "vkCmdPushConstants", status, status_size);
    vk->cmd_draw = (PFN_vkCmdDraw)ahb_vk_get_device_proc(
            device, "vkCmdDraw", status, status_size);
    vk->queue_submit = (PFN_vkQueueSubmit)ahb_vk_get_device_proc(
            device, "vkQueueSubmit", status, status_size);
    vk->device_wait_idle = (PFN_vkDeviceWaitIdle)ahb_vk_get_device_proc(
            device, "vkDeviceWaitIdle", status, status_size);
    vk->get_ahb_properties_android =
            (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)ahb_vk_get_device_proc(
                    device,
                    "vkGetAndroidHardwareBufferPropertiesANDROID",
                    status,
                    status_size);

    return vk->destroy_device != NULL
            && vk->get_device_queue != NULL
            && vk->create_command_pool != NULL
            && vk->destroy_command_pool != NULL
            && vk->allocate_command_buffers != NULL
            && vk->free_command_buffers != NULL
            && vk->create_fence != NULL
            && vk->destroy_fence != NULL
            && vk->reset_fences != NULL
            && vk->wait_for_fences != NULL
            && vk->create_render_pass != NULL
            && vk->destroy_render_pass != NULL
            && vk->create_shader_module != NULL
            && vk->destroy_shader_module != NULL
            && vk->create_pipeline_layout != NULL
            && vk->destroy_pipeline_layout != NULL
            && vk->create_graphics_pipelines != NULL
            && vk->destroy_pipeline != NULL
            && vk->create_image != NULL
            && vk->destroy_image != NULL
            && vk->get_image_memory_requirements2 != NULL
            && vk->allocate_memory != NULL
            && vk->free_memory != NULL
            && vk->bind_image_memory != NULL
            && vk->get_memory_fd_properties != NULL
            && vk->create_image_view != NULL
            && vk->destroy_image_view != NULL
            && vk->create_framebuffer != NULL
            && vk->destroy_framebuffer != NULL
            && vk->reset_command_buffer != NULL
            && vk->begin_command_buffer != NULL
            && vk->end_command_buffer != NULL
            && vk->cmd_pipeline_barrier != NULL
            && vk->cmd_blit_image != NULL
            && vk->cmd_copy_image != NULL
            && vk->cmd_begin_render_pass != NULL
            && vk->cmd_clear_attachments != NULL
            && vk->cmd_end_render_pass != NULL
            && vk->cmd_bind_pipeline != NULL
            && vk->cmd_push_constants != NULL
            && vk->cmd_draw != NULL
            && vk->queue_submit != NULL
            && vk->device_wait_idle != NULL
            && vk->get_ahb_properties_android != NULL
            ? 0
            : -1;
}

static void reset_ahb_vk_renderer_locked(void) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    long long next_generation = renderer->generation + 1;
    if (next_generation <= 0) {
        next_generation = 1;
    }

    if (renderer->device != VK_NULL_HANDLE && renderer->vk.device_wait_idle != NULL) {
        renderer->vk.device_wait_idle(renderer->device);
    }

    for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
        AhbVkSlot *slot = &renderer->slots[i];
        if (renderer->device != VK_NULL_HANDLE) {
            destroy_slot_pending_source_locked(slot);
            if (slot->framebuffer != VK_NULL_HANDLE && renderer->vk.destroy_framebuffer != NULL) {
                renderer->vk.destroy_framebuffer(renderer->device, slot->framebuffer, NULL);
            }
            if (slot->image_view != VK_NULL_HANDLE && renderer->vk.destroy_image_view != NULL) {
                renderer->vk.destroy_image_view(renderer->device, slot->image_view, NULL);
            }
            if (slot->memory != VK_NULL_HANDLE && renderer->vk.free_memory != NULL) {
                renderer->vk.free_memory(renderer->device, slot->memory, NULL);
            }
            if (slot->image != VK_NULL_HANDLE && renderer->vk.destroy_image != NULL) {
                renderer->vk.destroy_image(renderer->device, slot->image, NULL);
            }
            if (slot->render_fence != VK_NULL_HANDLE && renderer->vk.destroy_fence != NULL) {
                renderer->vk.destroy_fence(renderer->device, slot->render_fence, NULL);
            }
            if (slot->command_buffer != VK_NULL_HANDLE && renderer->vk.free_command_buffers != NULL
                    && renderer->command_pool != VK_NULL_HANDLE) {
                renderer->vk.free_command_buffers(
                        renderer->device,
                        renderer->command_pool,
                        1,
                        &slot->command_buffer);
            }
        }
        if (slot->buffer != NULL) {
            AHardwareBuffer_release(slot->buffer);
        }
        *slot = (AhbVkSlot){0};
    }
    if (renderer->device != VK_NULL_HANDLE) {
        destroy_source_cache_locked();
    }

    if (renderer->device != VK_NULL_HANDLE) {
        if (renderer->graphics_pipeline != VK_NULL_HANDLE && renderer->vk.destroy_pipeline != NULL) {
            renderer->vk.destroy_pipeline(renderer->device, renderer->graphics_pipeline, NULL);
        }
        if (renderer->pipeline_layout != VK_NULL_HANDLE && renderer->vk.destroy_pipeline_layout != NULL) {
            renderer->vk.destroy_pipeline_layout(renderer->device, renderer->pipeline_layout, NULL);
        }
        if (renderer->render_pass != VK_NULL_HANDLE && renderer->vk.destroy_render_pass != NULL) {
            renderer->vk.destroy_render_pass(renderer->device, renderer->render_pass, NULL);
        }
        if (renderer->command_pool != VK_NULL_HANDLE && renderer->vk.destroy_command_pool != NULL) {
            renderer->vk.destroy_command_pool(renderer->device, renderer->command_pool, NULL);
        }
        if (renderer->vk.destroy_device != NULL) {
            renderer->vk.destroy_device(renderer->device, NULL);
        }
    }
    if (renderer->instance != VK_NULL_HANDLE && renderer->vk.destroy_instance != NULL) {
        renderer->vk.destroy_instance(renderer->instance, NULL);
    }
    if (renderer->vk.library != NULL) {
        dlclose(renderer->vk.library);
    }
    if (renderer->vk.adrenotools_library != NULL) {
        dlclose(renderer->vk.adrenotools_library);
    }

    *renderer = (AhbVkRenderer){0};
    renderer->generation = next_generation;
    pthread_cond_broadcast(&g_ahb_vk_cond);
}

static int ahb_vk_select_memory_type_locked(uint32_t type_bits) {
    int fallback_index = -1;
    for (uint32_t i = 0; i < g_ahb_vk_renderer.memory_properties.memoryTypeCount; i++) {
        if ((type_bits & (1U << i)) == 0) {
            continue;
        }
        if (fallback_index < 0) {
            fallback_index = (int)i;
        }
        if ((g_ahb_vk_renderer.memory_properties.memoryTypes[i].propertyFlags
                    & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            return (int)i;
        }
    }
    return fallback_index;
}

static int ahb_vk_create_render_pass_locked(char *status, size_t status_size) {
    VkAttachmentDescription attachment = {
            .flags = 0,
            .format = g_ahb_vk_renderer.image_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkAttachmentReference color_ref = {
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_ref,
    };
    VkRenderPassCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .subpassCount = 1,
            .pSubpasses = &subpass,
    };
    VkResult result = g_ahb_vk_renderer.vk.create_render_pass(
            g_ahb_vk_renderer.device,
            &create_info,
            NULL,
            &g_ahb_vk_renderer.render_pass);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error render-pass %d", result);
        return -1;
    }
    return 0;
}

typedef struct AhbVk3dPushConstants {
    float phase;
    float aspect;
} AhbVk3dPushConstants;

static int ahb_vk_create_shader_module_locked(
        const uint32_t *code,
        size_t code_size,
        VkShaderModule *shader_module,
        const char *label,
        char *status,
        size_t status_size) {
    VkShaderModuleCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code_size,
            .pCode = code,
    };
    VkResult result = g_ahb_vk_renderer.vk.create_shader_module(
            g_ahb_vk_renderer.device,
            &create_info,
            NULL,
            shader_module);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error %s-shader %d", label, result);
        return -1;
    }
    return 0;
}

static int ahb_vk_create_3d_pipeline_locked(char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;

    if (ahb_vk_create_shader_module_locked(
                AHB_VK_3D_VERT_SPV,
                AHB_VK_3D_VERT_SPV_SIZE,
                &vertex_shader,
                "vertex",
                status,
                status_size) != 0) {
        return -1;
    }
    if (ahb_vk_create_shader_module_locked(
                AHB_VK_3D_FRAG_SPV,
                AHB_VK_3D_FRAG_SPV_SIZE,
                &fragment_shader,
                "fragment",
                status,
                status_size) != 0) {
        renderer->vk.destroy_shader_module(renderer->device, vertex_shader, NULL);
        return -1;
    }

    VkPushConstantRange push_constant_range = {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(AhbVk3dPushConstants),
    };
    VkPipelineLayoutCreateInfo layout_create = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant_range,
    };
    VkResult result = renderer->vk.create_pipeline_layout(
            renderer->device,
            &layout_create,
            NULL,
            &renderer->pipeline_layout);
    if (result != VK_SUCCESS) {
        renderer->vk.destroy_shader_module(renderer->device, fragment_shader, NULL);
        renderer->vk.destroy_shader_module(renderer->device, vertex_shader, NULL);
        snprintf(status, status_size, "producer: ahb-vk error pipeline-layout %d", result);
        return -1;
    }

    VkPipelineShaderStageCreateInfo shader_stages[2] = {
            {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertex_shader,
                    .pName = "main",
            },
            {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragment_shader,
                    .pName = "main",
            },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = (float)renderer->width,
            .height = (float)renderer->height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
            .offset = { .x = 0, .y = 0 },
            .extent = { .width = (uint32_t)renderer->width, .height = (uint32_t)renderer->height },
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rasterization = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState color_blend_attachment = {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                    | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT
                    | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blend = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &color_blend_attachment,
    };
    VkGraphicsPipelineCreateInfo pipeline_create = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = shader_stages,
            .pVertexInputState = &vertex_input,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_state,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &color_blend,
            .layout = renderer->pipeline_layout,
            .renderPass = renderer->render_pass,
            .subpass = 0,
    };

    result = renderer->vk.create_graphics_pipelines(
            renderer->device,
            VK_NULL_HANDLE,
            1,
            &pipeline_create,
            NULL,
            &renderer->graphics_pipeline);
    renderer->vk.destroy_shader_module(renderer->device, fragment_shader, NULL);
    renderer->vk.destroy_shader_module(renderer->device, vertex_shader, NULL);
    if (result != VK_SUCCESS) {
        renderer->vk.destroy_pipeline_layout(renderer->device, renderer->pipeline_layout, NULL);
        renderer->pipeline_layout = VK_NULL_HANDLE;
        snprintf(status, status_size, "producer: ahb-vk error graphics-pipeline %d", result);
        return -1;
    }
    return 0;
}

static int ahb_vk_import_slot_locked(int slot_index, char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    AhbVkSlot *slot = &renderer->slots[slot_index];

    AHardwareBuffer_Desc actual_desc;
    AHardwareBuffer_describe(slot->buffer, &actual_desc);
    slot->format = actual_desc.format;
    slot->usage = actual_desc.usage;
    slot->generation = renderer->generation;
    slot->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot->state = AHB_VK_SLOT_FREE;
    if (AHardwareBuffer_getId(slot->buffer, &slot->buffer_id) != 0) {
        slot->buffer_id = 0;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID format_properties = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID ahb_properties = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
            .pNext = &format_properties,
    };
    VkResult result = renderer->vk.get_ahb_properties_android(
            renderer->device,
            slot->buffer,
            &ahb_properties);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error ahb-props %d slot %d", result, slot_index);
        return -1;
    }
    if (format_properties.format == VK_FORMAT_UNDEFINED) {
        snprintf(status, status_size, "producer: ahb-vk unsupported undefined-format slot %d", slot_index);
        return -1;
    }
    if ((format_properties.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported fmt-features 0x%x slot %d",
                format_properties.formatFeatures,
                slot_index);
        return -1;
    }
    if (renderer->image_format == VK_FORMAT_UNDEFINED) {
        renderer->image_format = format_properties.format;
    } else if (renderer->image_format != format_properties.format) {
        snprintf(status, status_size, "producer: ahb-vk unsupported mixed-format slot %d", slot_index);
        return -1;
    }

    VkExternalMemoryImageCreateInfo external_image = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo image_create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &external_image,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format_properties.format,
            .extent = {
                    .width = actual_desc.width,
                    .height = actual_desc.height,
                    .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    result = renderer->vk.create_image(renderer->device, &image_create, NULL, &slot->image);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error create-image %d slot %d", result, slot_index);
        return -1;
    }

    VkMemoryDedicatedRequirements dedicated_requirements = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 memory_requirements = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = slot->image,
    };
    renderer->vk.get_image_memory_requirements2(
            renderer->device,
            &requirements_info,
            &memory_requirements);

    uint32_t type_bits = ahb_properties.memoryTypeBits
            & memory_requirements.memoryRequirements.memoryTypeBits;
    int memory_type_index = ahb_vk_select_memory_type_locked(type_bits);
    if (memory_type_index < 0) {
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported memory-types ahb 0x%x req 0x%x",
                ahb_properties.memoryTypeBits,
                memory_requirements.memoryRequirements.memoryTypeBits);
        return -1;
    }

    VkDeviceSize allocation_size = ahb_properties.allocationSize;
    if (allocation_size < memory_requirements.memoryRequirements.size) {
        allocation_size = memory_requirements.memoryRequirements.size;
    }
    VkMemoryDedicatedAllocateInfo dedicated_allocate = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = slot->image,
            .buffer = VK_NULL_HANDLE,
    };
    VkImportAndroidHardwareBufferInfoANDROID import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .pNext = &dedicated_allocate,
            .buffer = slot->buffer,
    };
    VkMemoryAllocateInfo allocate_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = allocation_size,
            .memoryTypeIndex = (uint32_t)memory_type_index,
    };
    result = renderer->vk.allocate_memory(renderer->device, &allocate_info, NULL, &slot->memory);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error alloc-memory %d slot %d", result, slot_index);
        return -1;
    }
    result = renderer->vk.bind_image_memory(renderer->device, slot->image, slot->memory, 0);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error bind-memory %d slot %d", result, slot_index);
        return -1;
    }

    VkImageViewCreateInfo view_create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = slot->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_properties.format,
            .components = {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },
    };
    result = renderer->vk.create_image_view(renderer->device, &view_create, NULL, &slot->image_view);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error image-view %d slot %d", result, slot_index);
        return -1;
    }

    VkCommandBufferAllocateInfo command_allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = renderer->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
    };
    result = renderer->vk.allocate_command_buffers(
            renderer->device,
            &command_allocate,
            &slot->command_buffer);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error cmd-alloc %d slot %d", result, slot_index);
        return -1;
    }

    VkExportFenceCreateInfo export_fence_create = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkFenceCreateInfo fence_create = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = renderer->supports_external_fence && renderer->supports_external_fence_fd
                    ? &export_fence_create
                    : NULL,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    result = renderer->vk.create_fence(renderer->device, &fence_create, NULL, &slot->render_fence);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error fence %d slot %d", result, slot_index);
        return -1;
    }
    return 0;
}

static int ahb_vk_create_framebuffers_locked(char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
        VkFramebufferCreateInfo framebuffer_create = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = renderer->render_pass,
                .attachmentCount = 1,
                .pAttachments = &renderer->slots[i].image_view,
                .width = (uint32_t)renderer->width,
                .height = (uint32_t)renderer->height,
                .layers = 1,
        };
        VkResult result = renderer->vk.create_framebuffer(
                renderer->device,
                &framebuffer_create,
                NULL,
                &renderer->slots[i].framebuffer);
        if (result != VK_SUCCESS) {
            snprintf(status, status_size, "producer: ahb-vk error framebuffer %d slot %d", result, i);
            return -1;
        }
    }
    return 0;
}

static int ahb_vk_choose_physical_device_locked(char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    uint32_t device_count = 0;
    VkResult result = renderer->vk.enumerate_physical_devices(
            renderer->instance,
            &device_count,
            NULL);
    if (result != VK_SUCCESS || device_count == 0) {
        snprintf(status, status_size, "producer: ahb-vk unsupported gpu-count %d/%u", result, device_count);
        return -1;
    }

    VkPhysicalDevice *devices = (VkPhysicalDevice *)calloc(device_count, sizeof(VkPhysicalDevice));
    if (devices == NULL) {
        snprintf(status, status_size, "producer: ahb-vk error oom gpus");
        return -1;
    }
    result = renderer->vk.enumerate_physical_devices(renderer->instance, &device_count, devices);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        free(devices);
        snprintf(status, status_size, "producer: ahb-vk error gpu-enum %d", result);
        return -1;
    }

    int selected = 0;
    for (uint32_t i = 0; i < device_count; i++) {
        if (!ahb_vk_has_device_extension(
                    devices[i],
                    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                    status,
                    status_size)) {
            continue;
        }

        uint32_t queue_count = 0;
        renderer->vk.get_physical_device_queue_family_properties(devices[i], &queue_count, NULL);
        VkQueueFamilyProperties *queues = NULL;
        if (queue_count > 0) {
            queues = (VkQueueFamilyProperties *)calloc(queue_count, sizeof(VkQueueFamilyProperties));
        }
        if (queues == NULL) {
            continue;
        }
        renderer->vk.get_physical_device_queue_family_properties(devices[i], &queue_count, queues);
        for (uint32_t q = 0; q < queue_count; q++) {
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                renderer->physical_device = devices[i];
                renderer->queue_family_index = q;
                selected = 1;
                break;
            }
        }
        free(queues);
        if (selected) {
            break;
        }
    }
    free(devices);

    if (!selected) {
        snprintf(status, status_size, "producer: ahb-vk unsupported no-gfx-ahb-device");
        return -1;
    }

    renderer->vk.get_physical_device_properties(
            renderer->physical_device,
            &renderer->device_properties);
    renderer->vk.get_physical_device_memory_properties(
            renderer->physical_device,
            &renderer->memory_properties);
    return 0;
}

static int ahb_vk_create_device_locked(char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    const char *candidate_extensions[] = {
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
            VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    };
    const char *enabled_extensions[10];
    uint32_t enabled_extension_count = 0;
    int supports_external_fence = 0;
    int supports_external_fence_fd = 0;

    for (uint32_t i = 0; i < (uint32_t)(sizeof(candidate_extensions) / sizeof(candidate_extensions[0])); i++) {
        if (ahb_vk_has_device_extension(
                    renderer->physical_device,
                    candidate_extensions[i],
                    status,
                    status_size)) {
            enabled_extensions[enabled_extension_count++] = candidate_extensions[i];
            if (strcmp(candidate_extensions[i], VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME) == 0) {
                supports_external_fence = 1;
            } else if (strcmp(candidate_extensions[i], VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME) == 0) {
                supports_external_fence_fd = 1;
            }
        } else if (strcmp(
                            candidate_extensions[i],
                            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)
                == 0) {
            snprintf(status, status_size, "producer: ahb-vk unsupported missing ahb-ext");
            return -1;
        }
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = renderer->queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
    };
    VkDeviceCreateInfo device_create = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_create,
            .enabledExtensionCount = enabled_extension_count,
            .ppEnabledExtensionNames = enabled_extensions,
    };
    VkResult result = renderer->vk.create_device(
            renderer->physical_device,
            &device_create,
            NULL,
            &renderer->device);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error create-device %d", result);
        return -1;
    }
    renderer->supports_external_fence = supports_external_fence;
    renderer->supports_external_fence_fd = supports_external_fence_fd;
    return 0;
}

static int ahb_vk_create_core_locked(char *status, size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    if (ahb_vk_load_global_dispatch_locked(status, status_size) != 0) {
        return -1;
    }

    renderer->api_version = VK_API_VERSION_1_0;
    if (renderer->vk.enumerate_instance_version != NULL) {
        VkResult result = renderer->vk.enumerate_instance_version(&renderer->api_version);
        if (result != VK_SUCCESS) {
            snprintf(status, status_size, "producer: ahb-vk unsupported api-version %d", result);
            return -1;
        }
    }
    if (renderer->api_version < VK_API_VERSION_1_1) {
        snprintf(status, status_size, "producer: ahb-vk unsupported api < 1.1");
        return -1;
    }

    VkApplicationInfo app_info = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "WayLandIEDisplayAhbVk",
            .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
            .pEngineName = "none",
            .engineVersion = VK_MAKE_VERSION(0, 0, 0),
            .apiVersion = renderer->api_version,
    };
    VkInstanceCreateInfo instance_create = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info,
    };
    VkResult result = renderer->vk.create_instance(&instance_create, NULL, &renderer->instance);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error create-instance %d", result);
        return -1;
    }
    if (ahb_vk_load_instance_dispatch_locked(status, status_size) != 0) {
        return -1;
    }
    if (ahb_vk_choose_physical_device_locked(status, status_size) != 0) {
        return -1;
    }
    if (ahb_vk_create_device_locked(status, status_size) != 0) {
        return -1;
    }
    if (ahb_vk_load_device_dispatch_locked(status, status_size) != 0) {
        return -1;
    }

    renderer->vk.get_device_queue(
            renderer->device,
            renderer->queue_family_index,
            0,
            &renderer->queue);
    if (renderer->queue == VK_NULL_HANDLE) {
        snprintf(status, status_size, "producer: ahb-vk error missing queue");
        return -1;
    }

    VkCommandPoolCreateInfo pool_create = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = renderer->queue_family_index,
    };
    result = renderer->vk.create_command_pool(
            renderer->device,
            &pool_create,
            NULL,
            &renderer->command_pool);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error command-pool %d", result);
        return -1;
    }
    return 0;
}

static int configure_ahb_vk_ring_locked(
        int width,
        int height,
        const char *tmp_dir,
        const char *hook_lib_dir,
        const char *driver_dir,
        const char *driver_name,
        char *status,
        size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    const uint64_t usage_options[] = {
            AHB_VK_PRIMARY_USAGE,
            AHB_VK_FALLBACK_USAGE,
    };
    const int usage_option_count = (int)(sizeof(usage_options) / sizeof(usage_options[0]));
    char first_failure[256] = "";

    if (tmp_dir == NULL
            || hook_lib_dir == NULL
            || driver_dir == NULL
            || driver_name == NULL
            || tmp_dir[0] == '\0'
            || hook_lib_dir[0] == '\0'
            || driver_dir[0] == '\0'
            || driver_name[0] == '\0') {
        snprintf(status, status_size, "producer: ahb-vk unsupported missing-loader-args");
        return -1;
    }

    if (renderer->configured
            && renderer->width == width
            && renderer->height == height
            && strcmp(renderer->tmp_dir, tmp_dir) == 0
            && strcmp(renderer->hook_lib_dir, hook_lib_dir) == 0
            && strcmp(renderer->driver_dir, driver_dir) == 0
            && strcmp(renderer->driver_name, driver_name) == 0) {
        return 0;
    }

    reset_ahb_vk_renderer_locked();
    snprintf(renderer->tmp_dir, sizeof(renderer->tmp_dir), "%s", tmp_dir);
    snprintf(renderer->hook_lib_dir, sizeof(renderer->hook_lib_dir), "%s", hook_lib_dir);
    snprintf(renderer->driver_dir, sizeof(renderer->driver_dir), "%s", driver_dir);
    snprintf(renderer->driver_name, sizeof(renderer->driver_name), "%s", driver_name);
    if (ahb_vk_create_core_locked(status, status_size) != 0) {
        reset_ahb_vk_renderer_locked();
        return -1;
    }
    renderer->width = width;
    renderer->height = height;

    for (int usage_index = 0; usage_index < usage_option_count; usage_index++) {
        uint64_t usage = usage_options[usage_index];
        AHardwareBuffer_Desc desc = {
                .width = (uint32_t)width,
                .height = (uint32_t)height,
                .layers = 1,
                .format = AHB_CPU_FORMAT,
                .usage = usage,
                .stride = 0,
                .rfu0 = 0,
                .rfu1 = 0,
        };

        if (!AHardwareBuffer_isSupported(&desc)) {
            continue;
        }

        int allocation_or_import_failed = 0;
        renderer->image_format = VK_FORMAT_UNDEFINED;
        for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
            AHardwareBuffer *buffer = NULL;
            int allocate_result = AHardwareBuffer_allocate(&desc, &buffer);
            if (allocate_result != 0 || buffer == NULL) {
                snprintf(
                        status,
                        status_size,
                        "producer: ahb-vk error allocate %d slot %d usage 0x%llx",
                        allocate_result,
                        i,
                        (unsigned long long)usage);
                allocation_or_import_failed = 1;
                break;
            }
            renderer->slots[i].buffer = buffer;
            if (ahb_vk_import_slot_locked(i, status, status_size) != 0) {
                allocation_or_import_failed = 1;
                break;
            }
        }

        if (!allocation_or_import_failed
                && ahb_vk_create_render_pass_locked(status, status_size) == 0
                && ahb_vk_create_3d_pipeline_locked(status, status_size) == 0
                && ahb_vk_create_framebuffers_locked(status, status_size) == 0) {
            renderer->configured = 1;
            renderer->next_slot = 0;
            renderer->usage = usage;
            return 0;
        }
        if (first_failure[0] == '\0' && status[0] != '\0') {
            snprintf(first_failure, sizeof(first_failure), "%s", status);
        }

        for (int i = 0; i < AHB_CPU_RING_SIZE; i++) {
            AhbVkSlot *slot = &renderer->slots[i];
            destroy_slot_pending_source_locked(slot);
            if (slot->framebuffer != VK_NULL_HANDLE) {
                renderer->vk.destroy_framebuffer(renderer->device, slot->framebuffer, NULL);
            }
            if (slot->image_view != VK_NULL_HANDLE) {
                renderer->vk.destroy_image_view(renderer->device, slot->image_view, NULL);
            }
            if (slot->memory != VK_NULL_HANDLE) {
                renderer->vk.free_memory(renderer->device, slot->memory, NULL);
            }
            if (slot->image != VK_NULL_HANDLE) {
                renderer->vk.destroy_image(renderer->device, slot->image, NULL);
            }
            if (slot->render_fence != VK_NULL_HANDLE) {
                renderer->vk.destroy_fence(renderer->device, slot->render_fence, NULL);
            }
            if (slot->command_buffer != VK_NULL_HANDLE) {
                renderer->vk.free_command_buffers(
                        renderer->device,
                        renderer->command_pool,
                        1,
                        &slot->command_buffer);
            }
            if (slot->buffer != NULL) {
                AHardwareBuffer_release(slot->buffer);
            }
            *slot = (AhbVkSlot){0};
        }
        if (renderer->graphics_pipeline != VK_NULL_HANDLE) {
            renderer->vk.destroy_pipeline(renderer->device, renderer->graphics_pipeline, NULL);
            renderer->graphics_pipeline = VK_NULL_HANDLE;
        }
        if (renderer->pipeline_layout != VK_NULL_HANDLE) {
            renderer->vk.destroy_pipeline_layout(renderer->device, renderer->pipeline_layout, NULL);
            renderer->pipeline_layout = VK_NULL_HANDLE;
        }
        if (renderer->render_pass != VK_NULL_HANDLE) {
            renderer->vk.destroy_render_pass(renderer->device, renderer->render_pass, NULL);
            renderer->render_pass = VK_NULL_HANDLE;
        }
    }

    if (first_failure[0] != '\0') {
        snprintf(status, status_size, "%s", first_failure);
    } else {
        snprintf(
                status,
                status_size,
                "producer: ahb-vk unsupported usage 0x%llx/0x%llx %dx%d",
                (unsigned long long)AHB_VK_PRIMARY_USAGE,
                (unsigned long long)AHB_VK_FALLBACK_USAGE,
                width,
                height);
    }
    reset_ahb_vk_renderer_locked();
    return -1;
}

static int find_free_ahb_vk_slot_locked(void) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    for (int attempt = 0; attempt < AHB_CPU_RING_SIZE; attempt++) {
        int slot_index = (renderer->next_slot + attempt) % AHB_CPU_RING_SIZE;
        if (renderer->slots[slot_index].state == AHB_VK_SLOT_FREE) {
            return slot_index;
        }
    }
    return -1;
}

static int find_free_ahb_vk_slot_with_wait_locked(long long *slot_wait_us) {
    int slot_index = find_free_ahb_vk_slot_locked();
    if (slot_wait_us != NULL) {
        *slot_wait_us = 0;
    }
    if (slot_index >= 0) {
        return slot_index;
    }

    struct timespec wait_start;
    struct timespec wait_end;
    struct timespec deadline = realtime_deadline_after_ns(AHB_VK_SLOT_WAIT_TIMEOUT_NS);
    clock_gettime(CLOCK_MONOTONIC, &wait_start);
    for (;;) {
        int wait_result = pthread_cond_timedwait(&g_ahb_vk_cond, &g_ahb_vk_mutex, &deadline);
        slot_index = find_free_ahb_vk_slot_locked();
        if (slot_index >= 0) {
            break;
        }
        if (wait_result == ETIMEDOUT || wait_result != 0) {
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &wait_end);
    if (slot_wait_us != NULL) {
        *slot_wait_us = elapsed_us(wait_start, wait_end);
    }
    return slot_index;
}

static VkClearValue ahb_vk_clear_value(long long frame_index, int slot_index) {
    float phase = (float)((frame_index % 180LL) / 179.0);
    VkClearValue clear_value;
    clear_value.color.float32[0] = 0.05f + (0.30f * phase);
    clear_value.color.float32[1] = 0.08f + (0.10f * (float)slot_index);
    clear_value.color.float32[2] = 0.42f + (0.35f * (1.0f - phase));
    clear_value.color.float32[3] = 1.0f;
    return clear_value;
}

static void destroy_imported_dmabuf_image_locked(ImportedDmaBufImage *source) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    if (source == NULL) {
        return;
    }
    if (source->image != VK_NULL_HANDLE) {
        renderer->vk.destroy_image(renderer->device, source->image, NULL);
    }
    if (source->memory != VK_NULL_HANDLE) {
        renderer->vk.free_memory(renderer->device, source->memory, NULL);
    }
    *source = (ImportedDmaBufImage){0};
}

static void destroy_slot_pending_source_locked(AhbVkSlot *slot) {
    if (slot == NULL || slot->pending_source == NULL) {
        return;
    }
    destroy_imported_dmabuf_image_locked(slot->pending_source);
    free(slot->pending_source);
    slot->pending_source = NULL;
}

static void destroy_source_cache_locked(void) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    for (int i = 0; i < AHB_VK_SOURCE_CACHE_SIZE; i++) {
        CachedDmaBufSource *entry = &renderer->source_cache[i];
        if (!entry->in_use) {
            continue;
        }
        destroy_imported_dmabuf_image_locked(&entry->image);
        *entry = (CachedDmaBufSource){0};
    }
    renderer->source_cache_clock = 0;
}

static int import_dmabuf_image_locked(
        int fd,
        int source_width,
        int source_height,
        uint32_t drm_format,
        uint64_t modifier,
        int planes,
        uint64_t stride0,
        uint64_t offset0,
        uint64_t size,
        ImportedDmaBufImage *source,
        char *status,
        size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    memset(source, 0, sizeof(*source));

    if (fd < 0 || source_width <= 0 || source_height <= 0
            || source_width > 16384 || source_height > 16384) {
        snprintf(status, status_size, "producer: dmabuf-vk error bad-source-size %dx%d",
                source_width, source_height);
        return -1;
    }
    if (planes != 1 || stride0 == 0 || size == 0) {
        snprintf(status, status_size, "producer: dmabuf-vk unsupported source-layout planes %d stride %llu size %llu",
                planes,
                (unsigned long long)stride0,
                (unsigned long long)size);
        return -1;
    }
    if (renderer->vk.get_memory_fd_properties == NULL) {
        snprintf(status, status_size, "producer: dmabuf-vk unsupported missing-dmabuf-dispatch");
        return -1;
    }

    VkFormat vk_format = map_drm_format_to_vk_format(drm_format);
    if (vk_format == VK_FORMAT_UNDEFINED) {
        snprintf(status, status_size, "producer: dmabuf-vk unsupported drm-format 0x%x",
                drm_format);
        return -1;
    }

    int has_modifier_ext = ahb_vk_has_device_extension(
            renderer->physical_device,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            status,
            status_size);
    if (modifier != 0 && !has_modifier_ext) {
        snprintf(status, status_size, "producer: dmabuf-vk unsupported modifier 0x%llx no-ext",
                (unsigned long long)modifier);
        return -1;
    }

    VkMemoryFdPropertiesKHR fd_properties = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    VkResult result = renderer->vk.get_memory_fd_properties(
            renderer->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            fd,
            &fd_properties);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error fd-props %d", result);
        return -1;
    }

    VkExternalMemoryImageCreateInfo external_image = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkSubresourceLayout plane_layout = {
            .offset = (VkDeviceSize)offset0,
            .size = (VkDeviceSize)size,
            .rowPitch = (VkDeviceSize)stride0,
            .arrayPitch = (VkDeviceSize)size,
            .depthPitch = (VkDeviceSize)size,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .pNext = &external_image,
            .drmFormatModifier = modifier,
            .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &plane_layout,
    };
    int use_modifier_create = has_modifier_ext;
    int source_is_linear = !use_modifier_create || modifier == 0;
    VkImageCreateInfo image_create = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = use_modifier_create
                    ? (const void *)&modifier_info
                    : (const void *)&external_image,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = vk_format,
            .extent = {
                    .width = (uint32_t)source_width,
                    .height = (uint32_t)source_height,
                    .depth = 1,
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = use_modifier_create
                    ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                    : VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = source_is_linear
                    ? VK_IMAGE_LAYOUT_PREINITIALIZED
                    : VK_IMAGE_LAYOUT_UNDEFINED,
    };
    result = renderer->vk.create_image(renderer->device, &image_create, NULL, &source->image);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error create-source %d fmt %d modifier 0x%llx",
                result,
                (int)vk_format,
                (unsigned long long)modifier);
        return -1;
    }

    VkMemoryDedicatedRequirements dedicated_requirements = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 memory_requirements = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            .pNext = &dedicated_requirements,
    };
    VkImageMemoryRequirementsInfo2 requirements_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = source->image,
    };
    renderer->vk.get_image_memory_requirements2(
            renderer->device,
            &requirements_info,
            &memory_requirements);

    uint32_t compatible_bits = fd_properties.memoryTypeBits
            & memory_requirements.memoryRequirements.memoryTypeBits;
    VkDeviceSize allocation_size = (VkDeviceSize)size;
    if (allocation_size < memory_requirements.memoryRequirements.size) {
        allocation_size = memory_requirements.memoryRequirements.size;
    }
    int import_fd = dup(fd);
    if (import_fd < 0) {
        snprintf(status, status_size, "producer: dmabuf-vk error dup-source");
        destroy_imported_dmabuf_image_locked(source);
        return -1;
    }

    VkResult last_allocate_result = VK_SUCCESS;
    int selected_memory_type = -1;
    int selected_dedicated = 0;
    for (int candidate = 0; candidate < 32; candidate++) {
        if ((compatible_bits & (1u << candidate)) == 0) {
            continue;
        }
        for (int dedicated_attempt = 0; dedicated_attempt < 2; dedicated_attempt++) {
            int use_dedicated = dedicated_attempt == 0
                    && (dedicated_requirements.requiresDedicatedAllocation
                            || dedicated_requirements.prefersDedicatedAllocation);
            int candidate_fd = import_fd;
            if (!use_dedicated && dedicated_attempt == 0) {
                continue;
            }
            if (candidate_fd < 0) {
                candidate_fd = dup(fd);
                if (candidate_fd < 0) {
                    continue;
                }
            }
            VkMemoryDedicatedAllocateInfo dedicated_allocate = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                    .image = use_dedicated ? source->image : VK_NULL_HANDLE,
                    .buffer = VK_NULL_HANDLE,
            };
            VkImportMemoryFdInfoKHR import_info = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                    .pNext = use_dedicated ? &dedicated_allocate : NULL,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    .fd = candidate_fd,
            };
            VkMemoryAllocateInfo allocate_info = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .pNext = &import_info,
                    .allocationSize = allocation_size,
                    .memoryTypeIndex = (uint32_t)candidate,
            };
            result = renderer->vk.allocate_memory(
                    renderer->device,
                    &allocate_info,
                    NULL,
                    &source->memory);
            if (result == VK_SUCCESS) {
                // The imported dma-buf payload is now referenced by Vulkan.
                // Close our duplicated fd immediately so long benchmark runs
                // do not exhaust the Android app process fd table.
                close(candidate_fd);
                if (candidate_fd == import_fd) {
                    import_fd = -1;
                }
                selected_memory_type = candidate;
                selected_dedicated = use_dedicated;
                break;
            }
            last_allocate_result = result;
            close(candidate_fd);
            if (candidate_fd == import_fd) {
                import_fd = -1;
            }
        }
        if (selected_memory_type >= 0) {
            break;
        }
    }
    if (import_fd >= 0) {
        close(import_fd);
    }
    if (selected_memory_type < 0) {
        snprintf(status, status_size, "producer: dmabuf-vk error alloc-source %d bits fd=0x%x req=0x%x",
                last_allocate_result,
                fd_properties.memoryTypeBits,
                memory_requirements.memoryRequirements.memoryTypeBits);
        destroy_imported_dmabuf_image_locked(source);
        return -1;
    }

    result = renderer->vk.bind_image_memory(renderer->device, source->image, source->memory, 0);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error bind-source %d type %d dedicated %d",
                result,
                selected_memory_type,
                selected_dedicated);
        destroy_imported_dmabuf_image_locked(source);
        return -1;
    }

    source->layout = source_is_linear ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_GENERAL;
    source->format = vk_format;
    source->width = source_width;
    source->height = source_height;
    return 0;
}

static int cached_source_matches(
        const CachedDmaBufSource *entry,
        const struct stat *st,
        int source_width,
        int source_height,
        uint32_t drm_format,
        uint64_t modifier,
        int planes,
        uint64_t stride0,
        uint64_t offset0,
        uint64_t size) {
    return entry != NULL
            && entry->in_use
            && entry->dev == st->st_dev
            && entry->ino == st->st_ino
            && entry->file_size == (uint64_t)st->st_size
            && entry->width == source_width
            && entry->height == source_height
            && entry->drm_format == drm_format
            && entry->modifier == modifier
            && entry->planes == planes
            && entry->stride0 == stride0
            && entry->offset0 == offset0
            && entry->size == size;
}

static ImportedDmaBufImage *get_cached_dmabuf_image_locked(
        int fd,
        int source_width,
        int source_height,
        uint32_t drm_format,
        uint64_t modifier,
        int planes,
        uint64_t stride0,
        uint64_t offset0,
        uint64_t size,
        char *status,
        size_t status_size) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(status, status_size, "producer: dmabuf-vk error source-stat errno %d", errno);
        return NULL;
    }

    for (int i = 0; i < AHB_VK_SOURCE_CACHE_SIZE; i++) {
        CachedDmaBufSource *entry = &renderer->source_cache[i];
        if (cached_source_matches(
                    entry,
                    &st,
                    source_width,
                    source_height,
                    drm_format,
                    modifier,
                    planes,
                    stride0,
                    offset0,
                    size)) {
            entry->last_used = ++renderer->source_cache_clock;
            return &entry->image;
        }
    }

    int selected_index = -1;
    uint64_t oldest_use = UINT64_MAX;
    for (int i = 0; i < AHB_VK_SOURCE_CACHE_SIZE; i++) {
        CachedDmaBufSource *entry = &renderer->source_cache[i];
        if (!entry->in_use) {
            selected_index = i;
            break;
        }
        if (entry->last_used < oldest_use) {
            oldest_use = entry->last_used;
            selected_index = i;
        }
    }
    if (selected_index < 0) {
        snprintf(status, status_size, "producer: dmabuf-vk error source-cache-full");
        return NULL;
    }

    CachedDmaBufSource *entry = &renderer->source_cache[selected_index];
    if (entry->in_use) {
        if (renderer->vk.device_wait_idle != NULL) {
            renderer->vk.device_wait_idle(renderer->device);
        }
        destroy_imported_dmabuf_image_locked(&entry->image);
        *entry = (CachedDmaBufSource){0};
    }

    entry->dev = st.st_dev;
    entry->ino = st.st_ino;
    entry->file_size = (uint64_t)st.st_size;
    entry->width = source_width;
    entry->height = source_height;
    entry->drm_format = drm_format;
    entry->modifier = modifier;
    entry->planes = planes;
    entry->stride0 = stride0;
    entry->offset0 = offset0;
    entry->size = size;
    entry->last_used = ++renderer->source_cache_clock;
    if (import_dmabuf_image_locked(
                fd,
                source_width,
                source_height,
                drm_format,
                modifier,
                planes,
                stride0,
                offset0,
                size,
                &entry->image,
                status,
                status_size) != 0) {
        *entry = (CachedDmaBufSource){0};
        return NULL;
    }
    entry->in_use = 1;
    return &entry->image;
}

static int render_dmabuf_to_ahb_vk_slot(
        int slot_index,
        long long frame_index,
        int dmabuf_fd,
        int source_width,
        int source_height,
        uint32_t drm_format,
        uint64_t modifier,
        int planes,
        uint64_t stride0,
        uint64_t offset0,
        uint64_t size,
        char *status,
        size_t status_size,
        long long *wait_us,
        int export_acquire_fence,
        int *acquire_fence_fd) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    AhbVkSlot *slot = &renderer->slots[slot_index];
    ImportedDmaBufImage *source = NULL;
    struct timespec wait_start;
    struct timespec wait_end;

    if (acquire_fence_fd != NULL) {
        *acquire_fence_fd = -1;
    }
    if (export_acquire_fence
            && (!renderer->supports_external_fence
            || !renderer->supports_external_fence_fd
            || renderer->vk.get_fence_fd == NULL)) {
        snprintf(status, status_size,
                "producer: dmabuf-vk-native-present unsupported missing-sync-fd-fence");
        return -1;
    }

    VkResult result = VK_SUCCESS;
    if (!export_acquire_fence) {
        result = renderer->vk.wait_for_fences(
                renderer->device,
                1,
                &slot->render_fence,
                VK_TRUE,
                AHB_VK_FENCE_TIMEOUT_NS);
        if (result != VK_SUCCESS) {
            snprintf(status, status_size, "producer: dmabuf-vk error pre-fence %d slot %d",
                    result,
                    slot_index);
            return -1;
        }
    }
    result = renderer->vk.reset_fences(renderer->device, 1, &slot->render_fence);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error reset-fence %d slot %d",
                result,
                slot_index);
        return -1;
    }
    source = get_cached_dmabuf_image_locked(
                dmabuf_fd,
                source_width,
                source_height,
                drm_format,
                modifier,
                planes,
                stride0,
                offset0,
                size,
                status,
                status_size);
    if (source == NULL) {
        return -1;
    }

    result = renderer->vk.reset_command_buffer(slot->command_buffer, 0);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error reset-cmd %d slot %d",
                result,
                slot_index);
        return -1;
    }

    VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = renderer->vk.begin_command_buffer(slot->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error begin-cmd %d slot %d",
                result,
                slot_index);
        return -1;
    }

    VkImageMemoryBarrier barriers[2];
    memset(barriers, 0, sizeof(barriers));
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = source->layout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = source->image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = slot->layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_READ_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = slot->layout;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = slot->image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    renderer->vk.cmd_pipeline_barrier(
            slot->command_buffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            2,
            barriers);

    VkImageBlit blit = {
            .srcSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },
            .srcOffsets = {
                    { .x = 0, .y = 0, .z = 0 },
                    { .x = source_width, .y = source_height, .z = 1 },
            },
            .dstSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },
            .dstOffsets = {
                    { .x = 0, .y = 0, .z = 0 },
                    { .x = renderer->width, .y = renderer->height, .z = 1 },
            },
    };
    renderer->vk.cmd_blit_image(
            slot->command_buffer,
            source->image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            slot->image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_NEAREST);

    VkImageMemoryBarrier present_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slot->image,
            .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },
    };
    renderer->vk.cmd_pipeline_barrier(
            slot->command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            1,
            &present_barrier);

    result = renderer->vk.end_command_buffer(slot->command_buffer);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error end-cmd %d slot %d",
                result,
                slot_index);
        return -1;
    }

    VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->command_buffer,
    };
    clock_gettime(CLOCK_MONOTONIC, &wait_start);
    result = renderer->vk.queue_submit(renderer->queue, 1, &submit_info, slot->render_fence);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error submit %d slot %d",
                result,
                slot_index);
        return -1;
    }
    if (export_acquire_fence) {
        int fence_fd = -1;
        VkFenceGetFdInfoKHR fd_info = {
                .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
                .fence = slot->render_fence,
                .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        result = renderer->vk.get_fence_fd(renderer->device, &fd_info, &fence_fd);
        clock_gettime(CLOCK_MONOTONIC, &wait_end);
        *wait_us = elapsed_us(wait_start, wait_end);
        if (result != VK_SUCCESS || fence_fd < -1) {
            snprintf(status, status_size,
                    "producer: dmabuf-vk-native-present error export-fence %d fd %d slot %d",
                    result,
                    fence_fd,
                    slot_index);
            if (fence_fd >= 0) {
                close(fence_fd);
            }
            return -1;
        }
        if (acquire_fence_fd != NULL) {
            *acquire_fence_fd = fence_fd;
        } else if (fence_fd >= 0) {
            close(fence_fd);
        }
        source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        slot->layout = VK_IMAGE_LAYOUT_GENERAL;
        snprintf(status, status_size,
                "producer: dmabuf-vk-native-present frame %lld gpu-blit-async loader adrenotools driver %s slot %d id %llu submit %lldus acquire-fence=%s source %dx%d target %dx%d drm 0x%x modifier 0x%llx zero-copy=gpu explicit-sync=surfacecontrol-acquire-fence",
                frame_index,
                renderer->driver_name,
                slot_index,
                (unsigned long long)slot->buffer_id,
                *wait_us,
                fence_fd >= 0 ? "sync-fd" : "already-signaled",
                source_width,
                source_height,
                renderer->width,
                renderer->height,
                drm_format,
                (unsigned long long)modifier);
        return 0;
    }
    result = renderer->vk.wait_for_fences(
            renderer->device,
            1,
            &slot->render_fence,
            VK_TRUE,
            AHB_VK_FENCE_TIMEOUT_NS);
    clock_gettime(CLOCK_MONOTONIC, &wait_end);
    *wait_us = elapsed_us(wait_start, wait_end);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: dmabuf-vk error wait %d slot %d",
                result,
                slot_index);
        return -1;
    }
    source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    slot->layout = VK_IMAGE_LAYOUT_GENERAL;
    snprintf(status, status_size,
            "producer: dmabuf-vk frame %lld gpu-blit loader adrenotools driver %s slot %d id %llu wait %lldus source %dx%d target %dx%d drm 0x%x modifier 0x%llx zero-copy=gpu",
            frame_index,
            renderer->driver_name,
            slot_index,
            (unsigned long long)slot->buffer_id,
            *wait_us,
            source_width,
            source_height,
            renderer->width,
            renderer->height,
            drm_format,
            (unsigned long long)modifier);
    return 0;
}

static int render_ahb_vk_slot(
        int slot_index,
        long long frame_index,
        char *status,
        size_t status_size,
        long long *wait_us) {
    AhbVkRenderer *renderer = &g_ahb_vk_renderer;
    AhbVkSlot *slot = &renderer->slots[slot_index];
    struct timespec wait_start;
    struct timespec wait_end;

    VkResult result = renderer->vk.wait_for_fences(
            renderer->device,
            1,
            &slot->render_fence,
            VK_TRUE,
            AHB_VK_FENCE_TIMEOUT_NS);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error pre-fence %d slot %d", result, slot_index);
        return -1;
    }
    result = renderer->vk.reset_fences(renderer->device, 1, &slot->render_fence);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error reset-fence %d slot %d", result, slot_index);
        return -1;
    }
    result = renderer->vk.reset_command_buffer(slot->command_buffer, 0);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error reset-cmd %d slot %d", result, slot_index);
        return -1;
    }

    VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = renderer->vk.begin_command_buffer(slot->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error begin-cmd %d slot %d", result, slot_index);
        return -1;
    }

    VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = slot->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = slot->layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slot->image,
            .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
            },
    };
    renderer->vk.cmd_pipeline_barrier(
            slot->command_buffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            1,
            &barrier);

    VkClearValue clear_value = ahb_vk_clear_value(frame_index, slot_index);
    VkRenderPassBeginInfo render_pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderer->render_pass,
            .framebuffer = slot->framebuffer,
            .renderArea = {
                    .offset = { .x = 0, .y = 0 },
                    .extent = { .width = (uint32_t)renderer->width, .height = (uint32_t)renderer->height },
            },
            .clearValueCount = 1,
            .pClearValues = &clear_value,
    };
    renderer->vk.cmd_begin_render_pass(
            slot->command_buffer,
            &render_pass_begin,
            VK_SUBPASS_CONTENTS_INLINE);

    VkClearAttachment bar_attachment = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
    };
    VkClearRect bar_rect = {
            .rect = {
                    .offset = { .x = 0, .y = 0 },
                    .extent = { .width = (uint32_t)(renderer->width / 5), .height = (uint32_t)renderer->height },
            },
            .baseArrayLayer = 0,
            .layerCount = 1,
    };
    int moving_x = (int)((frame_index * 29LL) % renderer->width);
    int bar_width = renderer->width / 8;
    if (bar_width < 16) {
        bar_width = 16;
    }
    for (int i = 0; i < 3; i++) {
        bar_attachment.clearValue.color.float32[0] = i == 0 ? 1.0f : 0.05f * (float)slot_index;
        bar_attachment.clearValue.color.float32[1] = i == 1 ? 1.0f : 0.20f + (0.08f * (float)i);
        bar_attachment.clearValue.color.float32[2] = i == 2 ? 1.0f : 0.10f + (0.10f * (float)i);
        bar_attachment.clearValue.color.float32[3] = 1.0f;
        bar_rect.rect.offset.x = (moving_x + (i * renderer->width / 4)) % renderer->width;
        bar_rect.rect.offset.y = i * (renderer->height / 12);
        bar_rect.rect.extent.width = (uint32_t)bar_width;
        bar_rect.rect.extent.height = (uint32_t)(renderer->height - bar_rect.rect.offset.y);
        renderer->vk.cmd_clear_attachments(
                slot->command_buffer,
                1,
                &bar_attachment,
                1,
                &bar_rect);
    }

    AhbVk3dPushConstants push_constants = {
            .phase = (float)((frame_index % 240LL) / 240.0),
            .aspect = renderer->height > 0 ? ((float)renderer->width / (float)renderer->height) : 1.0f,
    };
    renderer->vk.cmd_bind_pipeline(
            slot->command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            renderer->graphics_pipeline);
    renderer->vk.cmd_push_constants(
            slot->command_buffer,
            renderer->pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(push_constants),
            &push_constants);
    renderer->vk.cmd_draw(slot->command_buffer, 12, 1, 0, 0);

    renderer->vk.cmd_end_render_pass(slot->command_buffer);
    result = renderer->vk.end_command_buffer(slot->command_buffer);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error end-cmd %d slot %d", result, slot_index);
        return -1;
    }

    VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->command_buffer,
    };
    clock_gettime(CLOCK_MONOTONIC, &wait_start);
    result = renderer->vk.queue_submit(renderer->queue, 1, &submit_info, slot->render_fence);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error submit %d slot %d", result, slot_index);
        return -1;
    }
    result = renderer->vk.wait_for_fences(
            renderer->device,
            1,
            &slot->render_fence,
            VK_TRUE,
            AHB_VK_FENCE_TIMEOUT_NS);
    clock_gettime(CLOCK_MONOTONIC, &wait_end);
    *wait_us = elapsed_us(wait_start, wait_end);
    if (result != VK_SUCCESS) {
        snprintf(status, status_size, "producer: ahb-vk error wait %d slot %d", result, slot_index);
        return -1;
    }

    slot->layout = VK_IMAGE_LAYOUT_GENERAL;
    return 0;
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeRenderProducerFrame(
        JNIEnv *env,
        jclass clazz,
        jobject surface,
        jint requested_width,
        jint requested_height,
        jlong frame_index,
        jlong frame_time_nanos) {
    (void)clazz;

    if (surface == NULL) {
        return make_status(env, "producer: native error missing-surface");
    }
    if (requested_width <= 0 || requested_height <= 0) {
        return make_status(
                env,
                "producer: native error bad-size %dx%d",
                (int)requested_width,
                (int)requested_height);
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (window == NULL) {
        return make_status(env, "producer: native error window-from-surface");
    }

    ANativeWindow_Buffer buffer;
    ARect dirty = {
            .left = 0,
            .top = 0,
            .right = requested_width,
            .bottom = requested_height,
    };

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int lock_result = ANativeWindow_lock(window, &buffer, &dirty);
    if (lock_result != 0) {
        ANativeWindow_release(window);
        return make_status(env, "producer: native error lock-failed:%d", lock_result);
    }

    int draw_width = requested_width < buffer.width ? requested_width : buffer.width;
    int draw_height = requested_height < buffer.height ? requested_height : buffer.height;
    if (buffer.bits == NULL || draw_width <= 0 || draw_height <= 0 || buffer.stride < draw_width) {
        int unlock_result = ANativeWindow_unlockAndPost(window);
        ANativeWindow_release(window);
        return make_status(
                env,
                "producer: native error invalid-buffer stride:%d unlock:%d",
                buffer.stride,
                unlock_result);
    }

    write_pattern(
            &buffer,
            draw_width,
            draw_height,
            (long long)frame_index,
            (long long)frame_time_nanos);

    int unlock_result = ANativeWindow_unlockAndPost(window);
    clock_gettime(CLOCK_MONOTONIC, &end);
    ANativeWindow_release(window);

    if (unlock_result != 0) {
        return make_status(env, "producer: native error unlock-failed:%d", unlock_result);
    }

    return make_status(
            env,
            "producer: native-window frame %lld stride %d %dx%d %lldus",
            (long long)frame_index,
            buffer.stride,
            draw_width,
            draw_height,
            elapsed_us(start, end));
}

JNIEXPORT jobject JNICALL
Java_io_waylandie_display_MainActivity_nativeAcquireAhbCpuFrame(
        JNIEnv *env,
        jclass clazz,
        jint requested_width,
        jint requested_height,
        jlong frame_index,
        jlong frame_time_nanos) {
    (void)clazz;

    char status[192];
    jobject hardware_buffer = NULL;
    int slot_index = -1;
    long long generation = 0;

    if (requested_width <= 0 || requested_height <= 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback bad-size %dx%d",
                (int)requested_width,
                (int)requested_height);
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }

    pthread_mutex_lock(&g_ahb_cpu_mutex);

    if (configure_ahb_cpu_ring_locked(
                (int)requested_width,
                (int)requested_height,
                status,
                sizeof(status)) != 0) {
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot_index = find_free_ahb_cpu_slot_locked();
    if (slot_index < 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback ring-busy %d/%d gen %lld",
                AHB_CPU_RING_SIZE,
                AHB_CPU_RING_SIZE,
                g_ahb_cpu_generation);
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    AhbCpuSlot *slot = &g_ahb_cpu_slots[slot_index];
    AHardwareBuffer_Desc actual_desc;
    AHardwareBuffer_describe(slot->buffer, &actual_desc);

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    void *bits = NULL;
    ARect dirty = {
            .left = 0,
            .top = 0,
            .right = requested_width,
            .bottom = requested_height,
    };
    int lock_result = AHardwareBuffer_lock(
            slot->buffer,
            AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
            -1,
            &dirty,
            &bits);
    if (lock_result != 0 || bits == NULL) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback lock-failed:%d slot %d",
                lock_result,
                slot_index);
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    int draw_width = requested_width < (jint)actual_desc.width
            ? requested_width
            : (jint)actual_desc.width;
    int draw_height = requested_height < (jint)actual_desc.height
            ? requested_height
            : (jint)actual_desc.height;
    if (actual_desc.format != AHB_CPU_FORMAT
            || actual_desc.stride < (uint32_t)draw_width
            || draw_width <= 0
            || draw_height <= 0) {
        int unlock_after_error = AHardwareBuffer_unlock(slot->buffer, NULL);
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback invalid-buffer fmt:%u stride:%u unlock:%d",
                actual_desc.format,
                actual_desc.stride,
                unlock_after_error);
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    write_rgba_pattern(
            (uint8_t *)bits,
            actual_desc.stride,
            draw_width,
            draw_height,
            (long long)frame_index,
            (long long)frame_time_nanos,
            1);

    int unlock_result = AHardwareBuffer_unlock(slot->buffer, NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (unlock_result != 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback unlock-failed:%d slot %d",
                unlock_result,
                slot_index);
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    hardware_buffer = AHardwareBuffer_toHardwareBuffer(env, slot->buffer);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (hardware_buffer == NULL) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-cpu fallback wrap-failed slot %d",
                slot_index);
        generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    // The slot stays busy until Java's SurfaceControl release callback waits
    // its release fence, closes this Java wrapper, and calls native release.
    slot->in_use_by_surface_control = 1;
    slot->last_frame_index = (long long)frame_index;
    slot->generation = g_ahb_cpu_generation;
    slot->stride = actual_desc.stride;
    slot->usage = actual_desc.usage;
    generation = g_ahb_cpu_generation;
    g_ahb_cpu_next_slot = (slot_index + 1) % AHB_CPU_RING_SIZE;

    snprintf(
            status,
            sizeof(status),
            "producer: ahb-cpu frame %lld slot %d id %llu stride %u %dx%d usage 0x%llx %lldus",
            (long long)frame_index,
            slot_index,
            (unsigned long long)slot->buffer_id,
            actual_desc.stride,
            draw_width,
            draw_height,
            (unsigned long long)g_ahb_cpu_usage,
            elapsed_us(start, end));

    pthread_mutex_unlock(&g_ahb_cpu_mutex);

    jobject frame = make_ahb_cpu_frame(
            env,
            hardware_buffer,
            (jint)slot_index,
            (jlong)generation,
            status);
    (*env)->DeleteLocalRef(env, hardware_buffer);

    if (frame == NULL || (*env)->ExceptionCheck(env)) {
        pthread_mutex_lock(&g_ahb_cpu_mutex);
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_cpu_generation == generation
                && g_ahb_cpu_slots[slot_index].in_use_by_surface_control) {
            g_ahb_cpu_slots[slot_index].in_use_by_surface_control = 0;
        }
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
    }

    return frame;
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeExportAhbCpuSlot(
        JNIEnv *env,
        jclass clazz,
        jint socket_fd,
        jint slot_index,
        jlong generation) {
    (void)clazz;

    AHardwareBuffer *buffer = NULL;
    AHardwareBuffer_Desc actual_desc;
    uint64_t buffer_id = 0;

    if (socket_fd < 0) {
        return make_status(
                env,
                "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=bad-socket-fd");
    }

    pthread_mutex_lock(&g_ahb_cpu_mutex);
    if (generation != (jlong)g_ahb_cpu_generation) {
        long long current_generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(
                env,
                "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=stale-generation generation=%lld current=%lld",
                (long long)generation,
                current_generation);
    }
    if (slot_index < 0 || slot_index >= AHB_CPU_RING_SIZE) {
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(
                env,
                "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=bad-slot slot=%d",
                (int)slot_index);
    }

    AhbCpuSlot *slot = &g_ahb_cpu_slots[slot_index];
    if (slot->buffer == NULL || !slot->in_use_by_surface_control) {
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(
                env,
                "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=slot-not-live slot=%d",
                (int)slot_index);
    }

    buffer = slot->buffer;
    AHardwareBuffer_acquire(buffer);
    pthread_mutex_unlock(&g_ahb_cpu_mutex);

    AHardwareBuffer_describe(buffer, &actual_desc);
    (void)AHardwareBuffer_getId(buffer, &buffer_id);

    errno = 0;
    int send_result = AHardwareBuffer_sendHandleToUnixSocket(buffer, socket_fd);
    int send_errno = errno;
    AHardwareBuffer_release(buffer);

    if (send_result != 0) {
        return make_status(
                env,
                "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=fail reason=send-handle result=%d errno=%d slot=%d generation=%lld width=%u height=%u format=%u layers=%u usage=0x%llx stride=%u id=%llu",
                send_result,
                send_errno,
                (int)slot_index,
                (long long)generation,
                actual_desc.width,
                actual_desc.height,
                actual_desc.format,
                actual_desc.layers,
                (unsigned long long)actual_desc.usage,
                actual_desc.stride,
                (unsigned long long)buffer_id);
    }

    return make_status(
            env,
            "probe=ahb-present-export api=AHardwareBuffer_sendHandleToUnixSocket status=pass slot=%d generation=%lld width=%u height=%u format=%u layers=%u usage=0x%llx stride=%u id=%llu",
            (int)slot_index,
            (long long)generation,
            actual_desc.width,
            actual_desc.height,
            actual_desc.format,
            actual_desc.layers,
            (unsigned long long)actual_desc.usage,
            actual_desc.stride,
            (unsigned long long)buffer_id);
}

JNIEXPORT jobject JNICALL
Java_io_waylandie_display_MainActivity_nativeAcquireAhbVkFrame(
        JNIEnv *env,
        jclass clazz,
        jint requested_width,
        jint requested_height,
        jlong frame_index,
        jlong frame_time_nanos,
        jstring tmp_dir_string,
        jstring hook_lib_dir_string,
        jstring driver_dir_string,
        jstring driver_name_string) {
    (void)clazz;
    (void)frame_time_nanos;

    char status[256];
    jobject hardware_buffer = NULL;
    int slot_index = -1;
    long long generation = 0;
    long long wait_us = 0;
    long long slot_wait_us = 0;
    const char *tmp_dir = NULL;
    const char *hook_lib_dir = NULL;
    const char *driver_dir = NULL;
    const char *driver_name = NULL;

    if (requested_width <= 0 || requested_height <= 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-vk unsupported bad-size %dx%d",
                (int)requested_width,
                (int)requested_height);
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }
    if (tmp_dir_string == NULL
            || hook_lib_dir_string == NULL
            || driver_dir_string == NULL
            || driver_name_string == NULL) {
        snprintf(status, sizeof(status), "producer: ahb-vk unsupported missing-loader-strings");
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }

    tmp_dir = (*env)->GetStringUTFChars(env, tmp_dir_string, NULL);
    hook_lib_dir = (*env)->GetStringUTFChars(env, hook_lib_dir_string, NULL);
    driver_dir = (*env)->GetStringUTFChars(env, driver_dir_string, NULL);
    driver_name = (*env)->GetStringUTFChars(env, driver_name_string, NULL);
    if (tmp_dir == NULL || hook_lib_dir == NULL || driver_dir == NULL || driver_name == NULL) {
        if (tmp_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
        }
        if (hook_lib_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
        }
        if (driver_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
        }
        if (driver_name != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
        }
        snprintf(status, sizeof(status), "producer: ahb-vk unsupported loader-string-oom");
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }

    pthread_mutex_lock(&g_ahb_vk_mutex);
    int configure_result = configure_ahb_vk_ring_locked(
            (int)requested_width,
            (int)requested_height,
            tmp_dir,
            hook_lib_dir,
            driver_dir,
            driver_name,
            status,
            sizeof(status));
    (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
    (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
    (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
    (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
    if (configure_result != 0) {
        generation = g_ahb_vk_renderer.generation;
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot_index = find_free_ahb_vk_slot_with_wait_locked(&slot_wait_us);
    if (slot_index < 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: ahb-vk ring-busy %d/%d gen %lld slot-wait %lldus",
                AHB_CPU_RING_SIZE,
                AHB_CPU_RING_SIZE,
                g_ahb_vk_renderer.generation,
                slot_wait_us);
        generation = g_ahb_vk_renderer.generation;
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    AhbVkSlot *slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_RENDERING;
    generation = g_ahb_vk_renderer.generation;
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    if (render_ahb_vk_slot(
                slot_index,
                (long long)frame_index,
                status,
                sizeof(status),
                &wait_us) != 0) {
        pthread_mutex_lock(&g_ahb_vk_mutex);
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state == AHB_VK_SLOT_RENDERING) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FAILED;
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    hardware_buffer = AHardwareBuffer_toHardwareBuffer(env, slot->buffer);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (hardware_buffer == NULL) {
        pthread_mutex_lock(&g_ahb_vk_mutex);
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state == AHB_VK_SLOT_RENDERING) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        snprintf(status, sizeof(status), "producer: ahb-vk error wrap-failed slot %d", slot_index);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    pthread_mutex_lock(&g_ahb_vk_mutex);
    if (g_ahb_vk_renderer.generation != generation
            || g_ahb_vk_renderer.slots[slot_index].state != AHB_VK_SLOT_RENDERING) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        (*env)->DeleteLocalRef(env, hardware_buffer);
        snprintf(status, sizeof(status), "producer: ahb-vk error stale-render slot %d", slot_index);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL;
    slot->last_frame_index = (long long)frame_index;
    slot->generation = generation;
    g_ahb_vk_renderer.next_slot = (slot_index + 1) % AHB_CPU_RING_SIZE;

    snprintf(
            status,
            sizeof(status),
            "producer: ahb-vk frame %lld gpu3d draw12 loader adrenotools driver %s slot %d id %llu wait %lldus slot-wait %lldus %dx%d fmt %u usage 0x%llx",
            (long long)frame_index,
            g_ahb_vk_renderer.driver_name,
            slot_index,
            (unsigned long long)slot->buffer_id,
            wait_us,
            slot_wait_us,
            (int)requested_width,
            (int)requested_height,
            (unsigned int)g_ahb_vk_renderer.image_format,
            (unsigned long long)g_ahb_vk_renderer.usage);
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    jobject frame = make_ahb_cpu_frame(
            env,
            hardware_buffer,
            (jint)slot_index,
            (jlong)generation,
            status);
    (*env)->DeleteLocalRef(env, hardware_buffer);

    if (frame == NULL || (*env)->ExceptionCheck(env)) {
        pthread_mutex_lock(&g_ahb_vk_mutex);
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state
                        == AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
    }

    return frame;
}

typedef struct NativeAhbVkReleaseContext {
    int slot_index;
    long long generation;
} NativeAhbVkReleaseContext;

typedef ASurfaceControl *(*AndroidSurfaceControlFromJavaFn)(JNIEnv *env, jobject surface_control);
typedef void (*AndroidSurfaceTransactionSetBufferWithReleaseFn)(
        ASurfaceTransaction *transaction,
        ASurfaceControl *surface_control,
        AHardwareBuffer *buffer,
        int acquire_fence_fd,
        void *context,
        ASurfaceTransaction_OnBufferRelease callback);
/* Android 14 (API 34) fallback: plain setBuffer + present-completion callback. */
typedef void (*AndroidSurfaceTransactionSetBufferFn)(
        ASurfaceTransaction *transaction,
        ASurfaceControl *surface_control,
        AHardwareBuffer *buffer,
        int acquire_fence_fd);
typedef void (*AndroidSurfaceTransactionOnCompleteFn)(
        ASurfaceTransaction *transaction,
        void *context,
        void (*callback)(void *context, ASurfaceTransaction *transaction, int present_fence_fd));

static void *load_android_native_symbol(const char *name) {
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol != NULL) {
        return symbol;
    }
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        return NULL;
    }
    return dlsym(library, name);
}

static void native_ahb_vk_buffer_release(void *context, int release_fence_fd) {
    NativeAhbVkReleaseContext *release_context = (NativeAhbVkReleaseContext *)context;
    if (release_fence_fd >= 0) {
        struct pollfd poll_fd = {
                .fd = release_fence_fd,
                .events = POLLIN,
        };
        while (poll(&poll_fd, 1, -1) < 0 && errno == EINTR) {
        }
        close(release_fence_fd);
    }
    if (release_context == NULL) {
        return;
    }

    pthread_mutex_lock(&g_ahb_vk_mutex);
    if (release_context->generation == g_ahb_vk_renderer.generation
            && release_context->slot_index >= 0
            && release_context->slot_index < AHB_CPU_RING_SIZE) {
        AhbVkSlot *slot = &g_ahb_vk_renderer.slots[release_context->slot_index];
        if (slot->state == AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL
                && slot->generation == release_context->generation) {
            destroy_slot_pending_source_locked(slot);
            slot->state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
    }
    pthread_mutex_unlock(&g_ahb_vk_mutex);
    free(release_context);
}

/* Android 14 fallback: ASurfaceTransaction_setOnComplete gives a present fence, not a
 * buffer-release callback. We treat present completion as the release point; the slot
 * ring plus generation guard keeps recycling safe against late recycles. */
static void native_ahb_vk_present_complete(void *context, ASurfaceTransaction *transaction, int present_fence_fd) {
    (void)transaction;
    native_ahb_vk_buffer_release(context, present_fence_fd);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativePresentAhbVkDmaBufFrame(
        JNIEnv *env,
        jclass clazz,
        jobject surface_control_object,
        jint dmabuf_fd,
        jint source_width,
        jint source_height,
        jlong drm_format,
        jlong modifier,
        jint planes,
        jlong stride0,
        jlong offset0,
        jlong size,
        jint target_width,
        jint target_height,
        jlong frame_index,
        jstring tmp_dir_string,
        jstring hook_lib_dir_string,
        jstring driver_dir_string,
        jstring driver_name_string) {
    (void)clazz;

    char status[1024];
    ASurfaceControl *surface_control = NULL;
    ASurfaceTransaction *transaction = NULL;
    const char *tmp_dir = NULL;
    const char *hook_lib_dir = NULL;
    const char *driver_dir = NULL;
    const char *driver_name = NULL;
    NativeAhbVkReleaseContext *release_context = NULL;
    AndroidSurfaceControlFromJavaFn surface_control_from_java = NULL;
    AndroidSurfaceTransactionSetBufferWithReleaseFn set_buffer_with_release = NULL;
    AndroidSurfaceTransactionSetBufferFn set_buffer_plain = NULL;
    AndroidSurfaceTransactionOnCompleteFn on_complete = NULL;
    int slot_index = -1;
    long long generation = 0;
    long long submit_us = 0;
    long long slot_wait_us = 0;
    int acquire_fence_fd = -1;
    int source_wait_fence_fd = -1;

    if (surface_control_object == NULL) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported missing-surfacecontrol");
        return (*env)->NewStringUTF(env, status);
    }
    if (dmabuf_fd < 0
            || source_width <= 0
            || source_height <= 0
            || target_width <= 0
            || target_height <= 0
            || drm_format < 0
            || drm_format > UINT32_MAX
            || modifier < 0
            || planes != 1
            || stride0 <= 0
            || offset0 < 0
            || size <= 0) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported bad-meta source %dx%d target %dx%d planes %d",
                (int)source_width,
                (int)source_height,
                (int)target_width,
                (int)target_height,
                (int)planes);
        return (*env)->NewStringUTF(env, status);
    }
    if (tmp_dir_string == NULL
            || hook_lib_dir_string == NULL
            || driver_dir_string == NULL
            || driver_name_string == NULL) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported missing-loader-strings");
        return (*env)->NewStringUTF(env, status);
    }

    surface_control_from_java = (AndroidSurfaceControlFromJavaFn)
            load_android_native_symbol("ASurfaceControl_fromJava");
    set_buffer_with_release = (AndroidSurfaceTransactionSetBufferWithReleaseFn)
            load_android_native_symbol("ASurfaceTransaction_setBufferWithRelease");
    if (set_buffer_with_release == NULL) {
        /* Android 14 fallback: setBuffer (API 24+) + setOnComplete (API 29) */
        set_buffer_plain = (AndroidSurfaceTransactionSetBufferFn)
                load_android_native_symbol("ASurfaceTransaction_setBuffer");
        on_complete = (AndroidSurfaceTransactionOnCompleteFn)
                load_android_native_symbol("ASurfaceTransaction_setOnComplete");
    }
    if (surface_control_from_java == NULL
            || (set_buffer_with_release == NULL
                && (set_buffer_plain == NULL || on_complete == NULL))) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported missing-surfacecontrol-api from-java=%d set-buffer-release=%d plain=%d on-complete=%d",
                surface_control_from_java != NULL,
                set_buffer_with_release != NULL,
                set_buffer_plain != NULL,
                on_complete != NULL);
        return (*env)->NewStringUTF(env, status);
    }

    surface_control = surface_control_from_java(env, surface_control_object);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (surface_control == NULL) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported surfacecontrol-from-java");
        return (*env)->NewStringUTF(env, status);
    }
    transaction = ASurfaceTransaction_create();
    if (transaction == NULL) {
        ASurfaceControl_release(surface_control);
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present error transaction-create");
        return (*env)->NewStringUTF(env, status);
    }

    tmp_dir = (*env)->GetStringUTFChars(env, tmp_dir_string, NULL);
    hook_lib_dir = (*env)->GetStringUTFChars(env, hook_lib_dir_string, NULL);
    driver_dir = (*env)->GetStringUTFChars(env, driver_dir_string, NULL);
    driver_name = (*env)->GetStringUTFChars(env, driver_name_string, NULL);
    if (tmp_dir == NULL || hook_lib_dir == NULL || driver_dir == NULL || driver_name == NULL) {
        if (tmp_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
        }
        if (hook_lib_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
        }
        if (driver_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
        }
        if (driver_name != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
        }
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present unsupported loader-string-oom");
        return (*env)->NewStringUTF(env, status);
    }

    pthread_mutex_lock(&g_ahb_vk_mutex);
    int configure_result = configure_ahb_vk_ring_locked(
            (int)target_width,
            (int)target_height,
            tmp_dir,
            hook_lib_dir,
            driver_dir,
            driver_name,
            status,
            sizeof(status));
    (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
    (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
    (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
    (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
    if (configure_result != 0) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        return (*env)->NewStringUTF(env, status);
    }

    slot_index = find_free_ahb_vk_slot_with_wait_locked(&slot_wait_us);
    if (slot_index < 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: dmabuf-vk-native-present ring-busy %d/%d gen %lld slot-wait %lldus",
                AHB_CPU_RING_SIZE,
                AHB_CPU_RING_SIZE,
                g_ahb_vk_renderer.generation,
                slot_wait_us);
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        return (*env)->NewStringUTF(env, status);
    }

    AhbVkSlot *slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_RENDERING;
    generation = g_ahb_vk_renderer.generation;

    if (render_dmabuf_to_ahb_vk_slot(
                slot_index,
                (long long)frame_index,
                (int)dmabuf_fd,
                (int)source_width,
                (int)source_height,
                (uint32_t)drm_format,
                (uint64_t)modifier,
                (int)planes,
                (uint64_t)stride0,
                (uint64_t)offset0,
                (uint64_t)size,
                status,
                sizeof(status),
                &submit_us,
                1,
                &acquire_fence_fd) != 0) {
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state == AHB_VK_SLOT_RENDERING) {
            destroy_slot_pending_source_locked(&g_ahb_vk_renderer.slots[slot_index]);
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        if (acquire_fence_fd >= 0) {
            close(acquire_fence_fd);
        }
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        return (*env)->NewStringUTF(env, status);
    }

    if (g_ahb_vk_renderer.generation != generation
            || g_ahb_vk_renderer.slots[slot_index].state != AHB_VK_SLOT_RENDERING) {
        destroy_slot_pending_source_locked(&g_ahb_vk_renderer.slots[slot_index]);
        g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
        pthread_cond_broadcast(&g_ahb_vk_cond);
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        if (acquire_fence_fd >= 0) {
            close(acquire_fence_fd);
        }
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present error stale-render slot %d",
                slot_index);
        return (*env)->NewStringUTF(env, status);
    }

    if (acquire_fence_fd >= 0) {
        source_wait_fence_fd = dup(acquire_fence_fd);
        if (source_wait_fence_fd < 0) {
            int dup_errno = errno;
            destroy_slot_pending_source_locked(&g_ahb_vk_renderer.slots[slot_index]);
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
            pthread_mutex_unlock(&g_ahb_vk_mutex);
            close(acquire_fence_fd);
            ASurfaceTransaction_delete(transaction);
            ASurfaceControl_release(surface_control);
            snprintf(status, sizeof(status),
                    "producer: dmabuf-vk-native-present error source-fence-dup errno %d slot %d",
                    dup_errno,
                    slot_index);
            return (*env)->NewStringUTF(env, status);
        }
    }

    release_context = (NativeAhbVkReleaseContext *)calloc(1, sizeof(*release_context));
    if (release_context == NULL) {
        destroy_slot_pending_source_locked(&g_ahb_vk_renderer.slots[slot_index]);
        g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
        pthread_cond_broadcast(&g_ahb_vk_cond);
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        if (acquire_fence_fd >= 0) {
            close(acquire_fence_fd);
        }
        if (source_wait_fence_fd >= 0) {
            close(source_wait_fence_fd);
        }
        ASurfaceTransaction_delete(transaction);
        ASurfaceControl_release(surface_control);
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk-native-present error release-context-oom slot %d",
                slot_index);
        return (*env)->NewStringUTF(env, status);
    }
    release_context->slot_index = slot_index;
    release_context->generation = generation;

    slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL;
    slot->last_frame_index = (long long)frame_index;
    slot->generation = generation;
    g_ahb_vk_renderer.next_slot = (slot_index + 1) % AHB_CPU_RING_SIZE;

    ARect crop = {
            .left = 0,
            .top = 0,
            .right = (int32_t)target_width,
            .bottom = (int32_t)target_height,
    };
    if (set_buffer_with_release != NULL) {
        set_buffer_with_release(
                transaction,
                surface_control,
                slot->buffer,
                acquire_fence_fd,
                release_context,
                native_ahb_vk_buffer_release);
    } else {
        /* Android 14 path: attach the buffer, then use present completion as release. */
        set_buffer_plain(
                transaction,
                surface_control,
                slot->buffer,
                acquire_fence_fd);
        on_complete(transaction, (void *)release_context, native_ahb_vk_present_complete);
    }
    acquire_fence_fd = -1;
    release_context = NULL;
    ASurfaceTransaction_setZOrder(transaction, surface_control, 10);
    ASurfaceTransaction_setVisibility(
            transaction,
            surface_control,
            ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    ASurfaceTransaction_setBufferTransparency(
            transaction,
            surface_control,
            ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
    ASurfaceTransaction_setBufferAlpha(
            transaction,
            surface_control,
            WAYLANDIE_PRESENT_COMPOSITION_NUDGE_ALPHA);
    ASurfaceTransaction_setPosition(transaction, surface_control, 0, 0);
    ASurfaceTransaction_setCrop(transaction, surface_control, &crop);
    ASurfaceTransaction_setDamageRegion(transaction, surface_control, &crop, 1);

    if (slot_wait_us > 0) {
        char status_with_slot_wait[1024];
        snprintf(status_with_slot_wait, sizeof(status_with_slot_wait),
                "%s slot-wait %lldus", status, slot_wait_us);
        snprintf(status, sizeof(status), "%s", status_with_slot_wait);
    }
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    ASurfaceTransaction_apply(transaction);
    /*
     * SurfaceControl owns the acquire fence after setBufferWithRelease(); waiting on
     * our duplicate here serializes the producer thread and can miss 144 Hz slots.
     */
    if (source_wait_fence_fd >= 0) {
        close(source_wait_fence_fd);
        source_wait_fence_fd = -1;
    }
    ASurfaceTransaction_delete(transaction);
    ASurfaceControl_release(surface_control);
    return (*env)->NewStringUTF(env, status);
}

JNIEXPORT jobject JNICALL
Java_io_waylandie_display_MainActivity_nativeAcquireAhbVkDmaBufFrame(
        JNIEnv *env,
        jclass clazz,
        jint dmabuf_fd,
        jint source_width,
        jint source_height,
        jlong drm_format,
        jlong modifier,
        jint planes,
        jlong stride0,
        jlong offset0,
        jlong size,
        jint target_width,
        jint target_height,
        jlong frame_index,
        jstring tmp_dir_string,
        jstring hook_lib_dir_string,
        jstring driver_dir_string,
        jstring driver_name_string) {
    (void)clazz;

    char status[512];
    jobject hardware_buffer = NULL;
    const char *tmp_dir = NULL;
    const char *hook_lib_dir = NULL;
    const char *driver_dir = NULL;
    const char *driver_name = NULL;
    int slot_index = -1;
    long long generation = 0;
    long long wait_us = 0;
    long long slot_wait_us = 0;

    if (dmabuf_fd < 0
            || source_width <= 0
            || source_height <= 0
            || target_width <= 0
            || target_height <= 0
            || drm_format < 0
            || drm_format > UINT32_MAX
            || modifier < 0
            || planes != 1
            || stride0 <= 0
            || offset0 < 0
            || size <= 0) {
        snprintf(status, sizeof(status),
                "producer: dmabuf-vk unsupported bad-meta source %dx%d target %dx%d planes %d",
                (int)source_width,
                (int)source_height,
                (int)target_width,
                (int)target_height,
                (int)planes);
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }
    if (tmp_dir_string == NULL
            || hook_lib_dir_string == NULL
            || driver_dir_string == NULL
            || driver_name_string == NULL) {
        snprintf(status, sizeof(status), "producer: dmabuf-vk unsupported missing-loader-strings");
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }

    tmp_dir = (*env)->GetStringUTFChars(env, tmp_dir_string, NULL);
    hook_lib_dir = (*env)->GetStringUTFChars(env, hook_lib_dir_string, NULL);
    driver_dir = (*env)->GetStringUTFChars(env, driver_dir_string, NULL);
    driver_name = (*env)->GetStringUTFChars(env, driver_name_string, NULL);
    if (tmp_dir == NULL || hook_lib_dir == NULL || driver_dir == NULL || driver_name == NULL) {
        if (tmp_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
        }
        if (hook_lib_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
        }
        if (driver_dir != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
        }
        if (driver_name != NULL) {
            (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
        }
        snprintf(status, sizeof(status), "producer: dmabuf-vk unsupported loader-string-oom");
        return make_ahb_cpu_frame(env, NULL, -1, 0, status);
    }

    pthread_mutex_lock(&g_ahb_vk_mutex);
    int configure_result = configure_ahb_vk_ring_locked(
            (int)target_width,
            (int)target_height,
            tmp_dir,
            hook_lib_dir,
            driver_dir,
            driver_name,
            status,
            sizeof(status));
    (*env)->ReleaseStringUTFChars(env, tmp_dir_string, tmp_dir);
    (*env)->ReleaseStringUTFChars(env, hook_lib_dir_string, hook_lib_dir);
    (*env)->ReleaseStringUTFChars(env, driver_dir_string, driver_dir);
    (*env)->ReleaseStringUTFChars(env, driver_name_string, driver_name);
    if (configure_result != 0) {
        generation = g_ahb_vk_renderer.generation;
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot_index = find_free_ahb_vk_slot_with_wait_locked(&slot_wait_us);
    if (slot_index < 0) {
        snprintf(
                status,
                sizeof(status),
                "producer: dmabuf-vk ring-busy %d/%d gen %lld slot-wait %lldus",
                AHB_CPU_RING_SIZE,
                AHB_CPU_RING_SIZE,
                g_ahb_vk_renderer.generation,
                slot_wait_us);
        generation = g_ahb_vk_renderer.generation;
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    AhbVkSlot *slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_RENDERING;
    generation = g_ahb_vk_renderer.generation;
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    pthread_mutex_lock(&g_ahb_vk_mutex);
    if (g_ahb_vk_renderer.generation != generation
            || g_ahb_vk_renderer.slots[slot_index].state != AHB_VK_SLOT_RENDERING) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        snprintf(status, sizeof(status), "producer: dmabuf-vk error stale-before-render slot %d",
                slot_index);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }
    if (render_dmabuf_to_ahb_vk_slot(
                slot_index,
                (long long)frame_index,
                (int)dmabuf_fd,
                (int)source_width,
                (int)source_height,
                (uint32_t)drm_format,
                (uint64_t)modifier,
                (int)planes,
                (uint64_t)stride0,
                (uint64_t)offset0,
                (uint64_t)size,
                status,
                sizeof(status),
                &wait_us,
                0,
                NULL) != 0) {
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state == AHB_VK_SLOT_RENDERING) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot = &g_ahb_vk_renderer.slots[slot_index];
    hardware_buffer = AHardwareBuffer_toHardwareBuffer(env, slot->buffer);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    if (hardware_buffer == NULL) {
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state == AHB_VK_SLOT_RENDERING) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        snprintf(status, sizeof(status), "producer: dmabuf-vk error wrap-failed slot %d",
                slot_index);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    if (g_ahb_vk_renderer.generation != generation
            || g_ahb_vk_renderer.slots[slot_index].state != AHB_VK_SLOT_RENDERING) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        (*env)->DeleteLocalRef(env, hardware_buffer);
        snprintf(status, sizeof(status), "producer: dmabuf-vk error stale-render slot %d",
                slot_index);
        return make_ahb_cpu_frame(env, NULL, -1, (jlong)generation, status);
    }

    slot = &g_ahb_vk_renderer.slots[slot_index];
    slot->state = AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL;
    slot->last_frame_index = (long long)frame_index;
    slot->generation = generation;
    g_ahb_vk_renderer.next_slot = (slot_index + 1) % AHB_CPU_RING_SIZE;
    if (slot_wait_us > 0) {
        char status_with_slot_wait[512];
        snprintf(
                status_with_slot_wait,
                sizeof(status_with_slot_wait),
                "%s slot-wait %lldus",
                status,
                slot_wait_us);
        snprintf(status, sizeof(status), "%s", status_with_slot_wait);
    }
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    jobject frame = make_ahb_cpu_frame(
            env,
            hardware_buffer,
            (jint)slot_index,
            (jlong)generation,
            status);
    (*env)->DeleteLocalRef(env, hardware_buffer);
    if (frame == NULL || (*env)->ExceptionCheck(env)) {
        pthread_mutex_lock(&g_ahb_vk_mutex);
        if (slot_index >= 0
                && slot_index < AHB_CPU_RING_SIZE
                && g_ahb_vk_renderer.generation == generation
                && g_ahb_vk_renderer.slots[slot_index].state
                        == AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL) {
            g_ahb_vk_renderer.slots[slot_index].state = AHB_VK_SLOT_FREE;
            pthread_cond_broadcast(&g_ahb_vk_cond);
        }
        pthread_mutex_unlock(&g_ahb_vk_mutex);
    }
    return frame;
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeReleaseAhbCpuSlot(
        JNIEnv *env,
        jclass clazz,
        jint slot_index,
        jlong generation) {
    (void)clazz;

    pthread_mutex_lock(&g_ahb_cpu_mutex);

    if (generation != (jlong)g_ahb_cpu_generation) {
        long long current_generation = g_ahb_cpu_generation;
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(
                env,
                "ahb-cpu release stale slot %d gen %lld current %lld",
                (int)slot_index,
                (long long)generation,
                current_generation);
    }

    if (slot_index < 0 || slot_index >= AHB_CPU_RING_SIZE) {
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(env, "ahb-cpu release invalid slot %d", (int)slot_index);
    }

    AhbCpuSlot *slot = &g_ahb_cpu_slots[slot_index];
    if (!slot->in_use_by_surface_control) {
        pthread_mutex_unlock(&g_ahb_cpu_mutex);
        return make_status(env, "ahb-cpu release duplicate slot %d", (int)slot_index);
    }

    long long frame_index = slot->last_frame_index;
    slot->in_use_by_surface_control = 0;
    pthread_mutex_unlock(&g_ahb_cpu_mutex);

    return make_status(
            env,
            "ahb-cpu release slot %d frame %lld",
            (int)slot_index,
            frame_index);
}

JNIEXPORT jstring JNICALL
Java_io_waylandie_display_MainActivity_nativeReleaseAhbVkSlot(
        JNIEnv *env,
        jclass clazz,
        jint slot_index,
        jlong generation) {
    (void)clazz;

    pthread_mutex_lock(&g_ahb_vk_mutex);

    if (generation != (jlong)g_ahb_vk_renderer.generation) {
        long long current_generation = g_ahb_vk_renderer.generation;
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_status(
                env,
                "ahb-vk release stale slot %d gen %lld current %lld",
                (int)slot_index,
                (long long)generation,
                current_generation);
    }

    if (slot_index < 0 || slot_index >= AHB_CPU_RING_SIZE) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_status(env, "ahb-vk release invalid slot %d", (int)slot_index);
    }

    AhbVkSlot *slot = &g_ahb_vk_renderer.slots[slot_index];
    if (slot->state != AHB_VK_SLOT_IN_USE_BY_SURFACE_CONTROL) {
        pthread_mutex_unlock(&g_ahb_vk_mutex);
        return make_status(env, "ahb-vk release duplicate slot %d", (int)slot_index);
    }

    long long frame_index = slot->last_frame_index;
    destroy_slot_pending_source_locked(slot);
    slot->state = AHB_VK_SLOT_FREE;
    pthread_cond_broadcast(&g_ahb_vk_cond);
    pthread_mutex_unlock(&g_ahb_vk_mutex);

    return make_status(
            env,
            "ahb-vk release slot %d frame %lld",
            (int)slot_index,
            frame_index);
}

JNIEXPORT void JNICALL
Java_io_waylandie_display_MainActivity_nativeResetAhbCpuProducer(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;

    pthread_mutex_lock(&g_ahb_cpu_mutex);
    // Native can drop its reusable-ring references on teardown; any Java
    // wrappers already submitted to SurfaceControl hold their own references.
    reset_ahb_cpu_ring_locked();
    pthread_mutex_unlock(&g_ahb_cpu_mutex);
}

JNIEXPORT void JNICALL
Java_io_waylandie_display_MainActivity_nativeResetAhbVkProducer(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;

    pthread_mutex_lock(&g_ahb_vk_mutex);
    reset_ahb_vk_renderer_locked();
    pthread_mutex_unlock(&g_ahb_vk_mutex);
}
