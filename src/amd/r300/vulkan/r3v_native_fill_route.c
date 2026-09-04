/*
 * SPDX-License-Identifier: MIT
 *
 * The public fill route: one vkCmdFillBuffer recorded as a semantic
 * operation, resolved to an executor at the submission boundary, and --
 * when every gate admits it -- emitted as the PM4 the 2D engine performs.
 *
 * The admitted shape is one submit, one command buffer whose whole content
 * is a single vkCmdFillBuffer, one buffer object, one span segment on the
 * 256-byte carrier, at most three rectangles, one relocation entry, and one
 * completion.  Anything wider leaves the command buffer untouched for the
 * host path, so mixed host and device transfer ordering belongs to the
 * execution graph that will own it rather than to this file.
 *
 * The order is the whole safety argument, and it is a transaction rather
 * than a convention.  Prepare builds every fallible thing -- shape
 * admission, memory contract, route resolution, span decomposition, IB
 * sizing, allocation, emission, relocation validation.  Validate runs every
 * gate -- the frozen-cell predicate, the arming verdict over the stream
 * this route just built, the operator's declared submission identity, the
 * provenance record.  Commit alone installs the
 * stream and marks the record.  r3v_submit_transaction_record_effect
 * admits an effect in the commit phase alone and
 * r3v_submit_transaction_refuse rejects a refusal that follows one, so a
 * gate moved past the install fails the transaction rather than silently
 * writing the destination by neither executor.
 *
 * The arming verdict is a routing precondition rather than a later gate.
 * The submission path this route hands a stream to opens on the exact-value
 * hazard gate; a route that installed its stream and then met a closed gate
 * would leave a fill the host had already been told to skip and the device
 * never ran.  Asking the gate here means the ordinary closed-gate state
 * declines the route and the host performs the fill it was going to
 * perform.
 *
 * The transaction is this route's own.  The submission boundary creates
 * none, so the ordering proved here covers the route's effects on the
 * command buffer and states nothing about the submit around it.
 *
 * The whole-submit preflight r3v_submit_preflight_check performs has no
 * verdict to give here: every shape it refuses -- a second executable
 * command buffer, a second route candidate, a candidate with no stream,
 * recorded work the candidate does not cover, GPU_ONLY with no candidate --
 * this route's own admission refuses earlier and by name.  The submit-wide
 * accounting belongs to the submission boundary, over the buffers this
 * route never sees.
 */

#include "r3v_fill_route.h"
#include "r3v_memory_properties_contract.h"
#include "r3v_native.h"
#include "r3v_physical_device.h"
#include "r3v_route_policy.h"
#include "r3v_submit_preflight.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/blake3/blake3.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* The values r3v_fill_route.h spells so its admission builds without the
 * API headers, held to the spellings they stand for. */
static_assert(R3V_FILL_ROUTE_USAGE_TRANSFER_DST ==
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              "the fill route's usage bit is VK_BUFFER_USAGE_TRANSFER_DST_BIT");
static_assert(R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE ==
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
              "the fill route's memory property is "
              "VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT");
static_assert(R3V_FILL_ROUTE_DOMAIN_GTT == RADEON_GEM_DOMAIN_GTT,
              "the fill route's write domain is RADEON_GEM_DOMAIN_GTT");

/* One IB carries one segment.  A segment covers R300_RB2D_SAFE_EXCLUSIVE_END
 * rows before DST_WIDTH_HEIGHT would ask past the scissor the emitter opens,
 * so on this route's 256-byte carrier a 1 KiB-aligned destination offset
 * reaches 8191 * 256 bytes.  A longer interval decomposes into ordered
 * segments the planner already produces; carrying several in one stream
 * needs the relocation accounting and mid-stream refusal review this
 * contract has not run, so the route declines the range and the host path
 * carries it. */
#define R3V_NATIVE_FILL_ROUTE_MAX_SEGMENTS 1u

/* One submit, one command buffer: the arming authorization, the retained
 * evidence, and the completion all describe one transport. */
#define R3V_NATIVE_FILL_ROUTE_COMMAND_BUFFERS 1u

/* Whether the command buffer's whole content is one buffer fill.
 *
 * The copy count is this route's own fact -- exactly one, rather than the
 * "no copies" every other work kind is held to -- so it is tested here.
 * Every other kind rides r3v_native_cmd_buffer_has_other_recorded_work,
 * which reads the recorded-work census, so a work kind added to the command
 * buffer reaches this admission without a second field list to maintain.
 */
