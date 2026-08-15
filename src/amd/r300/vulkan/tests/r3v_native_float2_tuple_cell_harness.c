/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Drives the fetched FLOAT_4 + FLOAT_2 tuple cell through the native ICD
 * on the Radeon noop drm-shim.  The closed leg proves the fail-closed
 * boundary and retained semantic object.  The open leg proves the
 * recorder-to-queue transport while the shim leaves the poison carrier
 * and fetched vertex source unchanged.  The geometry leg binds an
 * oversized vertex BO so the tuple cell's frozen geometry is the one
 * arming factor that refuses.
 */

#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native.h"
#include "tests/r3v_native_shim_arming.h"

#include "util/mesa-blake3.h"

#define TUPLE_VERTEX_BYTES                                      \
   (R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *                   \
    (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +                \
     R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES))

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

static unsigned failures;

#define CHECK(condition, ...)                   \
   do {                                         \
      if (!(condition)) {                       \
         fprintf(stderr, "FAIL: " __VA_ARGS__); \
         fprintf(stderr, "\n");                 \
         failures++;                            \
      }                                         \
   } while (0)

static bool
same_file(const char *left, const char *right)
{
   struct stat left_status;
   struct stat right_status;
   return stat(left, &left_status) == 0 &&
          stat(right, &right_status) == 0 &&
          left_status.st_dev == right_status.st_dev &&
          left_status.st_ino == right_status.st_ino;
}

static int
attest_shim_provider(void)
{
   const char *expected = getenv("DRM_SHIM_EXPECTED_DSO");
   if (expected == NULL || expected[0] == '\0') {
      fprintf(stderr,
              "REFUSE: DRM_SHIM_EXPECTED_DSO is unset; the harness "
              "cannot attest the interposition provider\n");
      return 1;
   }

   static const char *const interposed[] = {"open", "ioctl"};
   for (unsigned index = 0; index < 2; index++) {
      dlerror();
      void *symbol = dlsym(RTLD_DEFAULT, interposed[index]);
      const char *error = dlerror();
      if (symbol == NULL || error != NULL) {
         fprintf(stderr, "REFUSE: symbol %s is unavailable: %s\n",
                 interposed[index], error != NULL ? error : "unknown");
         return 1;
      }
      Dl_info info;
      memset(&info, 0, sizeof(info));
      if (dladdr(symbol, &info) == 0 || info.dli_fname == NULL) {
         fprintf(stderr, "REFUSE: symbol %s has no provider object\n",
                 interposed[index]);
         return 1;
      }
      if (!same_file(info.dli_fname, expected)) {
         fprintf(stderr,
                 "REFUSE: symbol %s provider %s differs from expected "
                 "shim %s\n",
                 interposed[index], info.dli_fname, expected);
         return 1;
      }
   }
   return 0;
}

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)gdpa(device, #name)

static int
read_whole_file(const char *directory, const char *name, void **data_out,
                size_t *size_out)
{
   char path[1024];
   int path_length =
      snprintf(path, sizeof(path), "%s/%s", directory, name);
   if (path_length < 0 || (size_t)path_length >= sizeof(path))
      return 1;
   FILE *file = fopen(path, "rb");
   if (file == NULL)
      return 1;
   if (fseek(file, 0, SEEK_END) != 0) {
      fclose(file);
      return 1;
   }
   long length = ftell(file);
   if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      return 1;
   }
   void *data = malloc(length > 0 ? (size_t)length : 1);
   if (data == NULL) {
      fclose(file);
      return 1;
   }
   size_t bytes_read = fread(data, 1, (size_t)length, file);
   fclose(file);
   if (bytes_read != (size_t)length) {
      free(data);
      return 1;
   }
   *data_out = data;
   *size_out = (size_t)length;
   return 0;
}

static bool
evidence_file_present(const char *directory, const char *name)
{
   char path[1024];
   int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
   if (length < 0 || (size_t)length >= sizeof(path))
      return false;
   struct stat status;
   return stat(path, &status) == 0;
}

