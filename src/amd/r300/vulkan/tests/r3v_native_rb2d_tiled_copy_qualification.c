/* SPDX-License-Identifier: MIT */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_rb2d_copy.h"
#include "amd/r300/common/r300_zb_tile_copy.h"
#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define QUALIFICATION_BYTES 12288u
#define QUALIFICATION_GUARD_BYTES 2048u
#define QUALIFICATION_GRID_WIDTH 2u

static uint32_t qualification_write_mask = UINT32_MAX;
static bool qualification_byte_copy;
static struct r300_rb2d_copy_segment qualification_byte_span;

static struct r300_rb2d_copy_plan
byte_plan(void)
{
   return (struct r300_rb2d_copy_plan){
      .source_buffer_bytes = QUALIFICATION_BYTES,
      .destination_buffer_bytes = QUALIFICATION_BYTES,
      .segments = &qualification_byte_span,
      .segment_count = 1u,
      .byte_carrier = true,
   };
}

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *name);

static bool
parse_parity(const char *text, uint32_t *value)
{
   if (text == NULL || value == NULL || text[1] != '\0' ||
       (text[0] != '0' && text[0] != '1'))
      return false;
   *value = (uint32_t)(text[0] - '0');
   return true;
}

static uint32_t
source_word(uint32_t word_index)
{
   const uint32_t mixed = word_index * 0x9e3779b9u + 0x243f6a88u;
   return (mixed ^ (mixed >> 11) ^ (word_index << 19)) | 0x01000000u;
}

static uint32_t
destination_word(uint32_t word_index)
{
   const uint32_t mixed = word_index * 0x7f4a7c15u + 0xa5c31e27u;
   return (mixed ^ (mixed << 7) ^ (word_index >> 3)) | 0x80000000u;
}

static void
count_packed_component_mismatches(uint32_t actual, uint32_t expected,
                                  uint32_t *depth_mismatches,
                                  uint32_t *stencil_mismatches)
{
   if ((actual & 0xffffff00u) != (expected & 0xffffff00u))
      (*depth_mismatches)++;
   if ((actual & 0x000000ffu) != (expected & 0x000000ffu))
      (*stencil_mismatches)++;
}

static struct r300_zb_tile_copy_request
request_for(uint32_t source_x, uint32_t source_y, uint32_t destination_x,
            uint32_t destination_y)
{
   return (struct r300_zb_tile_copy_request){
      .source = {
         .tile_offset_bytes =
            QUALIFICATION_GUARD_BYTES +
            R300_ZB_TILE_COPY_BYTES *
               (source_y * QUALIFICATION_GRID_WIDTH + source_x),
         .buffer_bytes = QUALIFICATION_BYTES,
         .macro_x = source_x,
         .macro_y = source_y,
         .macro_width = 2u,
         .macro_height = 2u,
         .macro_pitch = 2u,
      },
      .destination = {
         .tile_offset_bytes =
            QUALIFICATION_GUARD_BYTES +
            R300_ZB_TILE_COPY_BYTES *
               (destination_y * QUALIFICATION_GRID_WIDTH + destination_x),
         .buffer_bytes = QUALIFICATION_BYTES,
         .macro_x = destination_x,
         .macro_y = destination_y,
         .macro_width = 2u,
         .macro_height = 2u,
         .macro_pitch = 2u,
      },
      .same_buffer = false,
      .same_format = true,
      .compressed = false,
      .multisample = false,
   };
}

static int
expected_stream(const struct r300_zb_tile_copy_request *request,
                uint32_t **words_out, uint32_t *dword_count_out)
{
   struct r300_zb_tile_copy_plan tile_plan;
   if (r300_zb_tile_copy_plan_build(request, &tile_plan) !=
       R300_ZB_TILE_COPY_OK)
      return -EINVAL;
   struct r300_rb2d_copy_segment
      segment_storage[R300_RB2D_COPY_MAX_SEGMENTS];
   struct r300_rb2d_copy_plan copy_plan;
   if (r300_rb2d_copy_plan_from_zb_tile(
          &tile_plan, QUALIFICATION_BYTES, QUALIFICATION_BYTES, false,
          segment_storage, &copy_plan) != R300_RB2D_COPY_OK)
      return -EINVAL;
   if (qualification_byte_copy)
      copy_plan = byte_plan();
   const uint32_t dwords = R300_RB2D_COPY_DWORDS(copy_plan.segment_count);
   uint32_t *words = calloc(dwords, sizeof(*words));
   if (words == NULL)
      return -ENOMEM;
   struct r300_rb2d_copy_ib emitted;
   const int result =
      r300_rb2d_copy_emit_masked_into(&copy_plan, qualification_write_mask,
                                     words, dwords, &emitted);
   if (result != 0 || emitted.ib_size_dwords != dwords ||
       r300_rb2d_copy_validate_reloc_sites(&emitted) != 0) {
      free(words);
      return result != 0 ? result : -EINVAL;
   }
   for (uint32_t site_index = 0; site_index < emitted.reloc_site_count;
        site_index++) {
      const struct r300_rb2d_copy_reloc_site *site =
         &emitted.reloc_sites[site_index];
      words[site->ib_index] =
         (site->role == R300_RB2D_COPY_SLOT_SOURCE ? 0u : 1u) * 4u;
   }
   *words_out = words;
   *dword_count_out = dwords;
   return 0;
}

