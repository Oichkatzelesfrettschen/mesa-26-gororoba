/*
 * SPDX-License-Identifier: MIT
 *
 * Host checks for the public fill route's shape admission.  A command
 * buffer carrying exactly one vkCmdFillBuffer routes to the RB2D solid
 * brush; the same buffer with a pending compute dispatch ahead of the fill
 * must not.  r3v_native_copy_slot (r3v_native_recording.c) checks only
 * pass_target for a recorded copy, and r3v_CmdDispatch (r3v_native_compute.c)
 * refuses only when deferred_copy_count != 0 -- neither checks the other's
 * pending state -- so a command buffer can carry a pending dispatch and a
 * fill copy at once, and shape_is_one_fill must refuse that shape on its
 * own rather than relying on an upstream recorder to have blocked it. The
 * device, memory, and command buffer are stack storage: routing touches no
 * DRM node, so the checks run with no device present.
 */

#undef NDEBUG

#include "r3v_native.h"
#include "r3v_route_policy.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A destination large enough for the fill below with headroom the span
 * decomposition never approaches. */
#define TEST_MEMORY_BYTES (64u * 1024u)
/* One pitch row at the route's own 256-byte carrier
 * (R300_RB2D_SPAN_PITCH_DIRECT_WRITE): the whole-row rectangle arm, one
 * segment, no partial row. */
#define TEST_FILL_BYTES 256u

static void
build_valid_fill(struct r3v_native_memory *memory,
                 struct r3v_native_buffer *dst,
                 struct r3v_native_deferred_copy *copy,
                 struct r3v_native_cmd_buffer *cmd_buffer)
{
   *memory = (struct r3v_native_memory){
      .bo = { .handle = 0x77u, .size = TEST_MEMORY_BYTES },
   };
   *dst = (struct r3v_native_buffer){ .memory = memory, .offset = 0 };
   *copy = (struct r3v_native_deferred_copy){
      .kind = R3V_NATIVE_COPY_FILL_BUFFER,
      .dst_buffer = dst,
      .dst_offset = 0,
      .size = TEST_FILL_BYTES,
      .clear_dword = 0xcafebabeu,
   };
   *cmd_buffer = (struct r3v_native_cmd_buffer){
      .deferred_copies = copy,
      .deferred_copy_count = 1,
   };
}

/* Routes cmd_buffer and reports whether the RB2D route claimed it.  The
 * copy's own gpu_routed flag and fill_route_active move together under
 * this file's header contract (a marked record without an installed IB
 * is a fill nobody performs), so the two are held to agreement here
 * rather than trusted independently by each caller. */
static bool
routed(struct r3v_native_device *device,
      struct r3v_native_cmd_buffer *cmd_buffer)
{
   VkResult result =
      r3v_native_cmd_buffer_route_deferred_fill(device, cmd_buffer);
   assert(result == VK_SUCCESS);
   assert(cmd_buffer->deferred_copies[0].gpu_routed ==
          cmd_buffer->fill_route_active);
   return cmd_buffer->fill_route_active;
}

/* Calibration: this exact fill shape, alone, reaches the GPU route under
 * its own precommitted gate.  Without this positive case, a refusal below
 * could be a bail-out on unrelated missing setup (no gate, no candidate
 * row) rather than the dispatch check working. */
static void
test_single_fill_routes(void)
{
   setenv("R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL", "1", 1);

   struct r3v_native_memory memory;
   struct r3v_native_buffer dst;
   struct r3v_native_deferred_copy copy;
   struct r3v_native_cmd_buffer cmd_buffer;
   build_valid_fill(&memory, &dst, &copy, &cmd_buffer);

   struct r3v_native_device device = { .execution_policy = R3V_EXECUTION_AUTO };

   assert(routed(&device, &cmd_buffer));
   assert(cmd_buffer.ib_size_dwords != 0);
   assert(cmd_buffer.cell_kind == R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC);

   free(cmd_buffer.ib);
   free(cmd_buffer.references);
}

/* The reachable defect this test pins: a command buffer records a
 * dispatch, then the same fill test_single_fill_routes proves routes on
 * its own.  Before this route's shape admission read
 * deferred_dispatch.pending, that combination still routed to the GPU:
 * install_ib made ib_size_dwords nonzero, and the queue's deferred-dispatch
 * execution block only runs for a zero-IB command buffer, so the dispatch
 * was silently dropped while vkQueueSubmit reported success. */
static void
test_dispatch_then_fill_refuses_shape(void)
{
   setenv("R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL", "1", 1);

   struct r3v_native_memory memory;
   struct r3v_native_buffer dst;
   struct r3v_native_deferred_copy copy;
   struct r3v_native_cmd_buffer cmd_buffer;
   build_valid_fill(&memory, &dst, &copy, &cmd_buffer);
   cmd_buffer.deferred_dispatch.pending = true;

   struct r3v_native_device device = { .execution_policy = R3V_EXECUTION_AUTO };

   assert(!routed(&device, &cmd_buffer));
   /* Refused before install: the command buffer is exactly as it was, so
    * the host path executes the fill and the queue's deferred-dispatch
    * block executes the dispatch. */
   assert(cmd_buffer.ib_size_dwords == 0);
   assert(cmd_buffer.ib == NULL);
   assert(!cmd_buffer.deferred_copies[0].gpu_routed);
}

int
main(void)
{
   test_single_fill_routes();
   test_dispatch_then_fill_refuses_shape();
   printf("r3v_native_fill_route_test: all checks passed\n");
   return 0;
}
