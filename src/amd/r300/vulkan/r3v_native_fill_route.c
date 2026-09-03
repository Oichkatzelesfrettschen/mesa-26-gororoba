/*
 * SPDX-License-Identifier: MIT
 *
 * The public fill route: vkCmdFillBuffer recorded as a semantic operation,
 * resolved to an executor at queue preparation, and -- when the RB2D route
 * answers -- emitted as PM4 the 2D engine performs.
 *
 * Recording stays side-effect-free, so this runs at submission where the
 * binding, the range, and the gates are the ones live at execution.  The
 * order inside it is the whole safety argument: every fallible step --
 * shape admission, route resolution, span decomposition, IB sizing,
 * allocation, emission, relocation validation -- completes before the
 * stream is installed, and the host fill for a routed record is skipped in
 * r3v_native_transfer.c before it maps anything.  A record that reaches
 * this file and leaves it routed is one the device performs; a record that
 * fails any step here leaves the command buffer exactly as it was and the
 * host path performs it, except under GPU_ONLY, where the refusal is the
 * answer.
 *
 * The admitted command shape is deliberately one: a command buffer whose
 * whole content is a single vkCmdFillBuffer, with no render pass, draw,
 * dispatch, query, event, or second transfer.  That gives the queue one
 * unambiguous transport and leaves mixed host and device transfer ordering
 * to the execution graph that will own it.
 */

#include "r3v_native.h"
#include "r3v_route_policy.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"

#include <stdlib.h>
#include <string.h>

/* One IB carries exactly one segment.  A segment covers
 * R300_RB2D_SAFE_EXCLUSIVE_END (8191) rows before DST_WIDTH_HEIGHT's row
 * count would ask past the scissor the emitter opens, so at this route's
 * 256-byte carrier (R300_RB2D_SPAN_PITCH_DIRECT_WRITE) a 1 KiB-aligned
 * destination offset reaches 8191 * 256 = 2,096,896 bytes -- 256 bytes
 * short of 2 MiB, the one row R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT's
 * partial-first-row rectangle gives up when the offset does not land on
 * the carrier's own row boundary.  Emitting several segments into one IB
 * to cover a longer range is a later revision of this route's contract,
 * not an omission here: r300_rb2d_linear_span_plan already produces an
 * ordered multi-segment decomposition, and nothing in the emission loop
 * below assumes segments == 1, but this file has not yet run the
 * relocation-accounting and mid-stream-refusal review a multi-segment IB
 * needs before it is trusted at submission. */
#define R3V_NATIVE_FILL_ROUTE_MAX_SEGMENTS 1u

/* Whether the command buffer's whole content is one fill.  Anything else
 * shares the queue with work this route does not order.
 *
 * The copy count is this route's own fact (exactly one, not the "no
 * copies" r3v_native_cmd_buffer_has_other_recorded_work leaves alone), so
 * it is tested here directly; every other work kind -- draw, dispatch,
 * query op, event op -- is tested through that one shared function, the
 * same field set r3v_native_queue_prepare_submission's inline-ordering
 * check reads.  A work kind added to the command buffer without a
 * matching edit there already breaks that check's own correctness, so
 * this route inherits the fix instead of carrying a second, independently
 * maintained list that a new kind can slip past (as deferred_dispatch.pending
 * did here before this fix: it was absent from this file's own list while
 * already present in the shared one). */
static bool
shape_is_one_fill(const struct r3v_native_cmd_buffer *cmd_buffer,
                  const struct r3v_native_deferred_copy **op_out)
{
   if (cmd_buffer->ib_size_dwords != 0 || cmd_buffer->deferred_copy_count != 1 ||
       r3v_native_cmd_buffer_has_other_recorded_work(cmd_buffer))
      return false;

   const struct r3v_native_deferred_copy *op = &cmd_buffer->deferred_copies[0];
   if (op->kind != R3V_NATIVE_COPY_FILL_BUFFER || op->dst_buffer == NULL ||
       op->dst_buffer->memory == NULL)
      return false;
   *op_out = op;
   return true;
}

VkResult
r3v_native_cmd_buffer_route_deferred_fill(struct r3v_native_device *device,
                                          struct r3v_native_cmd_buffer *cmd)
{
   const struct r3v_native_deferred_copy *op = NULL;
   const enum r3v_execution_policy policy = device->execution_policy;

   if (cmd == NULL || !shape_is_one_fill(cmd, &op)) {
      /* A shape this route does not admit is not a refusal: the host path
       * performs it, and GPU_ONLY's refusal belongs to a request the route
       * was asked to answer rather than to every command buffer. */
      return VK_SUCCESS;
   }

   bool gate_open[R300_OPERATION_ROUTE_COUNT] = { false };
   for (uint32_t r = 0; r < R300_OPERATION_ROUTE_COUNT; r++)
      gate_open[r] = device->compute_route_gates[r] != NULL;

   const struct r3v_route_request request = {
      .operation_id = R300_OPERATION_ID_CONSTFILL,
      .use = R300_ROUTE_USE_TRANSFER_BUFFER,
      .policy = policy,
      .byte_offset = op->dst_offset,
      .byte_size = op->size,
      .element_bytes = 4u,
      .destination_device_visible = true,
      .destination_host_mapped = op->dst_buffer->memory->map != NULL,
   };

   const struct r300_operation_route_row *route = NULL;
   const char *reason = NULL;
   const enum r3v_route_decision decision =
      r3v_route_policy_select(&request, gate_open, &route, &reason);