static bool
shape_is_one_fill(const struct r3v_native_cmd_buffer *cmd_buffer,
                  const struct r3v_native_deferred_copy **op_out)
{
   if (cmd_buffer->ib_size_dwords != 0 ||
       cmd_buffer->deferred_copy_count != 1 ||
       r3v_native_cmd_buffer_has_other_recorded_work(cmd_buffer))
      return false;

   const struct r3v_native_deferred_copy *op = &cmd_buffer->deferred_copies[0];
   if (op->kind != R3V_NATIVE_COPY_FILL_BUFFER || op->dst_buffer == NULL ||
       op->dst_buffer->memory == NULL)
      return false;
   *op_out = op;
   return true;
}

/* Everything the prepare phase built, released together on every path out
 * of it.  The stream survives the release when the commit phase takes it,
 * which install_ib does by taking ownership. */
struct fill_route_build {
   struct r300_rb2d_fill_plan *plans;
   struct r300_rb2d_fill_rect *rects;
   uint32_t *ib;
   uint32_t ib_dwords;
   struct r3v_fill_route_reloc_site sites[R300_RB2D_FILL_SLOT_COUNT];
   uint32_t site_count;
   uint32_t rect_count;
   uint32_t segment_count;
};

static void
build_release(struct fill_route_build *b)
{
   free(b->plans);
   free(b->rects);
   free(b->ib);
   memset(b, 0, sizeof(*b));
}

/* A route this command buffer's shape, range, or deployment does not admit.
 * The host path performs the fill, except under GPU_ONLY, where refusing is
 * the answer the policy exists to give. */
static VkResult
decline(struct r3v_native_device *device, enum r3v_execution_policy policy,
        const char *what, const char *reason)
{
   if (policy != R3V_EXECUTION_GPU_ONLY)
      return VK_SUCCESS;
   return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                    "r3v-native: fill route %s: %s", what,
                    reason != NULL ? reason : "unnamed");
}

/* Returns a command buffer this route claimed on an earlier submission to
 * the shape it was recorded in.
 *
 * A Vulkan command buffer is submittable more than once, and this route's
 * install is a submission-scoped effect: the arming authorization, the
 * declared submission identity, and the one-shot evidence directory each
 * describe one submission.  A second submit that found the first's stream
 * still installed would carry a spent authorization to the device, or
 * refuse a fill the host had already been told to skip.  Retiring the
 * install here runs the whole admission again for every submission, and
 * the record goes with the transport it described.
 */
static void
retire_previous_submission(struct r3v_native_cmd_buffer *cmd)
{
   r3v_native_cmd_buffer_release_ib(cmd);
   for (uint32_t i = 0; i < cmd->deferred_copy_count; i++)
      cmd->deferred_copies[i].gpu_routed = false;
   cmd->fill_route_provenance = (struct r3v_execution_provenance){ 0 };
   cmd->fill_route_active = false;
}