static void
check_command_relocations(const char *manifest_dir,
                          const struct r3v_native_cmd_buffer *native_cmd,
                          bool include_completion)
{
   const char *name = include_completion ? "submit_relocs.bin" : "relocs.bin";
   void *data = NULL;
   size_t size = 0;
   CHECK(read_whole_file(manifest_dir, name, &data, &size) == 0,
         "%s is retained", name);
   if (data == NULL)
      return;

   const unsigned expected_count =
      R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT + (include_completion ? 1 : 0);
   CHECK(size == expected_count * sizeof(struct drm_radeon_cs_reloc),
         "%s carries %u relocations (%zu bytes)", name, expected_count,
         size);
   if (size == expected_count * sizeof(struct drm_radeon_cs_reloc)) {
      const struct drm_radeon_cs_reloc *relocations = data;
      for (uint32_t index = 0;
           index < R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT; index++) {
         const struct r3v_native_bo_reference *reference =
            &native_cmd->references[index];
         CHECK(relocations[index].handle == reference->handle &&
                  relocations[index].read_domains == reference->read_domains &&
                  relocations[index].write_domain == reference->write_domain &&
                  relocations[index].flags == 0,
               "%s relocation %u matches the command reference", name,
               index);
      }
      if (include_completion) {
         const struct drm_radeon_cs_reloc *completion =
            &relocations[R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT];
         CHECK(completion->handle != relocations[0].handle &&
                  completion->handle != relocations[1].handle &&
                  completion->read_domains == 0 &&
                  completion->write_domain == RADEON_GEM_DOMAIN_GTT &&
                  completion->flags == 0,
               "the final relocation is the completion BO");
      }
   }
   free(data);
}

static void
check_unexecuted_memory(const void *carrier_map, uint32_t carrier_bytes,
                        const uint32_t *expected, uint32_t expected_dwords,
                        const void *vertex_map,
                        const uint8_t vertex_reference[TUPLE_VERTEX_BYTES])
{
   struct r300_r2vb_producer_carrier_verdict verdict;
   CHECK(r300_r2vb_producer_carrier_check(
            expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
            carrier_map, carrier_bytes, &verdict) == 0,
         "the carrier check accepts its inputs");
   CHECK(!verdict.expected_pass &&
            verdict.mismatched_dwords == expected_dwords &&
            verdict.poison_dwords == expected_dwords,
         "the noop shim executes no carrier dword");
   CHECK(verdict.tail_poison_pass && verdict.disturbed_tail_dwords == 0,
         "the carrier tail remains poison");
   CHECK(memcmp(vertex_map, vertex_reference, TUPLE_VERTEX_BYTES) == 0,
         "the read-only vertex source remains byte-identical");
}