static uint32_t
expected_destination_word(const struct r300_zb_tile_copy_request *request,
                          uint32_t word_index);

static void
print_stream(const struct r300_zb_tile_copy_request *request)
{
   uint32_t *words = NULL;
   uint32_t dwords = 0u;
   if (expected_stream(request, &words, &dwords) != 0) {
      fprintf(stderr, "expected stream construction failed\n");
      exit(2);
   }
   uint32_t source_before[QUALIFICATION_BYTES / sizeof(uint32_t)];
   uint32_t destination_before[QUALIFICATION_BYTES / sizeof(uint32_t)];
   uint32_t destination_expected[QUALIFICATION_BYTES / sizeof(uint32_t)];
   for (uint32_t word_index = 0;
        word_index < QUALIFICATION_BYTES / sizeof(uint32_t); word_index++) {
      source_before[word_index] = source_word(word_index);
      destination_before[word_index] = destination_word(word_index);
      destination_expected[word_index] =
         expected_destination_word(request, word_index);
   }
   blake3_hash ib_digest;
   blake3_hash source_digest;
   blake3_hash destination_before_digest;
   blake3_hash destination_expected_digest;
   char ib_hex[BLAKE3_OUT_LEN * 2u + 1u];
   char source_hex[BLAKE3_OUT_LEN * 2u + 1u];
   char destination_before_hex[BLAKE3_OUT_LEN * 2u + 1u];
   char destination_expected_hex[BLAKE3_OUT_LEN * 2u + 1u];
   _mesa_blake3_compute(words, (size_t)dwords * sizeof(*words), ib_digest);
   _mesa_blake3_compute(source_before, sizeof(source_before), source_digest);
   _mesa_blake3_compute(destination_before, sizeof(destination_before),
                        destination_before_digest);
   _mesa_blake3_compute(destination_expected, sizeof(destination_expected),
                        destination_expected_digest);
   _mesa_blake3_format(ib_hex, ib_digest);
   _mesa_blake3_format(source_hex, source_digest);
   _mesa_blake3_format(destination_before_hex, destination_before_digest);
   _mesa_blake3_format(destination_expected_hex, destination_expected_digest);
   if (qualification_write_mask != UINT32_MAX)
      printf("component=%s ", qualification_write_mask == 0xffffff00u
                                   ? "depth" : "stencil");
   if (qualification_byte_copy)
      printf("source-byte=%llu destination-byte=%llu byte-count=%u ",
             (unsigned long long)qualification_byte_span.source_offset_bytes,
             (unsigned long long)qualification_byte_span.destination_offset_bytes,
             qualification_byte_span.byte_count);
   printf("source=%u,%u destination=%u,%u ib_dwords=%u ib_blake3=%s "
          "source_before_blake3=%s destination_before_blake3=%s "
          "destination_expected_blake3=%s\n",
          request->source.macro_x, request->source.macro_y,
          request->destination.macro_x, request->destination.macro_y, dwords,
          ib_hex, source_hex, destination_before_hex,
          destination_expected_hex);
   free(words);
}

static int
prepare_all(void)
{
   if (qualification_byte_copy) {
      const struct r300_zb_tile_copy_request request = request_for(0, 0, 0, 0);
      print_stream(&request);
      return 0;
   }
   for (uint32_t source_x = 0; source_x < 2u; source_x++)
      for (uint32_t source_y = 0; source_y < 2u; source_y++)
         for (uint32_t destination_x = 0; destination_x < 2u;
              destination_x++)
            for (uint32_t destination_y = 0; destination_y < 2u;
                 destination_y++) {
               const struct r300_zb_tile_copy_request request =
                  request_for(source_x, source_y, destination_x,
                              destination_y);
               print_stream(&request);
            }
   return 0;
}

