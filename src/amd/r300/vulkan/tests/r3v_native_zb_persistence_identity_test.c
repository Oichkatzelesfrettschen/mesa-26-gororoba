/*
 * SPDX-License-Identifier: MIT
 */

#undef NDEBUG
#include "r3v_native.h"

#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/common/r300_reg.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
bind_command(struct r3v_native_cmd_buffer *command,
             struct r3v_native_bo_reference references[3],
             struct r3v_native_memory *vertex,
             struct r3v_native_memory *color,
             struct r3v_native_memory *depth_a,
             struct r3v_native_memory *depth_b,
             enum r3v_native_zb_persistence_ordinal ordinal,
             uint32_t *ib, uint32_t ib_dwords)
{
   memset(command, 0, sizeof(*command));
   references[R300_ZB_DEPTH_CONTROL_SLOT_VERTEX].handle = vertex->bo.handle;
   references[R300_ZB_DEPTH_CONTROL_SLOT_VERTEX].memory = vertex;
   references[R300_ZB_DEPTH_CONTROL_SLOT_COLOR].handle = color->bo.handle;
   references[R300_ZB_DEPTH_CONTROL_SLOT_COLOR].memory = color;
   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].handle =
      ordinal == R3V_NATIVE_ZB_PERSISTENCE_B ? depth_b->bo.handle
                                             : depth_a->bo.handle;
   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory =
      ordinal == R3V_NATIVE_ZB_PERSISTENCE_B ? depth_b : depth_a;
   command->cell_kind = R3V_NATIVE_CELL_KIND_ZB_TILED_PERSISTENCE_SERIAL;
   command->ib = ib;
   command->ib_size_dwords = ib_dwords;
   command->references = references;
   command->reference_count = 3;
   command->zb_persistence_configured = true;
   command->zb_persistence_ordinal = ordinal;
   command->zb_persistence_vertex = vertex;
   command->zb_persistence_vertex_generation = vertex->generation;
   command->zb_persistence_vertex_handle = vertex->bo.handle;
   command->zb_persistence_depth_a = depth_a;
   command->zb_persistence_depth_b = depth_b;
}

static void
test_public_persistence_binding(void)
{
   struct r3v_native_memory vertex = {
      .vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY,
      .bo = { .handle = 21, .size = 4096 },
   };
   struct r3v_native_memory color = {
      .vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY,
      .bo = { .handle = 22, .size = R300_ZB_DEPTH_CONTROL_COLOR_BYTES },
   };
   const uint32_t depth_bytes = r3v_native_zb_depth_surface_bytes(
      R3V_NATIVE_ZB_DEPTH_SURFACE_RS485M_Z24_MACROTILED_LOGICAL);
   struct r3v_native_memory depth_a = {
      .vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY,
      .bo = { .handle = 23, .size = depth_bytes },
   };
   struct r3v_native_memory depth_b = {
      .vk.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY,
      .bo = { .handle = 24, .size = depth_bytes },
   };
   struct r3v_native_depth_image_contract contract = {
      .surface = r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
   };
   struct r3v_native_bo_reference references[3] = {
      { .handle = 21, .read_domains = RADEON_GEM_DOMAIN_GTT,
        .memory = &vertex },
      { .handle = 22, .write_domain = RADEON_GEM_DOMAIN_GTT,
        .memory = &color },
      { .handle = 23, .read_domains = RADEON_GEM_DOMAIN_GTT,
        .write_domain = RADEON_GEM_DOMAIN_GTT, .memory = &depth_a },
   };
   struct r3v_native_deferred_draw draw = {
      .pending = true,
      .target_width = 64,
      .target_height = 64,
      .depth_memory = &depth_a,
      .depth_bound = { .contract = &contract },
      .depth_pipeline = {
         .hardware = { .depth_function = R300_ZS_LESS },
         .depth_test_enable = true,
      },
      .has_depth_pipeline = true,
   };
   struct r3v_native_cmd_buffer command = {
      .vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER,
      .cell_kind = R3V_NATIVE_CELL_KIND_TRIANGLE,
      .references = references,
      .reference_count = 3,
      .deferred_draw_count = 1,
      .deferred_draw_capacity = 1,
      .deferred_draws = &draw,
   };
   assert(r3v_native_bind_zb_tiled_persistence(
             r3v_native_cmd_buffer_to_handle(&command),
             r3v_native_memory_to_handle(&depth_a),
             r3v_native_memory_to_handle(&depth_b),
             R3V_NATIVE_ZB_PERSISTENCE_A_FIRST) == VK_SUCCESS);
   assert(command.cell_kind ==
          R3V_NATIVE_CELL_KIND_ZB_TILED_PERSISTENCE_SERIAL);
   assert(!r3v_native_cell_geometry_unfrozen(&command));

   command.deferred_draws[0].depth_pipeline.hardware.depth_write = true;
   assert(r3v_native_cell_geometry_unfrozen(&command));
   command.deferred_draws[0].depth_pipeline.hardware.depth_write = false;
   command.cell_kind = R3V_NATIVE_CELL_KIND_TRIANGLE;
   command.zb_persistence_configured = false;
   assert(r3v_native_bind_zb_tiled_persistence(
             r3v_native_cmd_buffer_to_handle(&command),
             r3v_native_memory_to_handle(&depth_a),
             r3v_native_memory_to_handle(&depth_a),
             R3V_NATIVE_ZB_PERSISTENCE_A_FIRST) ==
          VK_ERROR_INITIALIZATION_FAILED);
   assert(command.cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE);
   assert(!command.zb_persistence_configured);
}

