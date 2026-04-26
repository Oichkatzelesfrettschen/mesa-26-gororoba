/*
 * terakan_barrier_interop_headless.c
 *
 * Headless compute->graphics interop barrier verification harness.
 *
 * Protocol:
 *   1) Compute writes fullscreen-quad index data into a storage buffer
 *      that is also bound as an index buffer.
 *   2) Issue vkCmdPipelineBarrier with
 *      COMPUTE_SHADER/SHADER_WRITE -> VERTEX_INPUT/INDEX_READ.
 *   3) Draw indexed quad using the compute-populated index buffer.
 *   4) Read back linear R8G8B8A8_UNORM color image and verify payload coverage.
 *   5) Map index buffer after completion and verify compute writes landed.
 *
 * Build:
 *   gcc -O2 -std=c11 -Wall -Wextra -o terakan_barrier_interop_headless \
 *       terakan_barrier_interop_headless.c -lvulkan -lm
 */

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct AppConfig {
    const char *comp_path;
    const char *vert_path;
    const char *frag_path;
    const char *dump_path;
    const char *hex_path;
    uint32_t width;
    uint32_t height;
    uint8_t magic[4];
    bool interpolated_validation;
    VkIndexType index_type;
} AppConfig;

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--comp path] [--vert path] [--frag path] [--dump rgba_path] [--hex txt_path]\n"
            "          [--width N] [--height N] [--validation magic|interpolated]\n"
            "          [--index-type uint16|uint32]\n",
            prog);
}

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0ul || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_magic_rgba(const char *text, uint8_t out[4]) {
    if (!text || strlen(text) != 8) {
        return -1;
    }
    for (size_t i = 0; i < 4; i++) {
        char pair[3] = {text[i * 2], text[i * 2 + 1], '\0'};
        char *end = NULL;
        unsigned long value = strtoul(pair, &end, 16);
        if (!end || *end != '\0' || value > 255ul) {
            return -1;
        }
        out[i] = (uint8_t)value;
    }
    return 0;
}

static int parse_validation_mode(const char *text, bool *interpolated_validation) {
    if (!text || !interpolated_validation) {
        return -1;
    }
    if (strcmp(text, "magic") == 0) {
        *interpolated_validation = false;
        return 0;
    }
    if (strcmp(text, "interpolated") == 0) {
        *interpolated_validation = true;
        return 0;
    }
    return -1;
}

static int parse_index_type(const char *text, VkIndexType *out) {
    if (!text || !out) {
        return -1;
    }
    if (strcmp(text, "uint16") == 0) {
        *out = VK_INDEX_TYPE_UINT16;
        return 0;
    }
    if (strcmp(text, "uint32") == 0) {
        *out = VK_INDEX_TYPE_UINT32;
        return 0;
    }
    return -1;
}