int
main(int argc, char **argv)
{
   if (argc != 2 ||
       (strcmp(argv[1], "closed") != 0 && strcmp(argv[1], "open") != 0 &&
        strcmp(argv[1], "geometry") != 0)) {
      fprintf(stderr, "usage: %s closed|open|geometry\n", argv[0]);
      return 2;
   }

   const bool geometry_mode = strcmp(argv[1], "geometry") == 0;
   const bool open_gate = strcmp(argv[1], "open") == 0 || geometry_mode;

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_path[] = "/tmp/r3v-native-float2-tuple-XXXXXX";
   if (mkdtemp(manifest_path) == NULL) {
      fprintf(stderr, "manifest directory creation failed\n");
      return 2;
   }
   if (setenv("R3V_NATIVE_MANIFEST_DIR", manifest_path, 1) != 0) {
      fprintf(stderr, "manifest directory export failed\n");
      rmdir(manifest_path);
      return 2;
   }
   const char *manifest_dir = manifest_path;

   struct r300_r2vb_float2_tuple_ib reference;
   CHECK(r300_r2vb_float2_tuple_reference_emit(&reference) == 0,
         "reference tuple pass emits");
   if (failures != 0)
      return 1;
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);

   uint32_t carrier_bytes = 0;
   CHECK(r3v_native_producer_carrier_bytes(&carrier_bytes) == 0,
         "tuple carrier geometry resolves");
   uint32_t expected[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4];
   const uint32_t expected_dwords =
      (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   CHECK(r300_r2vb_float2_tuple_reference_expected(expected,
                                                   expected_dwords) == 0,
         "tuple carrier expectation resolves");
   uint8_t vertex_reference[TUPLE_VERTEX_BYTES];
   CHECK(r300_r2vb_float2_tuple_vertex_stream(
            r300_r2vb_float2_tuple_reference_records,
            R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_reference,
            sizeof(vertex_reference)) == 0,
         "tuple vertex source serializes");
   if (failures != 0)
      return 1;

   if (open_gate) {
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", reference_digest, 1);
      struct utsname host;
      if (uname(&host) != 0) {
         fprintf(stderr, "uname failed\n");
         return 2;
      }
      setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
      setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
             R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
   } else {
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   }

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS, "vkCreateInstance: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;

   uint32_t physical_device_count = 1;
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                       &physical_device);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            physical_device_count == 1,
         "one shim physical device enumerates: %d count %u", result,
         physical_device_count);
   if (physical_device == VK_NULL_HANDLE)
      return 1;

   const float queue_priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(
      physical_device,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &queue_priority,
            },
      },
      NULL, &device);
   CHECK(result == VK_SUCCESS, "vkCreateDevice: %d", result);
   if (result != VK_SUCCESS)
      return 1;
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   native_device->arming_provider = &r3v_native_shim_arming_provider;

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = carrier_bytes,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkDeviceMemory undersized_vertex_memory = VK_NULL_HANDLE;
   VkDeviceMemory oversized_vertex_memory = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   void *carrier_map = NULL;
   void *vertex_map = NULL;

   result = vkAllocateMemory(device, &allocation, NULL, &carrier_memory);
   CHECK(result == VK_SUCCESS && carrier_memory != VK_NULL_HANDLE,
         "carrier vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || carrier_memory == VK_NULL_HANDLE)
      goto done;

   allocation.allocationSize = TUPLE_VERTEX_BYTES;
   result = vkAllocateMemory(device, &allocation, NULL, &vertex_memory);
   CHECK(result == VK_SUCCESS && vertex_memory != VK_NULL_HANDLE,
         "vertex vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || vertex_memory == VK_NULL_HANDLE)
      goto done;

   if (geometry_mode) {
      allocation.allocationSize =
         TUPLE_VERTEX_BYTES + R3V_NATIVE_MEMORY_ALIGNMENT;
      result = vkAllocateMemory(device, &allocation, NULL,
                                &oversized_vertex_memory);
      CHECK(result == VK_SUCCESS &&
               oversized_vertex_memory != VK_NULL_HANDLE,
            "oversized vertex vkAllocateMemory: %d", result);
      if (result != VK_SUCCESS || oversized_vertex_memory == VK_NULL_HANDLE)
         goto done;
   }

   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS && pool != VK_NULL_HANDLE,
         "vkCreateCommandPool: %d", result);
   if (result != VK_SUCCESS || pool == VK_NULL_HANDLE)
      goto done;

   result = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &command_buffer);
   CHECK(result == VK_SUCCESS && command_buffer != VK_NULL_HANDLE,
         "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS || command_buffer == VK_NULL_HANDLE)
      goto done;
   result = vkBeginCommandBuffer(
      command_buffer,
      &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(command_buffer);
   if (geometry_mode) {
      result = r3v_native_record_r2vb_float2_tuple(
         command_buffer, carrier_memory, oversized_vertex_memory);
      CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
            "oversized tuple vertex refuses recording: %d", result);
      if (result != VK_ERROR_INITIALIZATION_FAILED)
         goto done;
      CHECK(native_cmd->ib == NULL && native_cmd->references == NULL,
            "a refused tuple vertex leaves the command buffer empty");
      int install_result = r3v_native_float2_tuple_cell_install(
         native_cmd, r3v_native_memory_from_handle(carrier_memory),
         r3v_native_memory_from_handle(oversized_vertex_memory));
      CHECK(install_result == 0,
            "tuple cell installs over the oversized vertex allocation");
      if (install_result != 0)
         goto done;
   } else {
      allocation.allocationSize = TUPLE_VERTEX_BYTES - 1;
      result = vkAllocateMemory(device, &allocation, NULL,
                                &undersized_vertex_memory);
      CHECK(result == VK_SUCCESS &&
               undersized_vertex_memory != VK_NULL_HANDLE,
            "undersized vertex vkAllocateMemory: %d", result);
      if (result != VK_SUCCESS || undersized_vertex_memory == VK_NULL_HANDLE)
         goto done;
      result = r3v_native_record_r2vb_float2_tuple(
         command_buffer, carrier_memory, undersized_vertex_memory);
      CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
            "undersized tuple vertex refuses recording: %d", result);
      CHECK(native_cmd->ib == NULL && native_cmd->references == NULL,
            "a refused tuple vertex leaves the command buffer empty");
      vkFreeMemory(device, undersized_vertex_memory, NULL);
      undersized_vertex_memory = VK_NULL_HANDLE;
      if (result != VK_ERROR_INITIALIZATION_FAILED)
         goto done;
      result = r3v_native_record_r2vb_float2_tuple(
         command_buffer, carrier_memory, vertex_memory);
      CHECK(result == VK_SUCCESS, "tuple cell recording: %d", result);
      if (result != VK_SUCCESS)
         goto done;
   }

   result = vkEndCommandBuffer(command_buffer);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      goto done;
   CHECK(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE,
         "the recorded cell declares the tuple kind");
   CHECK(native_cmd->ib != NULL &&
            native_cmd->ib_size_dwords == reference.ib_size_dwords &&
            memcmp(native_cmd->ib, reference.ib,
                   reference.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the installed stream is the reference tuple pass");
   CHECK(native_cmd->reference_count == R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT,
         "the tuple carries carrier and vertex references: %u",
         native_cmd->reference_count);
   if (native_cmd->ib == NULL ||
       native_cmd->ib_size_dwords != reference.ib_size_dwords ||
       native_cmd->reference_count != R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT)
      goto done;

   if (!geometry_mode) {
      result = vkMapMemory(device, carrier_memory, 0, VK_WHOLE_SIZE, 0,
                           &carrier_map);
      CHECK(result == VK_SUCCESS && carrier_map != NULL,
            "carrier maps before submission: %d", result);
      result = vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                           &vertex_map);
      CHECK(result == VK_SUCCESS && vertex_map != NULL,
            "vertex source maps before submission: %d", result);
      if (result != VK_SUCCESS || carrier_map == NULL || vertex_map == NULL)
         goto done;
      check_unexecuted_memory(carrier_map, carrier_bytes, expected,
                              expected_dwords, vertex_map,
                              vertex_reference);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   CHECK(queue != VK_NULL_HANDLE, "the native device exposes queue family 0");
   if (queue == VK_NULL_HANDLE)
      goto done;
   result = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &command_buffer,
      },
      VK_NULL_HANDLE);

   if (geometry_mode) {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "an oversized tuple vertex refuses submission: %d", result);
      CHECK(evidence_file_present(manifest_dir, "submit_manifest.json"),
            "the geometry refusal retains the submit object");
      CHECK(!evidence_file_present(manifest_dir, "attempt.token"),
            "the geometry refusal precedes one-shot disarm");
      check_command_relocations(manifest_dir, native_cmd, false);
      check_command_relocations(manifest_dir, native_cmd, true);
      goto done;
   }

   check_unexecuted_memory(carrier_map, carrier_bytes, expected,
                           expected_dwords, vertex_map, vertex_reference);
   check_command_relocations(manifest_dir, native_cmd, false);

   if (open_gate) {
      CHECK(result == VK_SUCCESS,
            "open tuple submission through the shim: %d", result);
      CHECK(r3v_native_queue_submission_status(device) ==
               R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the tuple shim submission reports completed transport: %s",
            r3v_native_queue_status_name(
               r3v_native_queue_submission_status(device)));
      check_command_relocations(manifest_dir, native_cmd, true);
      CHECK(evidence_file_present(manifest_dir, "attempt.token"),
            "the accepted tuple submission consumes the one-shot token");
      result = vkQueueSubmit(
         queue, 1,
         &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer,
         },
         VK_NULL_HANDLE);
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "a consumed token refuses the second tuple submission: %d",
            result);
      check_unexecuted_memory(carrier_map, carrier_bytes, expected,
                              expected_dwords, vertex_map, vertex_reference);
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "closed tuple gate fails closed: %d", result);
      void *ib_data = NULL;
      size_t ib_size = 0;
      CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) == 0,
            "tuple ib.bin is retained");
      if (ib_data != NULL) {
         CHECK(ib_size == reference.ib_size_dwords * sizeof(uint32_t),
               "tuple ib.bin length matches the reference pass");
         const size_t reference_size =
            reference.ib_size_dwords * sizeof(reference.ib[0]);
         uint8_t *reference_bytes = malloc(reference_size);
         CHECK(reference_bytes != NULL,
               "tuple reference serialization buffer allocates");
         if (reference_bytes != NULL && ib_size == reference_size) {
            r300_triangle_ib_serialize(reference.ib,
                                       reference.ib_size_dwords,
                                       reference_bytes);
            CHECK(memcmp(ib_data, reference_bytes, reference_size) == 0,
                  "tuple ib.bin is the canonical reference serialization");
         }
         free(reference_bytes);
         free(ib_data);
      }
      CHECK(!evidence_file_present(manifest_dir, "submit_manifest.json"),
            "the closed tuple gate retains no submit object");
      CHECK(!evidence_file_present(manifest_dir, "attempt.token"),
            "the closed tuple gate consumes no one-shot token");
   }

done:
   r300_r2vb_float2_tuple_pass_release(&reference);
   if (pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, pool, NULL);
   if (carrier_map != NULL)
      vkUnmapMemory(device, carrier_memory);
   if (vertex_map != NULL)
      vkUnmapMemory(device, vertex_memory);
   vkFreeMemory(device, carrier_memory, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, undersized_vertex_memory, NULL);
   vkFreeMemory(device, oversized_vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_float2_tuple_cell_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