int
main(void)
{
   test_public_persistence_binding();
   struct r3v_native_memory vertices[3] = {
      {.bo.handle = 11, .generation = 101},
      {.bo.handle = 15, .generation = 105},
      {.bo.handle = 16, .generation = 106},
   };
   struct r3v_native_memory color = {.bo.handle = 12, .generation = 102};
   struct r3v_native_memory depth_a = {.bo.handle = 13, .generation = 103};
   struct r3v_native_memory depth_b = {.bo.handle = 14, .generation = 104};
   uint32_t ib[] = {0x01020304, 0x11223344};
   struct r3v_native_bo_reference references[3][3] = {0};
   struct r3v_native_cmd_buffer commands[3];
   bind_command(&commands[0], references[0], &vertices[0], &color, &depth_a,
                &depth_b,
                R3V_NATIVE_ZB_PERSISTENCE_A_FIRST, ib, 2);
   struct r3v_native_zb_persistence_identity identity = {0};
   r3v_native_zb_persistence_identity_capture(&identity, &commands[0],
                                              "digest-a");

   bind_command(&commands[1], references[1], &vertices[1], &color, &depth_a,
                &depth_b,
                R3V_NATIVE_ZB_PERSISTENCE_B, ib, 2);
   bind_command(&commands[2], references[2], &vertices[2], &color, &depth_a,
                &depth_b, R3V_NATIVE_ZB_PERSISTENCE_A_FINAL, ib, 2);
   assert(r3v_native_zb_persistence_identity_matches(&identity, &commands[1],
                                                     "digest-a", 1));
   assert(r3v_native_zb_persistence_identity_matches(&identity, &commands[2],
                                                     "digest-a", 2));
   struct r3v_native_cmd_buffer *command = &commands[1];
   command->zb_persistence_vertex = &vertices[0];
   assert(r3v_native_zb_persistence_identity_mismatches(
             &identity, command, "digest-a", 1) ==
          R3V_NATIVE_ZB_PERSISTENCE_IDENTITY_VERTEX_BINDING);
   command->zb_persistence_vertex = &vertices[1];
   references[1][R300_ZB_DEPTH_CONTROL_SLOT_VERTEX].handle++;
   assert(r3v_native_zb_persistence_identity_mismatches(
             &identity, command, "digest-a", 1) ==
          R3V_NATIVE_ZB_PERSISTENCE_IDENTITY_HANDLE);
   references[1][R300_ZB_DEPTH_CONTROL_SLOT_VERTEX].handle--;
   command->zb_persistence_ordinal = R3V_NATIVE_ZB_PERSISTENCE_A_FINAL;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, command,
                                                      "digest-a", 1));
   command->zb_persistence_ordinal = R3V_NATIVE_ZB_PERSISTENCE_B;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, command,
                                                      "digest-b", 1));
   command->ib_size_dwords++;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, command,
                                                      "digest-a", 1));
   command->ib_size_dwords--;

#define MUTATE_FIELD(object, field) do {                                     \
   (object).field++;                                                          \
   assert(!r3v_native_zb_persistence_identity_matches(                       \
      &identity, command, "digest-a", 1));                                   \
   (object).field--;                                                          \
} while (0)
   MUTATE_FIELD(vertices[1], generation);
   MUTATE_FIELD(color, generation);
   MUTATE_FIELD(depth_a, generation);
   MUTATE_FIELD(depth_b, generation);
   MUTATE_FIELD(vertices[1].bo, handle);
   MUTATE_FIELD(color.bo, handle);
   MUTATE_FIELD(depth_a.bo, handle);
   MUTATE_FIELD(depth_b.bo, handle);
#undef MUTATE_FIELD

   references[1][R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory = &depth_a;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, command,
                                                      "digest-a", 1));
   references[1][R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory = &depth_b;
   command->zb_persistence_depth_a = &depth_b;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, command,
                                                      "digest-a", 1));

   printf("r3v_native_zb_persistence_identity_test: all checks passed\n");
   return 0;
}