static int parse_args(int argc, char **argv, AppConfig *cfg) {
    cfg->comp_path = "barrier_interop.comp.spv";
    cfg->vert_path = "first_triangle.vert.spv";
    cfg->frag_path = "first_triangle.frag.spv";
    cfg->dump_path = NULL;
    cfg->hex_path = NULL;
    cfg->width = 256;
    cfg->height = 256;
    cfg->magic[0] = 0xDEu;
    cfg->magic[1] = 0xADu;
    cfg->magic[2] = 0xBEu;
    cfg->magic[3] = 0xEFu;
    cfg->interpolated_validation = false;
    cfg->index_type = VK_INDEX_TYPE_UINT16;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--comp") == 0 && i + 1 < argc) {
            cfg->comp_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--vert") == 0 && i + 1 < argc) {
            cfg->vert_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--frag") == 0 && i + 1 < argc) {
            cfg->frag_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            cfg->dump_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--hex") == 0 && i + 1 < argc) {
            cfg->hex_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->width) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->height) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--magic-rgba") == 0 && i + 1 < argc) {
            if (parse_magic_rgba(argv[++i], cfg->magic) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--validation") == 0 && i + 1 < argc) {
            if (parse_validation_mode(argv[++i], &cfg->interpolated_validation) != 0) {
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--index-type") == 0 && i + 1 < argc) {
            if (parse_index_type(argv[++i], &cfg->index_type) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

static uint32_t *read_spirv(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open SPIR-V: %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || (size % 4) != 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint32_t *buf = (uint32_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size_out = (size_t)size;
    return buf;
}

static uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties *props,
                                 uint32_t type_bits,
                                 VkMemoryPropertyFlags req_flags) {
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props->memoryTypes[i].propertyFlags & req_flags) == req_flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool env_flag_enabled(const char *name) {
    const char *v = getenv(name);
    if (!v || v[0] == '\0') {
        return false;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "FALSE") == 0 ||
        strcmp(v, "off") == 0 || strcmp(v, "OFF") == 0) {
        return false;
    }
    return true;
}

static uint64_t timespec_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static void emit_fence_trace(const char *event, VkResult vr) {
    struct timespec real_ts = {0};
    struct timespec mono_ts = {0};
    clock_gettime(CLOCK_REALTIME, &real_ts);
    clock_gettime(CLOCK_MONOTONIC, &mono_ts);
    printf("FENCE_TRACE event=%s realtime_ns=%" PRIu64 " monotonic_ns=%" PRIu64 " vkresult=%d\n",
           event,
           timespec_to_ns(&real_ts),
           timespec_to_ns(&mono_ts),
           vr);
}

static void emit_index_probe(VkIndexType index_type, const void *mapped_data, size_t byte_size) {
    if (!mapped_data || byte_size == 0) {
        return;
    }
    const uint32_t *words = (const uint32_t *)mapped_data;
    size_t word_count = byte_size / sizeof(uint32_t);

    if (index_type == VK_INDEX_TYPE_UINT32) {
        size_t seq_count = byte_size / sizeof(uint32_t);
        if (seq_count > 6) {
            seq_count = 6;
        }
        size_t print_words = word_count < 6 ? word_count : 6;
        printf("INDEX_PROBE_WORDS_U32=");
        for (size_t i = 0; i < print_words; i++) {
            printf("%s0x%08x", (i == 0) ? "" : ",", words[i]);
        }
        printf("\n");
        printf("INDEX_PROBE_SEQ_U32=");
        for (size_t i = 0; i < seq_count; i++) {
            printf("%s%u", (i == 0) ? "" : ",", words[i]);
        }
        printf("\n");
        return;
    }

    size_t packed_words = word_count < 3 ? word_count : 3;
    printf("INDEX_PROBE_WORDS_U16_PACKED=");
    for (size_t i = 0; i < packed_words; i++) {
        printf("%s0x%08x", (i == 0) ? "" : ",", words[i]);
    }
    printf("\n");
    const uint16_t *idx16 = (const uint16_t *)mapped_data;
    size_t idx_count = byte_size / sizeof(uint16_t);
    if (idx_count > 6) {
        idx_count = 6;
    }
    printf("INDEX_PROBE_SEQ_U16=");
    for (size_t i = 0; i < idx_count; i++) {
        printf("%s%u", (i == 0) ? "" : ",", idx16[i]);
    }
    printf("\n");
}

static void write_hex_preview(const char *path,
                              const uint8_t *pixels,
                              uint32_t width,
                              uint32_t height) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    uint32_t preview_rows = height < 8 ? height : 8;
    uint32_t preview_cols = width < 8 ? width : 8;
    for (uint32_t y = 0; y < preview_rows; y++) {
        fprintf(f, "row %u:", y);
        for (uint32_t x = 0; x < preview_cols; x++) {
            const uint8_t *px = pixels + ((size_t)y * width + x) * 4u;
            fprintf(f, " %02x%02x%02x%02x", px[0], px[1], px[2], px[3]);
        }
        fputc('\n', f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    AppConfig cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    VkResult vr = VK_SUCCESS;
    int exit_code = EXIT_FAILURE;
    const bool compute_only = env_flag_enabled("TERAKAN_BARRIER_COMPUTE_ONLY");
    const bool draw_only = env_flag_enabled("TERAKAN_BARRIER_DRAW_ONLY");
    const bool skip_compute_sync_barrier =
        env_flag_enabled("BARRIER_INTEROP_SKIP_COMPUTE_SYNC_BARRIER");
    const bool strong_compute_index_barrier =
        env_flag_enabled("BARRIER_INTEROP_STRONG_COMPUTE_INDEX_BARRIER");
    const bool split_submit = env_flag_enabled("BARRIER_INTEROP_SPLIT_SUBMIT");
    const bool two_cmdb_single_submit =
        env_flag_enabled("BARRIER_INTEROP_TWO_CMDB_SINGLE_SUBMIT");
    const bool emulate_cmdb_boundary =
        env_flag_enabled("BARRIER_INTEROP_EMULATE_CMDB_BOUNDARY");
    const bool use_split_submit = split_submit && !compute_only && !draw_only;
    const bool use_two_cmdb_single_submit =
        two_cmdb_single_submit && !compute_only && !draw_only;
    const bool use_dual_cmd_buffers = use_split_submit || use_two_cmdb_single_submit;
    const bool use_cmdb_boundary_emulation =
        emulate_cmdb_boundary && !compute_only && !draw_only && !use_dual_cmd_buffers;
    static const uint16_t expected_idx16[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t expected_idx32[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t expected_idx16_packed[3] = {
        0x00010000u,
        0x00020002u,
        0x00030001u,
    };

    if (compute_only && draw_only) {
        fprintf(stderr, "TERAKAN_BARRIER_COMPUTE_ONLY and TERAKAN_BARRIER_DRAW_ONLY are mutually exclusive\n");
        return EXIT_FAILURE;
    }
    if (use_split_submit && use_two_cmdb_single_submit) {
        fprintf(stderr, "BARRIER_INTEROP_SPLIT_SUBMIT and BARRIER_INTEROP_TWO_CMDB_SINGLE_SUBMIT are mutually exclusive\n");
        return EXIT_FAILURE;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool compute_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet compute_desc_set = VK_NULL_HANDLE;
    VkShaderModule cs_module = VK_NULL_HANDLE;
    VkShaderModule vs_module = VK_NULL_HANDLE;
    VkShaderModule fs_module = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory index_mem = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t cmd_buffer_count = 0;
    VkFence submit_fence = VK_NULL_HANDLE;

    void *mapped = NULL;
    uint8_t *contiguous = NULL;

    uint32_t queue_family = UINT32_MAX;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vr = vkCreateInstance(&ici, NULL, &instance);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", vr);
        goto cleanup;
    }

    uint32_t pd_count = 0;
    vr = vkEnumeratePhysicalDevices(instance, &pd_count, NULL);
    if (vr != VK_SUCCESS || pd_count == 0) {
        fprintf(stderr, "vkEnumeratePhysicalDevices failed: %d (count=%u)\n", vr, pd_count);
        goto cleanup;
    }
    VkPhysicalDevice *devices = (VkPhysicalDevice *)calloc(pd_count, sizeof(*devices));
    if (!devices) {
        goto cleanup;
    }
    vr = vkEnumeratePhysicalDevices(instance, &pd_count, devices);
    if (vr != VK_SUCCESS) {
        free(devices);
        fprintf(stderr, "vkEnumeratePhysicalDevices(list) failed: %d\n", vr);
        goto cleanup;
    }
    phys = devices[0];
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("DEVICE_NAME=%s\n", props.deviceName);
    printf("API_VERSION=%u.%u.%u\n",
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));
    printf("VALIDATION_MODE=%s\n",
           cfg.interpolated_validation ? "interpolated" : "magic");
    printf("COMPUTE_SHADER=%s\n", cfg.comp_path);
    printf("BARRIER_PATH=COMPUTE_SHADER_WRITE->INDEX_READ\n");
    printf("INDEX_TYPE=%s\n", cfg.index_type == VK_INDEX_TYPE_UINT32 ? "UINT32" : "UINT16");
    printf("MODE=%s\n", compute_only ? "compute_only" : (draw_only ? "draw_only" : "full"));
    const char *submit_mode = "single";
    if (use_split_submit) {
        submit_mode = "split";
    } else if (use_two_cmdb_single_submit) {
        submit_mode = "single_submit_two_cmdb";
    }
    printf("SUBMIT_MODE=%s\n", submit_mode);
    printf("COMPUTE_SYNC_BARRIER=%s\n", skip_compute_sync_barrier ? "SKIPPED" : "ENABLED");
    printf("COMPUTE_INDEX_BARRIER_MODE=%s\n",
           strong_compute_index_barrier ? "STRONG" : "DEFAULT");
    printf("CMDB_BOUNDARY_EMULATION=%s\n",
           use_cmdb_boundary_emulation ? "ENABLED" : "DISABLED");
    if (!cfg.interpolated_validation) {
        printf("EXPECTED_RGBA=%02x,%02x,%02x,%02x\n",
               cfg.magic[0], cfg.magic[1], cfg.magic[2], cfg.magic[3]);
    }

    VkFormatProperties fmt_props;
    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &fmt_props);
    printf("LINEAR_TILING_FEATURES=0x%08x\n", fmt_props.linearTilingFeatures);
    if ((fmt_props.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
        fprintf(stderr, "R8G8B8A8_UNORM linear tiling is not color-attachment capable\n");
        goto cleanup;
    }

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, NULL);
    VkQueueFamilyProperties *qf_props =
        (VkQueueFamilyProperties *)calloc(qf_count, sizeof(*qf_props));
    if (!qf_props) {
        goto cleanup;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qf_count, qf_props);
    for (uint32_t i = 0; i < qf_count; i++) {
        if ((qf_props[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queue_family = i;
            break;
        }
    }
    free(qf_props);
    if (queue_family == UINT32_MAX) {
        fprintf(stderr, "No graphics+compute queue family found\n");
        goto cleanup;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    vr = vkCreateDevice(phys, &dci, NULL, &device);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed: %d\n", vr);
        goto cleanup;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkImageCreateInfo imci = {0};
    imci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imci.imageType = VK_IMAGE_TYPE_2D;
    imci.format = VK_FORMAT_R8G8B8A8_UNORM;
    imci.extent.width = cfg.width;
    imci.extent.height = cfg.height;
    imci.extent.depth = 1;
    imci.mipLevels = 1;
    imci.arrayLayers = 1;
    imci.samples = VK_SAMPLE_COUNT_1_BIT;
    imci.tiling = VK_IMAGE_TILING_LINEAR;
    imci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vr = vkCreateImage(device, &imci, NULL, &image);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImage(linear color) failed: %d\n", vr);
        goto cleanup;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, image, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    uint32_t mem_type = find_memory_type(
        &mem_props,
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "No HOST_VISIBLE|HOST_COHERENT memory type for linear color image\n");
        goto cleanup;
    }

    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex = mem_type;
    vr = vkAllocateMemory(device, &mai, NULL, &image_mem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed: %d\n", vr);
        goto cleanup;
    }

    vr = vkBindImageMemory(device, image, image_mem, 0);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkBindImageMemory failed: %d\n", vr);
        goto cleanup;
    }

    VkBufferCreateInfo ib_ci = {0};
    ib_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ib_ci.size = (cfg.index_type == VK_INDEX_TYPE_UINT32 ? sizeof(uint32_t) : sizeof(uint16_t)) * 6u;
    ib_ci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!draw_only) {
        ib_ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    ib_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vr = vkCreateBuffer(device, &ib_ci, NULL, &index_buffer);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateBuffer(index) failed: %d\n", vr);
        goto cleanup;
    }

    VkMemoryRequirements ib_mem_req;
    vkGetBufferMemoryRequirements(device, index_buffer, &ib_mem_req);
    uint32_t ib_mem_type = UINT32_MAX;
    if (!draw_only) {
        ib_mem_type = find_memory_type(
            &mem_props,
            ib_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ib_mem_type == UINT32_MAX) {
            ib_mem_type = find_memory_type(
                &mem_props,
                ib_mem_req.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        }
    }
    if (ib_mem_type == UINT32_MAX) {
        ib_mem_type = find_memory_type(
            &mem_props,
            ib_mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (ib_mem_type == UINT32_MAX) {
        fprintf(stderr, "No HOST_VISIBLE|HOST_COHERENT memory type for index buffer\n");
        goto cleanup;
    }
    printf("INDEX_MEM_TYPE=%u\n", ib_mem_type);
    printf("INDEX_MEM_FLAGS=0x%08x\n", mem_props.memoryTypes[ib_mem_type].propertyFlags);

    VkMemoryAllocateInfo ib_ai = {0};
    ib_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ib_ai.allocationSize = ib_mem_req.size;
    ib_ai.memoryTypeIndex = ib_mem_type;
    vr = vkAllocateMemory(device, &ib_ai, NULL, &index_mem);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory(index) failed: %d\n", vr);
        goto cleanup;
    }
    vr = vkBindBufferMemory(device, index_buffer, index_mem, 0);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkBindBufferMemory(index) failed: %d\n", vr);
        goto cleanup;
    }

    {
        void *ib_map = NULL;
        vr = vkMapMemory(device, index_mem, 0, ib_ci.size, 0, &ib_map);
        if (vr != VK_SUCCESS || ib_map == NULL) {
            fprintf(stderr, "vkMapMemory(index) failed: %d\n", vr);
            goto cleanup;
        }
        memset(ib_map, 0, (size_t)ib_ci.size);
        if (cfg.index_type == VK_INDEX_TYPE_UINT32) {
            memcpy(ib_map, expected_idx32, sizeof(expected_idx32));
        } else {
            memcpy(ib_map, expected_idx16, sizeof(expected_idx16));
        }
        vkUnmapMemory(device, index_mem);
    }

    VkImageViewCreateInfo ivci = {0};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;
    vr = vkCreateImageView(device, &ivci, NULL, &image_view);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateImageView failed: %d\n", vr);
        goto cleanup;
    }

    VkAttachmentDescription color_attach = {0};
    color_attach.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attach.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attach.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_ref = {0};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = {0};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color_attach;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    vr = vkCreateRenderPass(device, &rpci, NULL, &render_pass);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateRenderPass failed: %d\n", vr);
        goto cleanup;
    }

    VkFramebufferCreateInfo fbci = {0};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = render_pass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &image_view;
    fbci.width = cfg.width;
    fbci.height = cfg.height;
    fbci.layers = 1;
    vr = vkCreateFramebuffer(device, &fbci, NULL, &framebuffer);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateFramebuffer failed: %d\n", vr);
        goto cleanup;
    }

    size_t cs_size = 0;
    size_t vs_size = 0;
    size_t fs_size = 0;
    uint32_t *cs_code = NULL;
    if (!draw_only) {
        cs_code = read_spirv(cfg.comp_path, &cs_size);
    }
    uint32_t *vs_code = read_spirv(cfg.vert_path, &vs_size);
    uint32_t *fs_code = read_spirv(cfg.frag_path, &fs_size);
    if ((!draw_only && !cs_code) || !vs_code || !fs_code) {
        fprintf(stderr, "Failed to read shaders: comp=%s vert=%s frag=%s\n",
                cfg.comp_path, cfg.vert_path, cfg.frag_path);
        free(cs_code);
        free(vs_code);
        free(fs_code);
        goto cleanup;
    }

    VkShaderModuleCreateInfo smci = {0};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    if (!draw_only) {
        smci.codeSize = cs_size;
        smci.pCode = cs_code;
        vr = vkCreateShaderModule(device, &smci, NULL, &cs_module);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkCreateShaderModule(cs) failed: %d\n", vr);
            free(cs_code);
            free(vs_code);
            free(fs_code);
            goto cleanup;
        }
    }
    smci.codeSize = vs_size;
    smci.pCode = vs_code;
    vr = vkCreateShaderModule(device, &smci, NULL, &vs_module);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule(vs) failed: %d\n", vr);
        free(cs_code);
        free(vs_code);
        free(fs_code);
        goto cleanup;
    }
    smci.codeSize = fs_size;
    smci.pCode = fs_code;
    vr = vkCreateShaderModule(device, &smci, NULL, &fs_module);
    free(cs_code);
    free(vs_code);
    free(fs_code);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule(fs) failed: %d\n", vr);
        goto cleanup;
    }

    if (!draw_only) {
        VkDescriptorSetLayoutBinding compute_binding = {0};
        compute_binding.binding = 0;
        compute_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_binding.descriptorCount = 1;
        compute_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ds_layout_ci = {0};
        ds_layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_layout_ci.bindingCount = 1;
        ds_layout_ci.pBindings = &compute_binding;
        vr = vkCreateDescriptorSetLayout(device, &ds_layout_ci, NULL, &compute_set_layout);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkCreateDescriptorSetLayout(compute) failed: %d\n", vr);
            goto cleanup;
        }

        VkPipelineLayoutCreateInfo compute_plci = {0};
        compute_plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        compute_plci.setLayoutCount = 1;
        compute_plci.pSetLayouts = &compute_set_layout;
        vr = vkCreatePipelineLayout(device, &compute_plci, NULL, &compute_pipeline_layout);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkCreatePipelineLayout(compute) failed: %d\n", vr);
            goto cleanup;
        }

        VkComputePipelineCreateInfo cpipeline_ci = {0};
        cpipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpipeline_ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpipeline_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpipeline_ci.stage.module = cs_module;
        cpipeline_ci.stage.pName = "main";
        cpipeline_ci.layout = compute_pipeline_layout;
        vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpipeline_ci, NULL, &compute_pipeline);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkCreateComputePipelines failed: %d\n", vr);
            goto cleanup;
        }

        VkDescriptorPoolSize compute_pool_size = {0};
        compute_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_pool_size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo compute_pool_ci = {0};
        compute_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        compute_pool_ci.maxSets = 1;
        compute_pool_ci.poolSizeCount = 1;
        compute_pool_ci.pPoolSizes = &compute_pool_size;
        vr = vkCreateDescriptorPool(device, &compute_pool_ci, NULL, &compute_desc_pool);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkCreateDescriptorPool(compute) failed: %d\n", vr);
            goto cleanup;
        }

        VkDescriptorSetAllocateInfo compute_ds_ai = {0};
        compute_ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        compute_ds_ai.descriptorPool = compute_desc_pool;
        compute_ds_ai.descriptorSetCount = 1;
        compute_ds_ai.pSetLayouts = &compute_set_layout;
        vr = vkAllocateDescriptorSets(device, &compute_ds_ai, &compute_desc_set);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkAllocateDescriptorSets(compute) failed: %d\n", vr);
            goto cleanup;
        }

        VkDescriptorBufferInfo compute_vertex_info = {0};
        compute_vertex_info.buffer = index_buffer;
        compute_vertex_info.offset = 0;
        compute_vertex_info.range = ib_ci.size;
        VkWriteDescriptorSet compute_write = {0};
        compute_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        compute_write.dstSet = compute_desc_set;
        compute_write.dstBinding = 0;
        compute_write.descriptorCount = 1;
        compute_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        compute_write.pBufferInfo = &compute_vertex_info;
        vkUpdateDescriptorSets(device, 1, &compute_write, 0, NULL);
    }

    VkPipelineShaderStageCreateInfo stages[2] = {{0}, {0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineLayoutCreateInfo plci = {0};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    vr = vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreatePipelineLayout failed: %d\n", vr);
        goto cleanup;
    }

    VkGraphicsPipelineCreateInfo gpci = {0};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    gpci.subpass = 0;
    vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed: %d\n", vr);
        goto cleanup;
    }

    VkCommandPoolCreateInfo cpci = {0};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = queue_family;
    vr = vkCreateCommandPool(device, &cpci, NULL, &cmd_pool);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateCommandPool failed: %d\n", vr);
        goto cleanup;
    }

    VkCommandBufferAllocateInfo cbai = {0};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = use_dual_cmd_buffers ? 2u : 1u;
    vr = vkAllocateCommandBuffers(device, &cbai, cmd_buffers);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateCommandBuffers failed: %d\n", vr);
        goto cleanup;
    }
    cmd_buffer_count = cbai.commandBufferCount;
    VkCommandBuffer cmd = cmd_buffers[0];
    VkCommandBuffer split_draw_cmd = use_dual_cmd_buffers ? cmd_buffers[1] : VK_NULL_HANDLE;

    VkClearValue clear;
    memset(&clear, 0, sizeof(clear));
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp_begin = {0};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.offset.x = 0;
    rp_begin.renderArea.offset.y = 0;
    rp_begin.renderArea.extent.width = cfg.width;
    rp_begin.renderArea.extent.height = cfg.height;
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear;

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (use_dual_cmd_buffers) {
        vr = vkBeginCommandBuffer(cmd, &begin);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkBeginCommandBuffer (compute) failed: %d\n", vr);
            goto cleanup;
        }

        VkPipelineStageFlags barrier_src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkAccessFlags barrier_src_access = VK_ACCESS_SHADER_WRITE_BIT;
        VkPipelineStageFlags barrier_dst_stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        VkAccessFlags barrier_dst_access = VK_ACCESS_INDEX_READ_BIT;
        if (strong_compute_index_barrier) {
            barrier_src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            barrier_src_access = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier_dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            barrier_dst_access = VK_ACCESS_MEMORY_READ_BIT;
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
        vkCmdBindDescriptorSets(cmd,
                                VK_PIPELINE_BIND_POINT_COMPUTE,
                                compute_pipeline_layout,
                                0,
                                1,
                                &compute_desc_set,
                                0,
                                NULL);
        vkCmdDispatch(cmd, 1, 1, 1);
        if (!skip_compute_sync_barrier) {
            VkBufferMemoryBarrier compute_barrier = {0};
            compute_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            compute_barrier.srcAccessMask = barrier_src_access;
            compute_barrier.dstAccessMask = barrier_dst_access;
            compute_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            compute_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            compute_barrier.buffer = index_buffer;
            compute_barrier.offset = 0;
            compute_barrier.size = ib_ci.size;
            vkCmdPipelineBarrier(cmd,
                                 barrier_src_stage,
                                 barrier_dst_stage,
                                 0,
                                 0, NULL,
                                 1, &compute_barrier,
                                 0, NULL);
        }
        vr = vkEndCommandBuffer(cmd);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkEndCommandBuffer (compute) failed: %d\n", vr);
            goto cleanup;
        }

        vr = vkBeginCommandBuffer(split_draw_cmd, &begin);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkBeginCommandBuffer (draw) failed: %d\n", vr);
            goto cleanup;
        }
        vkCmdBeginRenderPass(split_draw_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(split_draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkViewport viewport = {0};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)cfg.width;
        viewport.height = (float)cfg.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(split_draw_cmd, 0, 1, &viewport);

        VkRect2D scissor;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = cfg.width;
        scissor.extent.height = cfg.height;
        vkCmdSetScissor(split_draw_cmd, 0, 1, &scissor);

        vkCmdBindIndexBuffer(split_draw_cmd, index_buffer, 0, cfg.index_type);
        vkCmdDrawIndexed(split_draw_cmd, 6, 1, 0, 0, 0);
        vkCmdEndRenderPass(split_draw_cmd);

        VkImageMemoryBarrier host_barrier = {0};
        host_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        host_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.image = image;
        host_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        host_barrier.subresourceRange.baseMipLevel = 0;
        host_barrier.subresourceRange.levelCount = 1;
        host_barrier.subresourceRange.baseArrayLayer = 0;
        host_barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(split_draw_cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             0, NULL,
                             0, NULL,
                             1, &host_barrier);
        vr = vkEndCommandBuffer(split_draw_cmd);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkEndCommandBuffer (draw) failed: %d\n", vr);
            goto cleanup;
        }
    } else {
        vr = vkBeginCommandBuffer(cmd, &begin);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkBeginCommandBuffer failed: %d\n", vr);
            goto cleanup;
        }

        if (!draw_only) {
            VkPipelineStageFlags barrier_src_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            VkAccessFlags barrier_src_access = VK_ACCESS_SHADER_WRITE_BIT;
            VkPipelineStageFlags barrier_dst_stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            VkAccessFlags barrier_dst_access = VK_ACCESS_INDEX_READ_BIT;
            if (strong_compute_index_barrier) {
                barrier_src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier_src_access = VK_ACCESS_MEMORY_WRITE_BIT;
                barrier_dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier_dst_access = VK_ACCESS_MEMORY_READ_BIT;
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
            vkCmdBindDescriptorSets(cmd,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    compute_pipeline_layout,
                                    0,
                                    1,
                                    &compute_desc_set,
                                    0,
                                    NULL);
            vkCmdDispatch(cmd, 1, 1, 1);
            if (!skip_compute_sync_barrier) {
                VkBufferMemoryBarrier compute_barrier = {0};
                compute_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                compute_barrier.srcAccessMask = barrier_src_access;
                compute_barrier.dstAccessMask = barrier_dst_access;
                compute_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                compute_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                compute_barrier.buffer = index_buffer;
                compute_barrier.offset = 0;
                compute_barrier.size = ib_ci.size;
                vkCmdPipelineBarrier(cmd,
                                     barrier_src_stage,
                                     barrier_dst_stage,
                                     0,
                                     0, NULL,
                                     1, &compute_barrier,
                                     0, NULL);
            }
            if (use_cmdb_boundary_emulation) {
                VkMemoryBarrier full_mem_barrier = {0};
                full_mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                full_mem_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                full_mem_barrier.dstAccessMask =
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                vkCmdPipelineBarrier(cmd,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     0,
                                     1, &full_mem_barrier,
                                     0, NULL,
                                     0, NULL);
            }
        }

        if (!compute_only) {
            vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            VkViewport viewport = {0};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = (float)cfg.width;
            viewport.height = (float)cfg.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor;
            scissor.offset.x = 0;
            scissor.offset.y = 0;
            scissor.extent.width = cfg.width;
            scissor.extent.height = cfg.height;
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindIndexBuffer(cmd, index_buffer, 0, cfg.index_type);
            vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
            vkCmdEndRenderPass(cmd);

            VkImageMemoryBarrier host_barrier = {0};
            host_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            host_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            host_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.image = image;
            host_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            host_barrier.subresourceRange.baseMipLevel = 0;
            host_barrier.subresourceRange.levelCount = 1;
            host_barrier.subresourceRange.baseArrayLayer = 0;
            host_barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT,
                                 0,
                                 0, NULL,
                                 0, NULL,
                                 1, &host_barrier);
        }

        vr = vkEndCommandBuffer(cmd);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkEndCommandBuffer failed: %d\n", vr);
            goto cleanup;
        }
    }

    VkFenceCreateInfo fence_ci = {0};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vr = vkCreateFence(device, &fence_ci, NULL, &submit_fence);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkCreateFence failed: %d\n", vr);
        goto cleanup;
    }
    if (use_split_submit) {
        VkSubmitInfo compute_submit = {0};
        compute_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        compute_submit.commandBufferCount = 1;
        compute_submit.pCommandBuffers = &cmd;
        emit_fence_trace("before_vkQueueSubmit_compute", VK_SUCCESS);
        vr = vkQueueSubmit(queue, 1, &compute_submit, submit_fence);
        emit_fence_trace("after_vkQueueSubmit_compute", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkQueueSubmit (compute) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkWaitForFences_compute", VK_SUCCESS);
        vr = vkWaitForFences(device, 1, &submit_fence, VK_TRUE, UINT64_MAX);
        emit_fence_trace("after_vkWaitForFences_compute", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences (compute) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkGetFenceStatus_compute", VK_SUCCESS);
        vr = vkGetFenceStatus(device, submit_fence);
        emit_fence_trace("after_vkGetFenceStatus_compute", vr);
        printf("FENCE_STATUS_COMPUTE=%d\n", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkGetFenceStatus (compute) failed: %d\n", vr);
            goto cleanup;
        }
        vr = vkResetFences(device, 1, &submit_fence);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkResetFences failed: %d\n", vr);
            goto cleanup;
        }

        VkSubmitInfo draw_submit = {0};
        draw_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        draw_submit.commandBufferCount = 1;
        draw_submit.pCommandBuffers = &split_draw_cmd;
        emit_fence_trace("before_vkQueueSubmit_draw", VK_SUCCESS);
        vr = vkQueueSubmit(queue, 1, &draw_submit, submit_fence);
        emit_fence_trace("after_vkQueueSubmit_draw", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkQueueSubmit (draw) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkWaitForFences_draw", VK_SUCCESS);
        vr = vkWaitForFences(device, 1, &submit_fence, VK_TRUE, UINT64_MAX);
        emit_fence_trace("after_vkWaitForFences_draw", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences (draw) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkGetFenceStatus_draw", VK_SUCCESS);
        vr = vkGetFenceStatus(device, submit_fence);
        emit_fence_trace("after_vkGetFenceStatus_draw", vr);
        printf("FENCE_STATUS_DRAW=%d\n", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkGetFenceStatus (draw) failed: %d\n", vr);
            goto cleanup;
        }
    } else if (use_two_cmdb_single_submit) {
        VkSubmitInfo submit = {0};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 2;
        submit.pCommandBuffers = cmd_buffers;
        emit_fence_trace("before_vkQueueSubmit_two_cmdb_single_submit", VK_SUCCESS);
        vr = vkQueueSubmit(queue, 1, &submit, submit_fence);
        emit_fence_trace("after_vkQueueSubmit_two_cmdb_single_submit", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkQueueSubmit (single-submit-two-cmdb) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkWaitForFences_two_cmdb_single_submit", VK_SUCCESS);
        vr = vkWaitForFences(device, 1, &submit_fence, VK_TRUE, UINT64_MAX);
        emit_fence_trace("after_vkWaitForFences_two_cmdb_single_submit", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences (single-submit-two-cmdb) failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkGetFenceStatus_two_cmdb_single_submit", VK_SUCCESS);
        vr = vkGetFenceStatus(device, submit_fence);
        emit_fence_trace("after_vkGetFenceStatus_two_cmdb_single_submit", vr);
        printf("FENCE_STATUS_TWO_CMDB_SINGLE_SUBMIT=%d\n", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkGetFenceStatus (single-submit-two-cmdb) failed: %d\n", vr);
            goto cleanup;
        }
    } else {
        VkSubmitInfo submit = {0};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        emit_fence_trace("before_vkQueueSubmit", VK_SUCCESS);
        vr = vkQueueSubmit(queue, 1, &submit, submit_fence);
        emit_fence_trace("after_vkQueueSubmit", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkQueueSubmit failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkWaitForFences", VK_SUCCESS);
        vr = vkWaitForFences(device, 1, &submit_fence, VK_TRUE, UINT64_MAX);
        emit_fence_trace("after_vkWaitForFences", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkWaitForFences failed: %d\n", vr);
            goto cleanup;
        }
        emit_fence_trace("before_vkGetFenceStatus", VK_SUCCESS);
        vr = vkGetFenceStatus(device, submit_fence);
        emit_fence_trace("after_vkGetFenceStatus", vr);
        printf("FENCE_STATUS=%d\n", vr);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "vkGetFenceStatus failed: %d\n", vr);
            goto cleanup;
        }
    }
    emit_fence_trace("before_vkQueueWaitIdle", VK_SUCCESS);
    vr = vkQueueWaitIdle(queue);
    emit_fence_trace("after_vkQueueWaitIdle", vr);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "vkQueueWaitIdle failed: %d\n", vr);
        goto cleanup;
    }

    bool vertex_buffer_ok = false;
    {
        uint32_t *ib_words = NULL;
        vr = vkMapMemory(device, index_mem, 0, ib_ci.size, 0, (void **)&ib_words);
        if (vr == VK_SUCCESS && ib_words != NULL) {
            vertex_buffer_ok = true;
            if (compute_only) {
                emit_index_probe(cfg.index_type, ib_words, (size_t)ib_ci.size);
            }
            if (cfg.index_type == VK_INDEX_TYPE_UINT32) {
                for (uint32_t i = 0; i < 6; i++) {
                    if (ib_words[i] != expected_idx32[i]) {
                        vertex_buffer_ok = false;
                        break;
                    }
                }
            } else {
                for (uint32_t i = 0; i < 3; i++) {
                    if (ib_words[i] != expected_idx16_packed[i]) {
                        vertex_buffer_ok = false;
                        break;
                    }
                }
            }
            printf("VERTEX_INPUT_BUFFER_OK=%s\n", vertex_buffer_ok ? "PASS" : "FAIL");
            printf("VERTEX_BUFFER_OK=%s\n", vertex_buffer_ok ? "PASS" : "FAIL");
            vkUnmapMemory(device, index_mem);
        } else {
            printf("VERTEX_INPUT_BUFFER_OK=FAIL\n");
            printf("VERTEX_BUFFER_OK=FAIL\n");
        }
    }

    if (compute_only) {
        printf("STATUS=%s\n", vertex_buffer_ok ? "PASS" : "FAIL");
        exit_code = vertex_buffer_ok ? EXIT_SUCCESS : EXIT_FAILURE;
        goto cleanup;
    }

    VkImageSubresource subresource = {0};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subresource.mipLevel = 0;
    subresource.arrayLayer = 0;
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, image, &subresource, &layout);
    printf("ROW_PITCH=%llu\n", (unsigned long long)layout.rowPitch);
    printf("OFFSET=%llu\n", (unsigned long long)layout.offset);

    vr = vkMapMemory(device, image_mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (vr != VK_SUCCESS || !mapped) {
        fprintf(stderr, "vkMapMemory failed: %d\n", vr);
        goto cleanup;
    }

    contiguous = (uint8_t *)malloc((size_t)cfg.width * cfg.height * 4u);
    if (!contiguous) {
        goto cleanup;
    }

    const uint8_t *src_base = (const uint8_t *)mapped + layout.offset;
    uint64_t expected_pixels = 0;
    uint64_t alpha_255_pixels = 0;
    uint8_t min_rgb[3] = {0xffu, 0xffu, 0xffu};
    uint8_t max_rgb[3] = {0x00u, 0x00u, 0x00u};
    for (uint32_t y = 0; y < cfg.height; y++) {
        const uint8_t *src_row = src_base + (size_t)y * layout.rowPitch;
        uint8_t *dst_row = contiguous + (size_t)y * cfg.width * 4u;
        memcpy(dst_row, src_row, (size_t)cfg.width * 4u);
        for (uint32_t x = 0; x < cfg.width; x++) {
            const uint8_t *px = dst_row + (size_t)x * 4u;
            if (px[3] == 0xffu) {
                alpha_255_pixels++;
            }
            for (uint32_t c = 0; c < 3; c++) {
                if (px[c] < min_rgb[c]) {
                    min_rgb[c] = px[c];
                }
                if (px[c] > max_rgb[c]) {
                    max_rgb[c] = px[c];
                }
            }
            if (px[0] == cfg.magic[0] && px[1] == cfg.magic[1] &&
                px[2] == cfg.magic[2] && px[3] == cfg.magic[3]) {
                expected_pixels++;
            }
        }
    }

    uint32_t cx = cfg.width / 2u;
    uint32_t cy = cfg.height / 2u;
    const uint8_t *center = contiguous + ((size_t)cy * cfg.width + cx) * 4u;
    printf("CENTER_RGBA=%02x,%02x,%02x,%02x\n",
           center[0], center[1], center[2], center[3]);
    printf("EXPECTED_PIXELS=%llu\n", (unsigned long long)expected_pixels);
    uint64_t total_pixels = (uint64_t)cfg.width * cfg.height;
    printf("TOTAL_PIXELS=%llu\n", (unsigned long long)total_pixels);
    printf("ALPHA_255_PIXELS=%llu\n", (unsigned long long)alpha_255_pixels);
    printf("RGB_RANGE_R=%u..%u\n", min_rgb[0], max_rgb[0]);
    printf("RGB_RANGE_G=%u..%u\n", min_rgb[1], max_rgb[1]);
    printf("RGB_RANGE_B=%u..%u\n", min_rgb[2], max_rgb[2]);

    if (cfg.dump_path) {
        FILE *dump = fopen(cfg.dump_path, "wb");
        if (dump) {
            fwrite(contiguous, 1, (size_t)cfg.width * cfg.height * 4u, dump);
            fclose(dump);
            printf("DUMP_PATH=%s\n", cfg.dump_path);
        }
    }
    write_hex_preview(cfg.hex_path, contiguous, cfg.width, cfg.height);
    if (cfg.hex_path) {
        printf("HEX_PATH=%s\n", cfg.hex_path);
    }

    bool pass = false;
    if (!cfg.interpolated_validation) {
        bool center_is_expected = center[0] == cfg.magic[0] && center[1] == cfg.magic[1] &&
                                  center[2] == cfg.magic[2] && center[3] == cfg.magic[3];
        bool enough_pixels = expected_pixels * 10u >= total_pixels * 9u;
        pass = center_is_expected && enough_pixels && vertex_buffer_ok;
    } else {
        uint32_t range_r = (uint32_t)max_rgb[0] - (uint32_t)min_rgb[0];
        uint32_t range_g = (uint32_t)max_rgb[1] - (uint32_t)min_rgb[1];
        uint32_t range_b = (uint32_t)max_rgb[2] - (uint32_t)min_rgb[2];
        bool alpha_coverage_ok = alpha_255_pixels >= (total_pixels * 95u) / 100u;
        bool non_uniform_rgb = range_r >= 32u && range_g >= 32u && range_b >= 32u;
        bool center_is_mixed = center[0] > 48u && center[0] < 208u &&
                               center[1] > 16u && center[1] < 208u &&
                               center[2] > 16u && center[2] < 208u &&
                               center[3] >= 240u;
        printf("INTERP_ALPHA_COVERAGE=%s\n", alpha_coverage_ok ? "PASS" : "FAIL");
        printf("INTERP_NON_UNIFORM_RGB=%s\n", non_uniform_rgb ? "PASS" : "FAIL");
        printf("INTERP_CENTER_MIXED=%s\n", center_is_mixed ? "PASS" : "FAIL");
        pass = alpha_coverage_ok && non_uniform_rgb && center_is_mixed && vertex_buffer_ok;
    }

    printf("STATUS=%s\n", pass ? "PASS" : "FAIL");
    exit_code = pass ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    if (mapped) {
        vkUnmapMemory(device, image_mem);
    }
    free(contiguous);

    if (cmd_pool && cmd_buffer_count > 0) {
        vkFreeCommandBuffers(device, cmd_pool, cmd_buffer_count, cmd_buffers);
    }
    if (submit_fence) {
        vkDestroyFence(device, submit_fence, NULL);
    }
    if (cmd_pool) {
        vkDestroyCommandPool(device, cmd_pool, NULL);
    }
    if (compute_pipeline) {
        vkDestroyPipeline(device, compute_pipeline, NULL);
    }
    if (pipeline) {
        vkDestroyPipeline(device, pipeline, NULL);
    }
    if (compute_pipeline_layout) {
        vkDestroyPipelineLayout(device, compute_pipeline_layout, NULL);
    }
    if (pipeline_layout) {
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    }
    if (cs_module) {
        vkDestroyShaderModule(device, cs_module, NULL);
    }
    if (vs_module) {
        vkDestroyShaderModule(device, vs_module, NULL);
    }
    if (fs_module) {
        vkDestroyShaderModule(device, fs_module, NULL);
    }
    if (compute_desc_pool) {
        vkDestroyDescriptorPool(device, compute_desc_pool, NULL);
    }
    if (compute_set_layout) {
        vkDestroyDescriptorSetLayout(device, compute_set_layout, NULL);
    }
    if (framebuffer) {
        vkDestroyFramebuffer(device, framebuffer, NULL);
    }
    if (render_pass) {
        vkDestroyRenderPass(device, render_pass, NULL);
    }
    if (image_view) {
        vkDestroyImageView(device, image_view, NULL);
    }
    if (image) {
        vkDestroyImage(device, image, NULL);
    }
    if (index_buffer) {
        vkDestroyBuffer(device, index_buffer, NULL);
    }
    if (image_mem) {
        vkFreeMemory(device, image_mem, NULL);
    }
    if (index_mem) {
        vkFreeMemory(device, index_mem, NULL);
    }
    if (device) {
        vkDestroyDevice(device, NULL);
    }
    if (instance) {
        vkDestroyInstance(instance, NULL);
    }
    return exit_code;
}