static int
ordered_operations_selftest(void)
{
   struct r3v_native_memory source = {0};
   struct r3v_native_memory destination = {0};
   source.bo.handle = 61u;
   source.bo.size = QUALIFICATION_BYTES;
   destination.bo.handle = 73u;
   destination.bo.size = QUALIFICATION_BYTES;
   const struct r300_rb2d_copy_segment segments[2] = {
      { .source_offset_bytes = 128u, .destination_offset_bytes = 256u,
        .byte_count = 8u },
      { .source_offset_bytes = 512u, .destination_offset_bytes = 768u,
        .byte_count = 16u },
   };
   struct r3v_native_rb2d_copy_operation *operations =
      calloc(2u, sizeof(*operations));
   struct r3v_native_bo_reference *references =
      calloc(2u, sizeof(*references));
   uint32_t *ib = calloc(2u * R300_RB2D_COPY_DWORDS(1u), sizeof(*ib));
   if (operations == NULL || references == NULL || ib == NULL) {
      free(operations);
      free(references);
      free(ib);
      return 1;
   }
   references[0] = (struct r3v_native_bo_reference){
      .handle = source.bo.handle, .read_domains = RADEON_GEM_DOMAIN_GTT,
      .memory = &source,
   };
   references[1] = (struct r3v_native_bo_reference){
      .handle = destination.bo.handle, .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = &destination,
   };
   const uint32_t masks[2] = {0xffffff00u, 0x000000ffu};
   for (uint32_t index = 0u; index < 2u; index++) {
      const struct r300_rb2d_copy_plan plan = {
         .source_buffer_bytes = QUALIFICATION_BYTES,
         .destination_buffer_bytes = QUALIFICATION_BYTES,
         .segments = &segments[index], .segment_count = 1u,
         .byte_carrier = true,
      };
      struct r300_rb2d_copy_ib emitted;
      if (r300_rb2d_copy_emit_masked_into(
             &plan, masks[index], ib + index * R300_RB2D_COPY_DWORDS(1u),
             R300_RB2D_COPY_DWORDS(1u), &emitted) != 0 ||
          r300_rb2d_copy_validate_reloc_sites(&emitted) != 0)
         goto fail;
      for (uint32_t site = 0u; site < emitted.reloc_site_count; site++)
         (ib + index * R300_RB2D_COPY_DWORDS(1u))[emitted.reloc_sites[site].ib_index] =
            (emitted.reloc_sites[site].role == R300_RB2D_COPY_SLOT_SOURCE ? 0u : 1u) * 4u;
      operations[index] = (struct r3v_native_rb2d_copy_operation){
         .source_memory = &source, .destination_memory = &destination,
         .segment_count = 1u, .source_buffer_bytes = QUALIFICATION_BYTES,
         .destination_buffer_bytes = QUALIFICATION_BYTES,
         .write_mask = masks[index], .byte_carrier = true,
      };
      operations[index].segments[0] = segments[index];
   }
   struct r3v_native_cmd_buffer cmd = {
      .cell_kind = R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION,
      .ib = ib, .ib_size_dwords = 2u * R300_RB2D_COPY_DWORDS(1u),
      .references = references, .reference_count = 2u,
      .rb2d_tiled_copy_configured = true,
      .rb2d_copy_geometry = R3V_NATIVE_RB2D_COPY_GEOMETRY_SEGMENTS,
      .rb2d_copy_operations = operations, .rb2d_copy_operation_count = 2u,
      .rb2d_copy_operation_capacity = 2u,
   };
   if (!r3v_native_rb2d_tiled_copy_geometry_valid(&cmd))
      goto fail;
   operations[1].segments[0].source_offset_bytes++;
   if (r3v_native_rb2d_tiled_copy_geometry_valid(&cmd))
      goto fail;
   operations[1].segments[0].source_offset_bytes--;
   operations[1].write_mask ^= 1u;
   if (r3v_native_rb2d_tiled_copy_geometry_valid(&cmd))
      goto fail;
   operations[1].write_mask ^= 1u;
   references[1].handle = references[0].handle;
   if (r3v_native_rb2d_tiled_copy_geometry_valid(&cmd))
      goto fail;
   references[1].handle = destination.bo.handle;
   r3v_native_cmd_buffer_release_ib(&cmd);
   if (cmd.rb2d_copy_operations != NULL || cmd.rb2d_copy_operation_count != 0u ||
       cmd.ib != NULL || cmd.references != NULL || cmd.ib_size_dwords != 0u)
      return 1;
   return 0;
fail:
   free(operations);
   free(references);
   free(ib);
   return 1;
}