   if (decision == R3V_ROUTE_DECISION_REFUSE) {
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v-native: fill route refused: %s",
                       reason != NULL ? reason : "no reason recorded");
   }
   if (decision != R3V_ROUTE_DECISION_GPU)
      return VK_SUCCESS;

   /* The relocated base is the buffer's binding inside its memory, so the
    * span the plan names runs from there and the footprint rule measures
    * against the memory the relocation carries. */
   const struct r3v_native_buffer *dst = op->dst_buffer;
   const struct r3v_native_span_context {
      uint64_t base;
      uint64_t bytes;
   } memory = { dst->offset, dst->memory->bo.size };

   const struct r300_rb2d_span span = {
      .byte_offset = memory.base + op->dst_offset,
      .byte_size = op->size,
      .value = op->clear_dword,
   };

   /* The carrier is the 256-byte row the retained direct-write control
    * stream exercises (R300_RB2D_SPAN_PITCH_DIRECT_WRITE), so this route's
    * plan differs from that witnessed stream in its rectangle list alone;
    * ARGB8888 is the one format whose pixel is the span's 32-bit pattern. */
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };

   enum r300_rb2d_span_refusal span_refusal = R300_RB2D_SPAN_OK;
   const uint32_t segments = r300_rb2d_linear_span_segments(
      &span, &layout, memory.bytes, &span_refusal);
   if (segments == 0 || segments > R3V_NATIVE_FILL_ROUTE_MAX_SEGMENTS) {
      const char *why = segments == 0
                           ? r300_rb2d_span_refusal_name(span_refusal)
                           : "span needs more segments than one IB carries";
      if (policy == R3V_EXECUTION_GPU_ONLY) {
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v-native: fill route cannot represent the "
                          "range: %s", why != NULL ? why : "unnamed");
      }
      /* AUTO keeps the host path for a range the carrier cannot name. */
      return VK_SUCCESS;
   }

   struct r300_rb2d_fill_plan *plans = calloc(segments, sizeof(*plans));
   struct r300_rb2d_fill_rect *rects =
      calloc((size_t)segments * R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT,
             sizeof(*rects));
   if (plans == NULL || rects == NULL) {
      free(plans);
      free(rects);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (r300_rb2d_linear_span_plan(&span, &layout, memory.bytes, plans, rects,
                                  segments, &span_refusal) != segments) {
      free(plans);
      free(rects);
      if (policy == R3V_EXECUTION_GPU_ONLY) {
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v-native: fill route decomposition refused: %s",
                          r300_rb2d_span_refusal_name(span_refusal));
      }
      return VK_SUCCESS;
   }

   /* A false return is an internal failure -- storage the caller sized
    * from this same segments count, or a rectangle count the fill plan
    * already admitted -- never a host fallback, so it reports device loss
    * rather than returning VK_SUCCESS to the host path. */
   uint32_t dwords = 0;
   if (!r300_rb2d_linear_span_dwords(plans, segments, &dwords) ||
       dwords == 0) {
      free(plans);
      free(rects);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route could not cost its own "
                       "decomposition");
   }

   uint32_t *ib = calloc(dwords, sizeof(*ib));
   if (ib == NULL) {
      free(plans);
      free(rects);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Each segment emits into its own window of the one IB, and every
    * segment names the destination once, so the relocation sites accumulate
    * across the stream rather than per plan. */
   uint32_t at = 0;
   uint32_t reloc_total = 0;
   bool emitted = true;
   for (uint32_t s = 0; s < segments && emitted; s++) {
      struct r300_rb2d_fill_ib segment_ib;
      const uint32_t room = dwords - at;
      if (r300_rb2d_fill_emit_into(&plans[s], ib + at, room, &segment_ib) !=
             0 ||
          r300_rb2d_fill_validate_reloc_sites(&segment_ib) != 0) {
         emitted = false;
         break;
      }
      at += segment_ib.ib_size_dwords;
      reloc_total += segment_ib.reloc_site_count;
   }

   free(plans);
   free(rects);

   if (!emitted || at != dwords) {
      free(ib);
      if (policy == R3V_EXECUTION_GPU_ONLY) {
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v-native: fill route emission did not fill the "
                          "sized stream");
      }
      return VK_SUCCESS;
   }

   struct r3v_native_bo_reference *references =
      calloc(1, sizeof(*references));
   if (references == NULL) {
      free(ib);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   /* One destination, written by the device and read by nothing, so the
    * reference carries a write domain and no read domain. */
   references[0] = (struct r3v_native_bo_reference){
      .handle = dst->memory->bo.handle,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = dst->memory,
   };

   r3v_native_cmd_buffer_install_ib(cmd, R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC,
                                    ib, dwords, references, 1u);

   /* The record is marked after the stream is installed, so the two move
    * together: a marked record without an installed IB would be a fill
    * nobody performs, and r3v_native_transfer.c refuses that pairing rather
    * than falling back to the host. */
   cmd->deferred_copies[0].gpu_routed = true;

   cmd->fill_route_provenance = (struct r3v_execution_provenance){
      .operation_id = route->operation_id,
      .route_id = route->route_id,
      .unit = route->unit,
      .executor = route->executor,
      .route_state = route->state,
      .host_semantic_node = false,
      .device_submission = true,
      .experimental_admission =
         route->state != R300_OPERATION_ROUTE_EXECUTING,
      .ib_dwords = dwords,
      .relocation_count = reloc_total,
   };
   cmd->fill_route_active = true;

   const char *provenance_reason = NULL;
   if (!r3v_execution_provenance_valid(&cmd->fill_route_provenance, policy,
                                       &provenance_reason)) {
      /* The record cannot be unwound here without leaving the command
       * buffer half-routed, so a provenance the policy rejects is a device
       * fault rather than a fallback. */
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route provenance is invalid: %s",
                       provenance_reason != NULL ? provenance_reason
                                                 : "unnamed");
   }

   return VK_SUCCESS;
}