VkResult
r3v_native_cmd_buffer_route_deferred_fill(struct r3v_native_device *device,
                                          struct r3v_native_cmd_buffer *cmd,
                                          uint32_t submit_command_buffers)
{
   const struct r3v_native_deferred_copy *op = NULL;
   const enum r3v_execution_policy policy = device->execution_policy;

   if (cmd != NULL && cmd->fill_route_active)
      retire_previous_submission(cmd);

   if (cmd == NULL || !shape_is_one_fill(cmd, &op)) {
      /* A shape this route does not admit is not a refusal: GPU_ONLY's
       * refusal belongs to a request the route was asked to answer, not to
       * every command buffer that reaches the boundary. */
      return VK_SUCCESS;
   }
   if (submit_command_buffers != R3V_NATIVE_FILL_ROUTE_COMMAND_BUFFERS) {
      return decline(device, policy, "declines the submit shape",
                     "a routed submit carries one command buffer");
   }

   struct r3v_submit_transaction transaction;
   r3v_submit_transaction_begin(&transaction);
   const char *reason = NULL;

   const struct r3v_native_buffer *dst = op->dst_buffer;
   const struct r3v_native_memory *memory = dst->memory;

   /* The allocation's properties are its memory type's, and the driver's
    * own table is the authority for what each index carries. */
   VkPhysicalDeviceMemoryProperties memory_properties;
   r3v_native_memory_properties_fill(&memory_properties, memory->bo.size);
   const uint32_t property_flags =
      memory->vk.memory_type_index < memory_properties.memoryTypeCount
         ? memory_properties.memoryTypes[memory->vk.memory_type_index]
              .propertyFlags
         : 0u;

   /* The memory contract, ahead of every rectangle: the buffer is bound and
    * carries the transfer-destination usage, the range counts whole dwords
    * inside both the VkBuffer and its VkDeviceMemory, the far edge from the
    * relocated base stays inside DST_PITCH_OFFSET's address envelope, and
    * the allocation is host-visible GTT so the oracle reads the result back
    * through the mapping its sentinel was written through. */
   const struct r3v_fill_route_memory contract = {
      .bound = true,
      .buffer_usage = dst->vk.usage,
      .memory_property_flags = property_flags,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .buffer_bytes = dst->vk.size,
      .binding_offset = dst->offset,
      .memory_bytes = memory->bo.size,
      .fill_offset = op->dst_offset,
      .fill_bytes = op->size,
   };
   enum r3v_fill_route_refusal admission =
      r3v_fill_route_memory_check(&contract);
   if (admission != R3V_FILL_ROUTE_ADMITTED) {
      return decline(device, policy, "declines the destination",
                     r3v_fill_route_refusal_name(admission));
   }

   bool gate_open[R300_OPERATION_ROUTE_COUNT] = { false };
   if (!r3v_route_gate_state_from_cache(device->compute_route_gates,
                                        gate_open,
                                        R300_OPERATION_ROUTE_COUNT)) {
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route cannot read the device's "
                       "route gates");
   }

   const struct r3v_route_request request = {
      .operation_id = R300_OPERATION_ID_CONSTFILL,
      .use = R300_ROUTE_USE_TRANSFER_BUFFER,
      .policy = policy,
      .byte_offset = op->dst_offset,
      .byte_size = op->size,
      .element_bytes = R3V_FILL_ROUTE_ELEMENT_BYTES,
      .destination_device_visible = true,
      .destination_host_mapped = true,
   };

   const struct r300_operation_route_row *route = NULL;
   const enum r3v_route_decision decision =
      r3v_route_policy_select(&request, gate_open, &route, &reason);
   if (decision == R3V_ROUTE_DECISION_REFUSE) {
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v-native: fill route refused: %s",
                       reason != NULL ? reason : "no reason recorded");
   }
   if (decision != R3V_ROUTE_DECISION_GPU) {
      /* The gate is closed or the policy asked for the host, so the route
       * is not this operation's executor and the host path performs it. */
      return VK_SUCCESS;
   }

   /* The relocated base is the buffer's binding inside its memory, so the
    * span runs from there and the containment rule measures against the
    * allocation the relocation carries. */
   const struct r300_rb2d_span span = {
      .byte_offset = dst->offset + op->dst_offset,
      .byte_size = op->size,
      .value = op->clear_dword,
   };
   /* The 256-byte carrier is the row the retained direct-write control
    * stream exercises, so this route's plan differs from that witnessed
    * stream in its rectangle list alone; ARGB8888 is the one format whose
    * pixel is the span's 32-bit pattern. */
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };

   struct fill_route_build build = { 0 };
   enum r300_rb2d_span_refusal span_refusal = R300_RB2D_SPAN_OK;
   build.segment_count = r300_rb2d_linear_span_segments(
      &span, &layout, memory->bo.size, &span_refusal);
   if (build.segment_count == 0 ||
       build.segment_count > R3V_NATIVE_FILL_ROUTE_MAX_SEGMENTS) {
      const char *why =
         build.segment_count == 0
            ? r300_rb2d_span_refusal_name(span_refusal)
            : "the range needs more segments than one stream carries";
      return decline(device, policy, "cannot represent the range", why);
   }

   build.plans = calloc(build.segment_count, sizeof(*build.plans));
   build.rects = calloc((size_t)build.segment_count *
                           R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT,
                        sizeof(*build.rects));
   if (build.plans == NULL || build.rects == NULL) {
      build_release(&build);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (r300_rb2d_linear_span_plan(&span, &layout, memory->bo.size,
                                  build.plans, build.rects,
                                  build.segment_count,
                                  &span_refusal) != build.segment_count) {
      build_release(&build);
      return decline(device, policy, "decomposition refused",
                     r300_rb2d_span_refusal_name(span_refusal));
   }
   for (uint32_t s = 0; s < build.segment_count; s++)
      build.rect_count += build.plans[s].rect_count;

   /* A false return names storage the caller sized from this same segment
    * count or a rectangle count the fill plan already admitted, so it is an
    * internal failure rather than a range the carrier cannot name. */
   if (!r300_rb2d_linear_span_dwords(build.plans, build.segment_count,
                                     &build.ib_dwords) ||
       build.ib_dwords == 0) {
      build_release(&build);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route could not cost its own "
                       "decomposition");
   }

   build.ib = calloc(build.ib_dwords, sizeof(*build.ib));
   if (build.ib == NULL) {
      build_release(&build);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   /* Each segment emits into its own window of the one stream and names the
    * destination once, so the relocation sites accumulate across the stream
    * rather than per plan. */
   uint32_t at = 0;
   for (uint32_t s = 0; s < build.segment_count; s++) {
      struct r300_rb2d_fill_ib segment;
      if (r300_rb2d_fill_emit_into(&build.plans[s], build.ib + at,
                                   build.ib_dwords - at, &segment) != 0 ||
          r300_rb2d_fill_validate_reloc_sites(&segment) != 0) {
         build_release(&build);
         return decline(device, policy, "emission refused",
                        "the plan did not emit its own validated stream");
      }
      for (uint32_t r = 0; r < segment.reloc_site_count; r++) {
         if (build.site_count >= ARRAY_SIZE(build.sites)) {
            build_release(&build);
            return decline(device, policy, "emission refused",
                           "the stream names more relocation sites than one "
                           "destination takes");
         }
         build.sites[build.site_count].ib_index =
            at + segment.reloc_sites[r].ib_index;
         build.sites[build.site_count].slot = segment.reloc_sites[r].slot;
         build.site_count++;
      }
      at += segment.ib_size_dwords;
   }
   if (at != build.ib_dwords || build.site_count == 0) {
      build_release(&build);
      return decline(device, policy, "emission refused",
                     "the emitted stream does not fill the length it was "
                     "sized to");
   }

   /* Every fallible construction is behind; the gates run next and the
    * install after them. */
   if (!r3v_submit_transaction_advance(&transaction,
                                       R3V_SUBMIT_PHASE_VALIDATE, &reason)) {
      build_release(&build);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route transaction: %s", reason);
   }

   /* The cell this route is about to install, held to the shape its kind
    * freezes.  r3v_native_cell_geometry_unfrozen builds the same struct
    * from the installed command buffer, so the predicate that admits the
    * submission is the predicate the arming gate judges it by. */
   const struct r3v_fill_route_cell cell = {
      .copy_count = 1,
      .reference_count = 1,
      .copy_is_fill = true,
      .gpu_routed = true,
      .destination_bound = true,
      .reference_names_destination = true,
      .fill_offset = op->dst_offset,
      .fill_bytes = op->size,
      .buffer_bytes = dst->vk.size,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
   };
   /* Every field above is either this route's own constant or a value the
    * memory contract already admitted, so a false here contradicts a rule
    * that already held rather than describing a destination the caller
    * named.  It reports device loss for that reason: no host path answers
    * an internal contradiction. */
   if (!r3v_fill_route_cell_frozen(&cell)) {
      build_release(&build);
      return vk_errorf(
         device, VK_ERROR_DEVICE_LOST,
         "r3v-native: fill route built a cell its own kind refuses: %s",
         r3v_fill_route_refusal_name(R3V_FILL_ROUTE_REFUSE_CELL_UNFROZEN));
   }

   /* The arming verdict over the stream this route just built.  The
    * submission path opens on the exact-value hazard gate and one
    * authorization names one stream digest, so a route that installed
    * ahead of this verdict would hand the boundary a stream it refuses --
    * with the host already told to skip the fill and no ioctl to replace
    * it.  Asking here leaves the closed-gate state on the host path.
    */
   if (!device->submit_hazard_accepted || device->manifest_dir == NULL) {
      build_release(&build);
      return decline(device, policy, "declines the deployment",
                     "the submission gate is closed or names no evidence "
                     "directory");
   }

   char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
   char kernel_release[128];
   char module_srcversion[128];
   struct r3v_native_arming_facts facts = { 0 };
   r300_triangle_ib_digest_hex(build.ib, build.ib_dwords, ib_digest);
   r3v_native_arming_collect_from(
      device->arming_provider != NULL ? device->arming_provider
                                      : r3v_native_arming_host_provider(),
      &facts, device->pdevice->pci_vendor_id, device->pdevice->pci_device_id,
      R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC, ib_digest, device->manifest_dir,
      kernel_release, sizeof(kernel_release), module_srcversion,
      sizeof(module_srcversion));
   facts.nonmaximum_extent = false;
   const enum r3v_native_arming_verdict arming =
      r3v_native_arming_evaluate(&facts);
   if (arming != R3V_NATIVE_ARMING_ARMED) {
      build_release(&build);
      return decline(device, policy, "declines the arming state",
                     r3v_native_arming_verdict_name(arming));
   }

   /* The operator's declared submission identity.  The arming digest binds
    * the register stream; this binds the allocation, the carrier, the
    * decomposition, the stream length, the relocation sites, the buffer
    * object's role, and the deployment epoch, so one authorization names
    * one submission and the same stream against a different destination is
    * a different one. */
   const struct r3v_fill_route_identity identity = {
      .allocation_bytes = memory->bo.size,
      .buffer_bytes = dst->vk.size,
      .binding_offset = dst->offset,
      .fill_offset = op->dst_offset,
      .fill_bytes = op->size,
      .fill_value = op->clear_dword,
      .pitch_bytes = layout.pitch_bytes,
      .format = (uint32_t)layout.format,
      .segment_count = build.segment_count,
      .rect_count = build.rect_count,
      .rects = build.rects,
      .ib_dwords = build.ib_dwords,
      .ib = build.ib,
      .relocation_count = build.site_count,
      .reloc_sites = build.sites,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .destination_handle = memory->bo.handle,
      .kernel_release = facts.running_kernel_release,
      .module_srcversion = facts.running_module_srcversion,
   };
   char actual_identity[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   admission = r3v_fill_route_authority_check(
      &identity, device->authorized_fill_identity, actual_identity, &reason);
   if (admission != R3V_FILL_ROUTE_ADMITTED) {
      build_release(&build);
      return decline(device, policy,
                     r3v_fill_route_refusal_name(admission),
                     actual_identity[0] != '\0' ? actual_identity : reason);
   }

   /* The record a hardware claim rests on, validated before anything
    * touches the command buffer.  The route prepared a stream and nothing
    * has reached the kernel, so the phase is PREPARED and the submission
    * flag is false; the submission boundary advances both when its ioctl
    * runs. */
   const struct r3v_execution_provenance provenance = {
      .operation_id = route->operation_id,
      .route_id = route->route_id,
      .unit = route->unit,
      .executor = route->executor,
      .route_state = route->state,
      .phase = R3V_EXECUTION_PHASE_PREPARED,
      .host_semantic_node = false,
      .device_submission = false,
      .experimental_admission =
         route->state != R300_OPERATION_ROUTE_EXECUTING,
      .ib_dwords = build.ib_dwords,
      .relocation_count = build.site_count,
   };
   if (!r3v_execution_provenance_valid(&provenance, policy, &reason)) {
      build_release(&build);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route provenance is invalid: %s",
                       reason != NULL ? reason : "unnamed");
   }

   struct r3v_native_bo_reference *references = calloc(1, sizeof(*references));
   if (references == NULL) {
      build_release(&build);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   /* One destination, written by the device and read by nothing in the
    * stream, so the reference carries a write domain and no read domain. */
   references[0] = (struct r3v_native_bo_reference){
      .handle = memory->bo.handle,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .memory = dst->memory,
   };

   if (!r3v_submit_transaction_advance(&transaction, R3V_SUBMIT_PHASE_COMMIT,
                                       &reason)) {
      free(references);
      build_release(&build);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route transaction: %s", reason);
   }

   /* The install takes the stream and the reference list, so the build
    * releases what it still owns and never the stream. */
   uint32_t *ib = build.ib;
   const uint32_t ib_dwords = build.ib_dwords;
   build.ib = NULL;
   build_release(&build);

   if (!r3v_submit_transaction_record_effect(&transaction, &reason) ||
       !r3v_submit_transaction_record_effect(&transaction, &reason)) {
      free(references);
      free(ib);
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "r3v-native: fill route transaction: %s", reason);
   }
   r3v_native_cmd_buffer_install_ib(cmd, R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC,
                                    ib, ib_dwords, references, 1u);
   /* Both commit together, after the stream installs: a marked record with
    * no installed stream is a fill nobody performs, and the transfer path
    * skips a marked record rather than falling back to the host. */
   cmd->deferred_copies[0].gpu_routed = true;
   cmd->fill_route_provenance = provenance;
   cmd->fill_route_active = true;

   return VK_SUCCESS;
}