static int
selftest(void)
{
   if (ordered_operations_selftest() != 0)
      return 1;
   const struct r300_zb_tile_copy_request request = request_for(0u, 1u, 1u, 0u);
   uint32_t *ib = NULL;
   uint32_t ib_dwords = 0u;
   if (expected_stream(&request, &ib, &ib_dwords) != 0)
      return 1;
   struct r3v_native_memory source = {0};
   struct r3v_native_memory destination = {0};
   source.bo.handle = 31u;
   source.bo.size = QUALIFICATION_BYTES;
   destination.bo.handle = 47u;
   destination.bo.size = QUALIFICATION_BYTES;
   struct r3v_native_bo_reference references[2] = {
      { .handle = source.bo.handle,
        .read_domains = RADEON_GEM_DOMAIN_GTT,
        .memory = &source },
      { .handle = destination.bo.handle,
        .write_domain = RADEON_GEM_DOMAIN_GTT,
        .memory = &destination },
   };
   struct r3v_native_cmd_buffer cmd = {0};
   cmd.cell_kind = R3V_NATIVE_CELL_KIND_RB2D_TILED_COPY_QUALIFICATION;
   cmd.ib = ib;
   cmd.ib_size_dwords = ib_dwords;
   cmd.references = references;
   cmd.reference_count = 2u;
   cmd.rb2d_tiled_copy_configured = true;
   if (!qualification_byte_copy)
      cmd.rb2d_tiled_copy_request = request;
   cmd.rb2d_tiled_copy_write_mask = qualification_write_mask;
   if (qualification_byte_copy) {
      cmd.rb2d_copy_geometry = R3V_NATIVE_RB2D_COPY_GEOMETRY_SEGMENTS;
      cmd.rb2d_copy_segment_count = 1;
      cmd.rb2d_copy_segments[0] = qualification_byte_span;
      cmd.rb2d_copy_source_buffer_bytes = QUALIFICATION_BYTES;
      cmd.rb2d_copy_destination_buffer_bytes = QUALIFICATION_BYTES;
      cmd.rb2d_copy_byte_carrier = true;
   }
   cmd.rb2d_copy_operations = calloc(1u, sizeof(*cmd.rb2d_copy_operations));
   if (cmd.rb2d_copy_operations == NULL) {
      free(ib);
      return 1;
   }
   cmd.rb2d_copy_operation_count = 1u;
   cmd.rb2d_copy_operation_capacity = 1u;
   cmd.rb2d_copy_operations[0] = (struct r3v_native_rb2d_copy_operation){
      .source_memory = &source,
      .destination_memory = &destination,
      .source_buffer_bytes = QUALIFICATION_BYTES,
      .destination_buffer_bytes = QUALIFICATION_BYTES,
      .write_mask = qualification_write_mask,
      .byte_carrier = qualification_byte_copy,
   };
   if (qualification_byte_copy) {
      cmd.rb2d_copy_operations[0].segment_count = 1u;
      cmd.rb2d_copy_operations[0].segments[0] = qualification_byte_span;
   } else {
      struct r300_zb_tile_copy_plan tile_plan;
      struct r300_rb2d_copy_segment segments[R300_RB2D_COPY_MAX_SEGMENTS];
      struct r300_rb2d_copy_plan copy_plan;
      if (r300_zb_tile_copy_plan_build(&request, &tile_plan) != R300_ZB_TILE_COPY_OK ||
          r300_rb2d_copy_plan_from_zb_tile(
             &tile_plan, QUALIFICATION_BYTES, QUALIFICATION_BYTES, false,
             segments, &copy_plan) != R300_RB2D_COPY_OK) {
         free(cmd.rb2d_copy_operations);
         free(ib);
         return 1;
      }
      cmd.rb2d_copy_operations[0].segment_count = copy_plan.segment_count;
      memcpy(cmd.rb2d_copy_operations[0].segments, copy_plan.segments,
             copy_plan.segment_count * sizeof(copy_plan.segments[0]));
   }
   if (!r3v_native_rb2d_tiled_copy_geometry_valid(&cmd)) {
      free(cmd.rb2d_copy_operations);
      free(ib);
      return 1;
   }

#define REFUSES(statement)                                                   \
   do {                                                                      \
      statement;                                                             \
      if (r3v_native_rb2d_tiled_copy_geometry_valid(&cmd))                  \
         return 1;                                                           \
      cmd = valid;                                                           \
      *cmd.rb2d_copy_operations = valid_operation;                          \
      memcpy(ib, pristine_ib, (size_t)ib_dwords * sizeof(*ib));              \
      references[0] = valid_references[0];                                   \
      references[1] = valid_references[1];                                   \
   } while (0)
   const struct r3v_native_cmd_buffer valid = cmd;
   const struct r3v_native_rb2d_copy_operation valid_operation =
      cmd.rb2d_copy_operations[0];
   const struct r3v_native_bo_reference valid_references[2] = {
      references[0], references[1]
   };
   uint32_t *pristine_ib = malloc((size_t)ib_dwords * sizeof(*pristine_ib));
   if (pristine_ib == NULL)
      return 1;
   memcpy(pristine_ib, ib, (size_t)ib_dwords * sizeof(*ib));

   REFUSES(ib[0] ^= 1u);
   REFUSES(references[0].read_domains = 0u);
   REFUSES(references[1].write_domain = 0u);
   REFUSES(references[1].handle = references[0].handle);
   REFUSES(destination.bo.size--);
   destination.bo.size = QUALIFICATION_BYTES;
   REFUSES(cmd.rb2d_tiled_copy_configured = false);
   REFUSES(cmd.rb2d_tiled_copy_write_mask ^= 1u;
           cmd.rb2d_copy_operations[0].write_mask ^= 1u);
   if (qualification_byte_copy) {
      REFUSES(cmd.rb2d_copy_segments[0].source_offset_bytes++;
              cmd.rb2d_copy_operations[0].segments[0].source_offset_bytes++);
      REFUSES(cmd.rb2d_copy_segments[0].destination_offset_bytes++;
              cmd.rb2d_copy_operations[0].segments[0].destination_offset_bytes++);
      REFUSES(cmd.rb2d_copy_segments[0].byte_count++;
              cmd.rb2d_copy_operations[0].segments[0].byte_count++);
      REFUSES(cmd.rb2d_copy_segment_count = 0;
              cmd.rb2d_copy_operations[0].segment_count = 0);
      REFUSES(cmd.rb2d_copy_byte_carrier = false;
              cmd.rb2d_copy_operations[0].byte_carrier = false);
   } else {
      REFUSES(cmd.rb2d_tiled_copy_request.destination.macro_x ^= 1u);
   }
#undef REFUSES

   uint32_t depth_mismatches = 0u;
   uint32_t stencil_mismatches = 0u;
   count_packed_component_mismatches(0x123456abu, 0x123456cdu,
                                     &depth_mismatches,
                                     &stencil_mismatches);
   if (depth_mismatches != 0u || stencil_mismatches != 1u)
      return 1;
   count_packed_component_mismatches(0x133456cdu, 0x123456cdu,
                                     &depth_mismatches,
                                     &stencil_mismatches);
   if (depth_mismatches != 1u || stencil_mismatches != 1u)
      return 1;

   free(pristine_ib);
   free(cmd.rb2d_copy_operations);
   free(ib);
   printf("r3v_native_rb2d_tiled_copy_qualification: selftest passed\n");
   return 0;
}

