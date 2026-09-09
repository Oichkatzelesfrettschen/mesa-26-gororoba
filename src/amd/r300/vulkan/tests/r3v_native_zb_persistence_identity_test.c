/*
 * SPDX-License-Identifier: MIT
 */

#undef NDEBUG
#include "r3v_native.h"

#include "amd/r300/common/r300_zb_depth_control_cell.h"

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
   references[R300_ZB_DEPTH_CONTROL_SLOT_VERTEX].memory = vertex;
   references[R300_ZB_DEPTH_CONTROL_SLOT_COLOR].memory = color;
   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory =
      ordinal == R3V_NATIVE_ZB_PERSISTENCE_B ? depth_b : depth_a;
   command->cell_kind = R3V_NATIVE_CELL_KIND_ZB_TILED_PERSISTENCE_SERIAL;
   command->ib = ib;
   command->ib_size_dwords = ib_dwords;
   command->references = references;
   command->reference_count = 3;
   command->zb_persistence_configured = true;
   command->zb_persistence_ordinal = ordinal;
   command->zb_persistence_depth_a = depth_a;
   command->zb_persistence_depth_b = depth_b;
}

int
main(void)
{
   struct r3v_native_memory vertex = {.bo.handle = 11, .generation = 101};
   struct r3v_native_memory color = {.bo.handle = 12, .generation = 102};
   struct r3v_native_memory depth_a = {.bo.handle = 13, .generation = 103};
   struct r3v_native_memory depth_b = {.bo.handle = 14, .generation = 104};
   uint32_t ib[] = {0x01020304, 0x11223344};
   struct r3v_native_bo_reference references[3] = {0};
   struct r3v_native_cmd_buffer command;
   bind_command(&command, references, &vertex, &color, &depth_a, &depth_b,
                R3V_NATIVE_ZB_PERSISTENCE_A_FIRST, ib, 2);
   struct r3v_native_zb_persistence_identity identity = {0};
   r3v_native_zb_persistence_identity_capture(&identity, &command, "digest-a");

   bind_command(&command, references, &vertex, &color, &depth_a, &depth_b,
                R3V_NATIVE_ZB_PERSISTENCE_B, ib, 2);
   assert(r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                     "digest-a", 1));
   command.zb_persistence_ordinal = R3V_NATIVE_ZB_PERSISTENCE_A_FINAL;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                      "digest-a", 1));
   command.zb_persistence_ordinal = R3V_NATIVE_ZB_PERSISTENCE_B;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                      "digest-b", 1));
   command.ib_size_dwords++;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                      "digest-a", 1));
   command.ib_size_dwords--;

#define MUTATE_FIELD(object, field) do {                                     \
   (object).field++;                                                          \
   assert(!r3v_native_zb_persistence_identity_matches(                       \
      &identity, &command, "digest-a", 1));                                  \
   (object).field--;                                                          \
} while (0)
   MUTATE_FIELD(vertex, generation);
   MUTATE_FIELD(color, generation);
   MUTATE_FIELD(depth_a, generation);
   MUTATE_FIELD(depth_b, generation);
   MUTATE_FIELD(vertex.bo, handle);
   MUTATE_FIELD(color.bo, handle);
   MUTATE_FIELD(depth_a.bo, handle);
   MUTATE_FIELD(depth_b.bo, handle);
#undef MUTATE_FIELD

   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory = &depth_a;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                      "digest-a", 1));
   references[R300_ZB_DEPTH_CONTROL_SLOT_DEPTH].memory = &depth_b;
   command.zb_persistence_depth_a = &depth_b;
   assert(!r3v_native_zb_persistence_identity_matches(&identity, &command,
                                                      "digest-a", 1));

   printf("r3v_native_zb_persistence_identity_test: all checks passed\n");
   return 0;
}