static uint32_t
expected_destination_word(const struct r300_zb_tile_copy_request *request,
                          uint32_t word_index)
{
   const uint64_t destination_offset =
      (uint64_t)word_index * sizeof(uint32_t);
   if (qualification_byte_copy) {
      uint32_t expected = destination_word(word_index);
      for (uint32_t lane = 0; lane < 4u; lane++) {
         const uint64_t offset = destination_offset + lane;
         if (offset < qualification_byte_span.destination_offset_bytes ||
             offset - qualification_byte_span.destination_offset_bytes >=
                qualification_byte_span.byte_count)
            continue;
         const uint64_t source_offset = qualification_byte_span.source_offset_bytes +
            offset - qualification_byte_span.destination_offset_bytes;
         const uint32_t source_byte =
            (source_word(source_offset / 4u) >> (8u * (source_offset % 4u))) & 0xffu;
         expected = (expected & ~(0xffu << (8u * lane))) |
                    (source_byte << (8u * lane));
      }
      return expected;
   }
   if (destination_offset >= request->destination.tile_offset_bytes &&
       destination_offset < request->destination.tile_offset_bytes +
                               R300_ZB_TILE_COPY_BYTES) {
      const uint32_t parity_xor =
         ((request->source.macro_x ^ request->destination.macro_x) & 1u) *
            1024u |
         ((request->source.macro_y ^ request->destination.macro_y) & 1u) *
            512u;
      const uint64_t source_offset =
         request->source.tile_offset_bytes +
         ((destination_offset - request->destination.tile_offset_bytes) ^
          parity_xor);
      return (source_word((uint32_t)(source_offset / sizeof(uint32_t))) &
              qualification_write_mask) |
             (destination_word(word_index) & ~qualification_write_mask);
   }
   return destination_word(word_index);
}

static VkResult
record_copy(VkCommandBuffer command, VkDeviceMemory source,
            VkDeviceMemory destination,
            const struct r300_zb_tile_copy_request *request)
{
   if (qualification_byte_copy) {
      const struct r300_rb2d_copy_plan plan = byte_plan();
      return r3v_native_record_rb2d_copy(command, source, destination, &plan,
                                       qualification_write_mask);
   }
   return r3v_native_record_rb2d_tiled_copy_masked(
      command, source, destination, request, qualification_write_mask);
}

static int
run_hardware(const char *evidence_directory,
             const struct r300_zb_tile_copy_request *request)
{
   int exit_code = 2;
   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkDeviceMemory source = VK_NULL_HANDLE;
   VkDeviceMemory destination = VK_NULL_HANDLE;
   VkCommandPool pool = VK_NULL_HANDLE;
   void *initial_source = NULL;
   void *initial_destination = NULL;
   void *source_map = NULL;
   void *destination_map = NULL;
   PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *) = NULL;
   PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = NULL;
   const char *declared_directory = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (getenv("LD_PRELOAD") != NULL || declared_directory == NULL ||
       strcmp(declared_directory, evidence_directory) != 0) {
      fprintf(stderr, "hardware preflight refused the process environment\n");
      goto cleanup;
   }
   get_instance_proc = vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)get_instance_proc(NULL, "vkCreateInstance");
   if (create_instance == NULL)
      goto cleanup;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO },
      NULL, &instance);
   if (result != VK_SUCCESS)
      goto cleanup;
#define LOAD_INSTANCE(name)                                                  \
   PFN_##name name = (PFN_##name)get_instance_proc(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   vkGetDeviceProcAddr =
      (PFN_vkGetDeviceProcAddr)get_instance_proc(instance,
                                                  "vkGetDeviceProcAddr");
   if (vkEnumeratePhysicalDevices == NULL ||
       vkGetPhysicalDeviceProperties == NULL || vkCreateDevice == NULL ||
       vkGetDeviceProcAddr == NULL)
      goto cleanup;

   uint32_t physical_count = 1u;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || physical_count != 1u)
      goto cleanup;
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(physical, &properties);
   if (properties.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       properties.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE)
      goto cleanup;
   const float priority = 1.0f;
   result = vkCreateDevice(
      physical,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1u,
         .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0u,
            .queueCount = 1u,
            .pQueuePriorities = &priority,
         },
      },
      NULL, &device);
   if (result != VK_SUCCESS)
      goto cleanup;
#define LOAD_DEVICE(name)                                                    \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   if (vkAllocateMemory == NULL || vkMapMemory == NULL ||
       vkUnmapMemory == NULL || vkCreateCommandPool == NULL ||
       vkAllocateCommandBuffers == NULL || vkBeginCommandBuffer == NULL ||
       vkEndCommandBuffer == NULL || vkGetDeviceQueue == NULL ||
       vkQueueSubmit == NULL)
      goto cleanup;

   const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = QUALIFICATION_BYTES,
      .memoryTypeIndex = 0u,
   };
   if (vkAllocateMemory(device, &allocation, NULL, &source) != VK_SUCCESS ||
       vkAllocateMemory(device, &allocation, NULL, &destination) != VK_SUCCESS ||
       source == destination)
      goto cleanup;
   if (vkMapMemory(device, source, 0u, VK_WHOLE_SIZE, 0u, &initial_source) !=
          VK_SUCCESS ||
       vkMapMemory(device, destination, 0u, VK_WHOLE_SIZE, 0u,
                   &initial_destination) != VK_SUCCESS ||
       initial_source == NULL || initial_destination == NULL)
      goto cleanup;
   uint32_t *initial_source_words = initial_source;
   uint32_t *initial_destination_words = initial_destination;
   for (uint32_t word_index = 0;
        word_index < QUALIFICATION_BYTES / sizeof(uint32_t); word_index++) {
      initial_source_words[word_index] = source_word(word_index);
      initial_destination_words[word_index] = destination_word(word_index);
   }
   if (r3v_native_evidence_write_file(evidence_directory, "source-before.bin",
                                      initial_source, QUALIFICATION_BYTES) != 0 ||
       r3v_native_evidence_write_file(evidence_directory,
                                      "destination-before.bin",
                                      initial_destination,
                                      QUALIFICATION_BYTES) != 0)
      goto cleanup;
   vkUnmapMemory(device, source);
   initial_source = NULL;
   vkUnmapMemory(device, destination);
   initial_destination = NULL;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0u,
          },
          NULL, &pool) != VK_SUCCESS)
      goto cleanup;
   VkCommandBuffer command = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1u,
          },
          &command) != VK_SUCCESS ||
       vkBeginCommandBuffer(
          command,
          &(VkCommandBufferBeginInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
          }) != VK_SUCCESS ||
       record_copy(command, source, destination, request) != VK_SUCCESS ||
       vkEndCommandBuffer(command) != VK_SUCCESS)
      goto cleanup;
   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0u, 0u, &queue);
   result = vkQueueSubmit(
      queue, 1u,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1u,
         .pCommandBuffers = &command,
      },
      VK_NULL_HANDLE);
   const enum r3v_native_queue_status status =
      r3v_native_queue_submission_status(device);

   if (vkMapMemory(device, source, 0u, VK_WHOLE_SIZE, 0u, &source_map) !=
          VK_SUCCESS ||
       vkMapMemory(device, destination, 0u, VK_WHOLE_SIZE, 0u,
                   &destination_map) != VK_SUCCESS ||
       source_map == NULL || destination_map == NULL)
      goto cleanup;
   if (r3v_native_evidence_write_file(evidence_directory, "source.bin",
                                      source_map, QUALIFICATION_BYTES) != 0 ||
       r3v_native_evidence_write_file(evidence_directory, "destination.bin",
                                      destination_map,
                                      QUALIFICATION_BYTES) != 0)
      goto cleanup;

   const uint32_t *source_words = source_map;
   const uint32_t *destination_words = destination_map;
   uint32_t source_mismatches = 0u;
   uint32_t destination_mismatches = 0u;
   uint32_t depth_mismatches = 0u;
   uint32_t stencil_mismatches = 0u;
   uint32_t target_mismatches = 0u;
   uint32_t selected_byte_mismatches = 0u;
   uint32_t exterior_byte_mismatches = 0u;
   uint32_t prefix_guard_mismatches = 0u;
   uint32_t untargeted_grid_mismatches = 0u;
   uint32_t tail_guard_mismatches = 0u;
   for (uint32_t word_index = 0;
        word_index < QUALIFICATION_BYTES / sizeof(uint32_t); word_index++) {
      if (source_words[word_index] !=
          source_word(word_index))
         source_mismatches++;
      const uint32_t expected = expected_destination_word(request, word_index);
      if (destination_words[word_index] != expected) {
         destination_mismatches++;
         count_packed_component_mismatches(
            destination_words[word_index], expected, &depth_mismatches,
            &stencil_mismatches);
         const uint64_t byte_offset =
            (uint64_t)word_index * sizeof(uint32_t);
         if (qualification_byte_copy) {
            for (unsigned byte = 0; byte < 4; byte++) {
               if (((destination_words[word_index] ^ expected) >> (8u * byte) &
                    0xffu) == 0u)
                  continue;
               const uint64_t offset = byte_offset + byte;
               if (offset >= qualification_byte_span.destination_offset_bytes &&
                   offset - qualification_byte_span.destination_offset_bytes <
                      qualification_byte_span.byte_count)
                  selected_byte_mismatches++;
               else
                  exterior_byte_mismatches++;
            }
         }
         if (qualification_byte_copy ?
                (byte_offset + 4u > qualification_byte_span.destination_offset_bytes &&
                 byte_offset < qualification_byte_span.destination_offset_bytes +
                                  qualification_byte_span.byte_count) :
                (byte_offset >= request->destination.tile_offset_bytes &&
             byte_offset < request->destination.tile_offset_bytes +
                              R300_ZB_TILE_COPY_BYTES))
            target_mismatches++;
         else if (byte_offset < QUALIFICATION_GUARD_BYTES)
            prefix_guard_mismatches++;
         else if (byte_offset < QUALIFICATION_GUARD_BYTES +
                                   4u * R300_ZB_TILE_COPY_BYTES)
            untargeted_grid_mismatches++;
         else
            tail_guard_mismatches++;
      }
   }
   if (qualification_byte_copy)
      printf("selected_byte_mismatches=%u exterior_byte_mismatches=%u\n",
             selected_byte_mismatches, exterior_byte_mismatches);
   vkUnmapMemory(device, source);
   source_map = NULL;
   vkUnmapMemory(device, destination);
   destination_map = NULL;
   printf("submit_result=%d queue_status=%s source_mismatches=%u "
          "destination_mismatches=%u depth_mismatches=%u "
          "stencil_mismatches=%u target_mismatches=%u "
          "prefix_guard_mismatches=%u untargeted_grid_mismatches=%u "
          "tail_guard_mismatches=%u\n",
          result, r3v_native_queue_status_name(status), source_mismatches,
          destination_mismatches, depth_mismatches, stencil_mismatches,
          target_mismatches, prefix_guard_mismatches,
          untargeted_grid_mismatches, tail_guard_mismatches);
   exit_code =
      result == VK_SUCCESS && status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            source_mismatches == 0u && destination_mismatches == 0u
         ? 0
         : 1;

cleanup:
   if (device != VK_NULL_HANDLE && vkGetDeviceProcAddr != NULL) {
      PFN_vkUnmapMemory cleanup_unmap =
         (PFN_vkUnmapMemory)vkGetDeviceProcAddr(device, "vkUnmapMemory");
      PFN_vkDestroyCommandPool cleanup_destroy_pool =
         (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(
            device, "vkDestroyCommandPool");
      PFN_vkFreeMemory cleanup_free_memory =
         (PFN_vkFreeMemory)vkGetDeviceProcAddr(device, "vkFreeMemory");
      PFN_vkDestroyDevice cleanup_destroy_device =
         (PFN_vkDestroyDevice)vkGetDeviceProcAddr(device, "vkDestroyDevice");
      if (cleanup_unmap != NULL) {
         if (source_map != NULL || initial_source != NULL)
            cleanup_unmap(device, source);
         if (destination_map != NULL || initial_destination != NULL)
            cleanup_unmap(device, destination);
      }
      if (pool != VK_NULL_HANDLE && cleanup_destroy_pool != NULL)
         cleanup_destroy_pool(device, pool, NULL);
      if (source != VK_NULL_HANDLE && cleanup_free_memory != NULL)
         cleanup_free_memory(device, source, NULL);
      if (destination != VK_NULL_HANDLE && cleanup_free_memory != NULL)
         cleanup_free_memory(device, destination, NULL);
      if (cleanup_destroy_device != NULL)
         cleanup_destroy_device(device, NULL);
   }
   if (instance != VK_NULL_HANDLE && get_instance_proc != NULL) {
      PFN_vkDestroyInstance cleanup_destroy_instance =
         (PFN_vkDestroyInstance)get_instance_proc(instance,
                                                   "vkDestroyInstance");
      if (cleanup_destroy_instance != NULL)
         cleanup_destroy_instance(instance, NULL);
   }
   return exit_code;
}

int
main(int argc, char **argv)
{
   if (argc > 1 && strcmp(argv[1], "--byte-span") == 0) {
      if (argc != 6)
         return 2;
      uint64_t values[3];
      for (unsigned index = 0; index < 3; index++) {
         const char *text = argv[index + 2];
         char *end;
         if (*text < '0' || *text > '9')
            return 2;
         errno = 0;
         values[index] = strtoull(text, &end, 10);
         if (errno || *end)
            return 2;
      }
      if (values[2] == 0 || values[2] > 256)
         return 2;
      qualification_byte_copy = true;
      qualification_byte_span = (struct r300_rb2d_copy_segment){
         .source_offset_bytes = values[0],
         .destination_offset_bytes = values[1],
         .byte_count = (uint32_t)values[2],
      };
      if (strcmp(argv[5], "--prepare") == 0)
         return prepare_all();
      if (strcmp(argv[5], "--selftest") == 0)
         return selftest();
      if (argv[5][0] == '-')
         return 2;
      const struct r300_zb_tile_copy_request request = request_for(0, 0, 0, 0);
      return run_hardware(argv[5], &request);
   }
   if (argc > 1 && (strcmp(argv[1], "--depth-only") == 0 ||
                   strcmp(argv[1], "--stencil-only") == 0)) {
      qualification_write_mask = strcmp(argv[1], "--depth-only") == 0
                                    ? 0xffffff00u : 0x000000ffu;
      argc--;
      argv++;
   }
   if (argc == 2 && strcmp(argv[1], "--prepare") == 0)
      return prepare_all();
   if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
      return selftest();
   if (argc != 6) {
      fprintf(stderr,
              "usage: %s --prepare | --selftest | <evidence-dir> sx sy dx dy\n",
              argv[0]);
      return 2;
   }
   uint32_t source_x, source_y, destination_x, destination_y;
   if (!parse_parity(argv[2], &source_x) ||
       !parse_parity(argv[3], &source_y) ||
       !parse_parity(argv[4], &destination_x) ||
       !parse_parity(argv[5], &destination_y))
      return 2;
   const struct r300_zb_tile_copy_request request =
      request_for(source_x, source_y, destination_x, destination_y);
   return run_hardware(argv[1], &request);
}
