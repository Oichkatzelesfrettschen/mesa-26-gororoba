/*
 * SPDX-License-Identifier: MIT
 *
 * The public fill route on the host: what it admits, what it declines, and
 * what the command buffer holds after each answer.
 *
 * The route's ordering is the property under test.  Prepare builds the
 * stream, validate runs every gate, and commit alone installs it, so a
 * declined route leaves the command buffer with no stream, no cell kind,
 * no relocation, and an unrouted copy -- which is what leaves the fill for
 * the host store loop to perform.  Every arm below drives the real route
 * and reads that state back, so a gate moved past the install fails here
 * rather than reading correct in the source.
 *
 * The reference submission is built independently in this file through the
 * same span and fill layers, and its stream digest and submission identity
 * are what the fixture authorizes.  A route that emitted any other stream,
 * decomposed the interval differently, or named a different destination
 * would fail the arming gate or the authority rather than pass silently.
 *
 * The host-exclusion legs run the real execute_copy path over a host
 * mapping the test owns: the routed record leaves the destination as it
 * found it and moves no counter, and the same record unrouted fills the
 * destination and moves the counter by exactly one.  Neither leg opens a
 * DRM node -- the mapping is the test's own storage and the route's own
 * work is arithmetic -- so the counter is the observation rather than a
 * reading of the source.
 */

#include "r3v_fill_route.h"
#include "r3v_native.h"
#include "r3v_physical_device.h"
#include "r3v_route_policy.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/list.h"
#include "util/mesa-blake3.h"

#include "vk_alloc.h"
#include "vk_command_pool.h"
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_physical_device.h"

#include <radeon_drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      if (!(cond)) {                                                         \
         failures++;                                                         \
         fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                              \
      }                                                                      \
   } while (0)

/* The attended cell the route's own document declares. */
#define CELL_ALLOCATION_BYTES (64u * 1024u)
#define CELL_FILL_OFFSET 12u
#define CELL_FILL_BYTES 4992u
#define CELL_FILL_VALUE 0x11223344u
#define CELL_SENTINEL 0xa5u
#define FIXTURE_KERNEL "6.16.0-fixture"
#define FIXTURE_SRCVERSION "FIXTURESRCVERSION0000000"
#define FIXTURE_EVIDENCE_DIR "/fixture-evidence"
#define SCENE_BO_HANDLE 0x77u

struct fixture {
   const char *hazard_gate;
   const char *authorized_ib_digest;
   bool evidence_dir_present;
   bool attempt_token_present;
};

static const char *
fixture_read_env(void *ctx, const char *name)
{
   const struct fixture *f = ctx;
   if (strcmp(name, "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED") == 0)
      return f->hazard_gate;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_IB_BLAKE3") == 0)
      return f->authorized_ib_digest;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE") == 0)
      return FIXTURE_KERNEL;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION") == 0)
      return FIXTURE_SRCVERSION;
   return NULL;
}

static void
fixture_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", FIXTURE_KERNEL);
}

static void
fixture_read_module_srcversion(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", FIXTURE_SRCVERSION);
}

static bool
fixture_directory_present(void *ctx, const char *path)
{
   const struct fixture *f = ctx;
   (void)path;
   return f->evidence_dir_present;
}

static bool
fixture_file_present(void *ctx, const char *path)
{
   const struct fixture *f = ctx;
   (void)path;
   return f->attempt_token_present;
}

/* The whole recorded world one arm drives: one allocation the test owns and
 * maps itself, one buffer bound at its base, one recorded fill, and the
 * command buffer that carries them. */
struct scene {
   struct r3v_native_memory memory;
   struct r3v_native_buffer buffer;
   struct r3v_native_deferred_copy *copy;
   struct r3v_native_cmd_buffer cmd;
   struct vk_command_pool pool;
   struct r3v_physical_device pdevice;
   struct r3v_native_device device;
   struct vk_instance instance;
   struct fixture fixture;
   uint8_t *storage;
};

/* The runtime object chain vk_errorf walks to reach an instance.  A refusal
 * under GPU_ONLY reports through that path, so the scene carries the three
 * links it reads -- object type, owning device, and instance -- and the two
 * empty callback lists that let the report return without a logging
 * backend. */
static void
scene_scaffold_vk_objects(struct scene *s)
{
   s->instance.base.type = VK_OBJECT_TYPE_INSTANCE;
   s->instance.base.client_visible = true;
   list_inithead(&s->instance.debug_utils.callbacks);
   list_inithead(&s->instance.debug_report.callbacks);

   s->pdevice.vk.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
   s->pdevice.vk.base.client_visible = true;
   s->pdevice.vk.instance = &s->instance;

   s->device.vk.base.type = VK_OBJECT_TYPE_DEVICE;
   s->device.vk.base.client_visible = true;
   s->device.vk.base.device = &s->device.vk;
   s->device.vk.physical = &s->pdevice.vk;
}

static void
scene_release(struct scene *s)
{
   free(s->cmd.ib);
   free(s->cmd.references);
   s->cmd.ib = NULL;
   s->cmd.references = NULL;
   /* The reset path frees the recording storage itself, so the scene
    * releases whatever is still bound. */
   vk_free(&s->pool.alloc, s->cmd.deferred_copies);
   s->cmd.deferred_copies = NULL;
   s->copy = NULL;
   free(s->storage);
   s->storage = NULL;
}

/* The reference submission, built here through the same layers the route
 * runs, so the fixture authorizes the exact stream and identity the route
 * must produce and nothing else. */
struct reference {
   char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
   char identity[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
};

static bool
build_reference(struct reference *ref)
{
   const struct r300_rb2d_span span = {
      .byte_offset = CELL_FILL_OFFSET,
      .byte_size = CELL_FILL_BYTES,
      .value = CELL_FILL_VALUE,
   };
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };
   struct r300_rb2d_fill_plan plans[1];
   struct r300_rb2d_fill_rect rects[R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   enum r300_rb2d_span_refusal refusal = R300_RB2D_SPAN_OK;
   if (r300_rb2d_linear_span_plan(&span, &layout, CELL_ALLOCATION_BYTES,
                                  plans, rects, 1, &refusal) != 1)
      return false;

   uint32_t dwords = 0;
   if (!r300_rb2d_linear_span_dwords(plans, 1, &dwords))
      return false;
   uint32_t *ib = calloc(dwords, sizeof(*ib));
   if (ib == NULL)
      return false;
   struct r300_rb2d_fill_ib emitted;
   if (r300_rb2d_fill_emit_into(&plans[0], ib, dwords, &emitted) != 0) {
      free(ib);
      return false;
   }
   r300_triangle_ib_digest_hex(ib, dwords, ref->ib_digest);

   struct r3v_fill_route_reloc_site sites[R300_RB2D_FILL_SLOT_COUNT];
   for (uint32_t r = 0; r < emitted.reloc_site_count; r++) {
      sites[r].ib_index = emitted.reloc_sites[r].ib_index;
      sites[r].slot = emitted.reloc_sites[r].slot;
   }
   const struct r3v_fill_route_identity id = {
      .allocation_bytes = CELL_ALLOCATION_BYTES,
      .buffer_bytes = CELL_ALLOCATION_BYTES,
      .binding_offset = 0,
      .fill_offset = CELL_FILL_OFFSET,
      .fill_bytes = CELL_FILL_BYTES,
      .fill_value = CELL_FILL_VALUE,
      .pitch_bytes = layout.pitch_bytes,
      .format = (uint32_t)layout.format,
      .segment_count = 1,
      .rect_count = plans[0].rect_count,
      .rects = rects,
      .ib_dwords = dwords,
      .ib = ib,
      .relocation_count = emitted.reloc_site_count,
      .reloc_sites = sites,
      .read_domains = 0,
      .write_domain = RADEON_GEM_DOMAIN_GTT,
      .destination_handle = SCENE_BO_HANDLE,
      .kernel_release = FIXTURE_KERNEL,
      .module_srcversion = FIXTURE_SRCVERSION,
   };
   const bool ok = r3v_fill_route_identity_digest(&id, ref->identity, NULL);
   free(ib);
   return ok;
}

/* One arm's world, every gate open and every declaration matching, so a
 * refusal below names the field the arm mutated. */
static bool
scene_init(struct scene *s, const struct reference *ref)
{
   memset(s, 0, sizeof(*s));
   s->storage = malloc(CELL_ALLOCATION_BYTES);
   if (s->storage == NULL)
      return false;
   memset(s->storage, CELL_SENTINEL, CELL_ALLOCATION_BYTES);

   s->memory.bo.handle = SCENE_BO_HANDLE;
   s->memory.bo.size = CELL_ALLOCATION_BYTES;
   /* A live application mapping, so the transfer path reuses it and reaches
    * no DRM node while still running its real store loop. */
   s->memory.map = s->storage;
   s->memory.vk.memory_type_index = 0;

   s->buffer.memory = &s->memory;
   s->buffer.offset = 0;
   s->buffer.vk.size = CELL_ALLOCATION_BYTES;
   s->buffer.vk.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

   /* The recording's own storage, from the pool allocator the command
    * buffer names, so the reset path frees what it is entitled to free. */
   s->pool.alloc = *vk_default_allocator();
   s->cmd.vk.pool = &s->pool;
   s->copy = vk_alloc(&s->pool.alloc, sizeof(*s->copy), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (s->copy == NULL)
      return false;
   *s->copy = (struct r3v_native_deferred_copy){
      .kind = R3V_NATIVE_COPY_FILL_BUFFER,
      .dst_buffer = &s->buffer,
      .dst_offset = CELL_FILL_OFFSET,
      .size = CELL_FILL_BYTES,
      .clear_dword = CELL_FILL_VALUE,
   };
   s->cmd.deferred_copies = s->copy;
   s->cmd.deferred_copy_capacity = 1;
   s->cmd.deferred_copy_count = 1;

   s->pdevice.pci_vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR;
   s->pdevice.pci_device_id = R3V_NATIVE_ARMING_PCI_DEVICE;
   scene_scaffold_vk_objects(s);

   s->fixture = (struct fixture){
      .hazard_gate = "1",
      .authorized_ib_digest = ref->ib_digest,
      .evidence_dir_present = true,
      .attempt_token_present = false,
   };
   static struct r3v_native_arming_provider provider;
   provider = (struct r3v_native_arming_provider){
      .read_env = fixture_read_env,
      .read_kernel_release = fixture_read_kernel_release,
      .read_module_srcversion = fixture_read_module_srcversion,
      .directory_present = fixture_directory_present,
      .file_present = fixture_file_present,
      .ctx = &s->fixture,
   };
   s->device.pdevice = &s->pdevice;
   s->device.arming_provider = &provider;
   /* The harness replaces the fact provider, so it declares the board it
    * stands in for; 1002:5974 alone resolves to no platform and the gate
    * refuses it. */
   s->device.arming_platform = R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M;
   s->device.execution_policy = R3V_EXECUTION_AUTO;
   s->device.submit_hazard_accepted = true;
   s->device.manifest_dir = FIXTURE_EVIDENCE_DIR;
   s->device.authorized_fill_identity = ref->identity;
   s->device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = "1";
   mtx_init(&s->device.drm.cache_event_mutex, mtx_plain);
   return true;
}

/* Whether the route claimed the command buffer, with the two facts a
 * claimed record carries held to agreement: a marked copy without an
 * installed stream is a fill nobody performs. */
static bool
route(struct scene *s, VkResult *result_out)
{
   const VkResult result = r3v_native_cmd_buffer_route_deferred_fill(
      &s->device, &s->cmd, 1u);
   if (result_out != NULL)
      *result_out = result;
   const bool claimed = s->cmd.fill_route_active;
   CHECK(s->copy->gpu_routed == claimed,
         "the routed flag and the active record disagree");
   CHECK(claimed == (s->cmd.ib_size_dwords != 0),
         "the active record and the installed stream disagree");
   return claimed;
}

/* The command buffer a declined route leaves: no stream, no cell kind, no
 * relocation, an unrouted copy, and a destination the route never touched.
 * This is the ordering property, read from state rather than from source. */
static void
check_untouched(const struct scene *s, const char *arm)
{
   CHECK(s->cmd.ib == NULL && s->cmd.ib_size_dwords == 0,
         "%s: the declined route installed a stream", arm);
   CHECK(s->cmd.references == NULL && s->cmd.reference_count == 0,
         "%s: the declined route installed a relocation", arm);
   CHECK(s->cmd.cell_kind == R3V_NATIVE_CELL_KIND_UNDECLARED,
         "%s: the declined route declared a cell kind", arm);
   CHECK(!s->copy->gpu_routed && !s->cmd.fill_route_active,
         "%s: the declined route marked the record", arm);
   CHECK(s->device.host_semantic_writes == 0,
         "%s: the route itself moved the host-write counter", arm);
   for (uint32_t i = 0; i < CELL_ALLOCATION_BYTES; i++) {
      if (s->storage[i] != CELL_SENTINEL) {
         CHECK(false, "%s: the declined route wrote byte %u", arm, i);
         return;
      }
   }
}

/* Calibration: the attended cell, every gate open, routes.  Without this
 * every refusal below could be a bail-out on unrelated missing setup. */
static void
test_attended_cell_routes(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   VkResult result = VK_SUCCESS;
   const bool claimed = route(&s, &result);
   CHECK(result == VK_SUCCESS, "the attended cell returns %d", result);
   CHECK(claimed, "the attended cell does not route");
   if (claimed) {
      CHECK(s.cmd.cell_kind == R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC,
            "the routed cell kind is %d", (int)s.cmd.cell_kind);
      CHECK(s.cmd.reference_count == 1, "the routed cell names %u buffers",
            s.cmd.reference_count);
      CHECK(s.cmd.references[0].write_domain == RADEON_GEM_DOMAIN_GTT &&
               s.cmd.references[0].read_domains == 0,
            "the routed relocation carries the wrong domains");
      CHECK(s.cmd.fill_route_provenance.route_id ==
               R300_OPERATION_ROUTE_RB2D_CONST_FILL,
            "the provenance names another route");
      CHECK(!s.cmd.fill_route_provenance.host_semantic_node,
            "the provenance reports a host semantic node");
      CHECK(!s.cmd.fill_route_provenance.experimental_admission,
            "an executing route reports an experimental admission");
      CHECK(s.cmd.fill_route_provenance.phase ==
               R3V_EXECUTION_PHASE_PREPARED,
            "the provenance claims a phase past preparation");
      /* The predicate that admitted the submission is the one the arming
       * gate judges the installed cell by, so the installed cell reads
       * frozen through the queue's own geometry fact. */
      CHECK(!r3v_native_cell_geometry_unfrozen(&s.cmd),
            "the arming gate reports the installed cell unfrozen");
      /* The same predicate reports a broken cell unfrozen, so the arm is a
       * real geometry contract rather than the default every unhandled
       * kind falls to.  Each mutation is restored before the next. */
      s.cmd.references[0].read_domains = RADEON_GEM_DOMAIN_GTT;
      CHECK(r3v_native_cell_geometry_unfrozen(&s.cmd),
            "a device-read destination reports the cell frozen");
      s.cmd.references[0].read_domains = 0;
      s.cmd.references[0].write_domain = 0;
      CHECK(r3v_native_cell_geometry_unfrozen(&s.cmd),
            "an unwritten destination reports the cell frozen");
      s.cmd.references[0].write_domain = RADEON_GEM_DOMAIN_GTT;
      s.copy->gpu_routed = false;
      CHECK(r3v_native_cell_geometry_unfrozen(&s.cmd),
            "an unrouted copy under this cell kind reports the cell frozen");
      s.copy->gpu_routed = true;
      s.cmd.reference_count = 2;
      CHECK(r3v_native_cell_geometry_unfrozen(&s.cmd),
            "a second relocation reports the cell frozen");
      s.cmd.reference_count = 1;
      s.copy->size = CELL_FILL_BYTES + 2;
      CHECK(r3v_native_cell_geometry_unfrozen(&s.cmd),
            "a range off the dword grid reports the cell frozen");
      s.copy->size = CELL_FILL_BYTES;
      CHECK(!r3v_native_cell_geometry_unfrozen(&s.cmd),
            "the restored cell no longer reads frozen");
      /* The stream the route built is the one the fixture authorized, so
       * the arming gate compared a live digest rather than a stale one. */
      char digest[BLAKE3_OUT_LEN * 2 + 1];
      r300_triangle_ib_digest_hex(s.cmd.ib, s.cmd.ib_size_dwords, digest);
      CHECK(strcmp(digest, ref->ib_digest) == 0,
            "the routed stream digest differs from the reference");
   }
   scene_release(&s);
}

/* The windowed contract executes beside the single-window one, so its own
 * gate selects it and the record it installs names the V2 route cell.  An
 * executing row reports no experimental admission, which is the promotion's
 * one visible consequence in a record. */
static void
test_windowed_route_routes(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = NULL;
   s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2] =
      "1";
   /* The receipted 256-byte carrier, pinned, so the arm measures the route
    * rather than the chooser. */
   s.device.rb2d_v2_pinned_pitch_bytes = 256u;

   VkResult result = VK_SUCCESS;
   const bool claimed = route(&s, &result);
   CHECK(result == VK_SUCCESS, "the windowed cell returns %d", result);
   CHECK(claimed, "the windowed cell does not route");
   if (claimed) {
      CHECK(s.cmd.cell_kind == R3V_NATIVE_CELL_KIND_RB2D_FILL_V2_ROUTE,
            "the routed cell kind is %d", (int)s.cmd.cell_kind);
      CHECK(s.cmd.fill_route_provenance.route_id ==
               R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2,
            "the provenance names another route");
      CHECK(s.cmd.fill_route_provenance.route_state ==
               R300_OPERATION_ROUTE_EXECUTING,
            "the provenance reports an unpromoted route");
      CHECK(!s.cmd.fill_route_provenance.experimental_admission,
            "an executing route reports an experimental admission");
   }
   scene_release(&s);
}

/* Both RB2D gates open name two executing contracts for one transfer
 * destination, and the route policy refuses the pair rather than ranking
 * it.  The refusal reaches the caller as a route refusal, so an AUTO
 * request does not quietly become a host fill. */
static void
test_both_rb2d_gates_open_refuse(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2] =
      "1";
   VkResult result = VK_SUCCESS;
   const bool claimed = route(&s, &result);
   CHECK(!claimed, "two open gates still route");
   CHECK(result == R3V_NATIVE_REFUSAL_RESULT,
         "two open gates return %d", result);
   check_untouched(&s, "two open RB2D route gates");
   scene_release(&s);
}

/* Each way the route declines, with the command buffer read back after
 * every one.  A mutation names one field of the calibrated scene. */
static void
test_declines_leave_the_command_buffer_untouched(const struct reference *ref)
{
   struct scene s;

#define ARM(name, mutate)                                                    \
   do {                                                                      \
      if (!scene_init(&s, ref)) {                                            \
         CHECK(false, "the scene does not build");                           \
         return;                                                             \
      }                                                                      \
      mutate;                                                                \
      VkResult armed_result = VK_SUCCESS;                                    \
      const bool claimed = route(&s, &armed_result);                         \
      CHECK(!claimed, "%s still routes", name);                              \
      CHECK(armed_result == VK_SUCCESS,                                      \
            "%s refuses under AUTO with %d", name, armed_result);            \
      if (!claimed)                                                          \
         check_untouched(&s, name);                                          \
      scene_release(&s);                                                     \
   } while (0)

   /* The route gate is the only door: a closed gate leaves the fill on the
    * host path whatever else stands open. */
   ARM("a closed route gate",
       s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] =
          NULL);
   ARM("a route gate holding \"0\"",
       s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] =
          "0");
   ARM("a route gate holding an empty value",
       s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] =
          "");

   /* The submission gate the route hands its stream to.  A route that
    * installed ahead of this would leave the fill for neither executor. */
   ARM("a closed submission gate", s.device.submit_hazard_accepted = false);
   ARM("no evidence directory", s.device.manifest_dir = NULL);
   ARM("a closed hazard gate in the collected facts",
       s.fixture.hazard_gate = NULL);
   ARM("an undeclared stream digest", s.fixture.authorized_ib_digest = NULL);
   ARM("a stream digest naming another stream",
       s.fixture.authorized_ib_digest =
          "0000000000000000000000000000000000000000000000000000000000000000");
   ARM("an absent evidence directory", s.fixture.evidence_dir_present = false);
   ARM("a spent attempt token", s.fixture.attempt_token_present = true);
   ARM("another chip", s.pdevice.pci_device_id = 0x5975u);

   /* The operator's declared submission identity. */
   ARM("no declared submission identity",
       s.device.authorized_fill_identity = NULL);
   ARM("a declared identity of the wrong width",
       s.device.authorized_fill_identity = "deadbeef");
   ARM("a declared identity naming another submission",
       s.device.authorized_fill_identity =
          "0000000000000000000000000000000000000000000000000000000000000000");
   /* The same geometry over a different buffer object is a different
    * submission, so the declaration built for the scene's own destination
    * admits nothing else. */
   ARM("another destination buffer object",
       s.memory.bo.handle = SCENE_BO_HANDLE + 1u);

   /* The memory contract. */
   ARM("a destination without transfer-destination usage",
       s.buffer.vk.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
   ARM("a range off the dword grid", s.copy->size = CELL_FILL_BYTES + 2);
   ARM("a range past the buffer", s.copy->size = CELL_ALLOCATION_BYTES * 2);
   ARM("a device-local allocation", s.memory.vk.memory_type_index = 1);

   /* The submit shape and the command-buffer shape. */
   ARM("a second recorded fill", s.cmd.deferred_copy_count = 2);
   ARM("a pending dispatch beside the fill",
       s.cmd.deferred_dispatch.pending = true);
   ARM("a recorded draw beside the fill", s.cmd.deferred_draw_count = 1);
   ARM("a recorded query op beside the fill", s.cmd.query_op_count = 1);
   ARM("a recorded event op beside the fill", s.cmd.event_op_count = 1);

   /* The board gate: 1002:5974 sits on desktop Xpress 1100 systems as well,
    * so an unresolved board and an id that disagrees with the resolved one
    * both leave the fill on the host path. */
   ARM("an unresolved board",
       s.device.arming_platform = R300_PLATFORM_ID_NONE);
   ARM("a vendor id the resolved board does not carry",
       s.pdevice.pci_vendor_id = 0x10de);
#undef ARM

   /* A command buffer that already carries a stream is another cell's, so
    * the route leaves it alone.  It stands outside the loop above because
    * its own precondition breaks the installed-stream agreement that loop
    * reads back. */
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.cmd.ib_size_dwords = 4;
   CHECK(r3v_native_cmd_buffer_route_deferred_fill(&s.device, &s.cmd, 1u) ==
            VK_SUCCESS,
         "a command buffer with an installed stream refuses under AUTO");
   CHECK(!s.cmd.fill_route_active && !s.copy->gpu_routed,
         "a command buffer with an installed stream still routes");
   CHECK(s.cmd.ib == NULL, "the route replaced an installed stream");
   scene_release(&s);

   /* A wider submit is the route's own contract rather than a command
    * buffer fact, so it is driven through the submit count directly. */
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   CHECK(r3v_native_cmd_buffer_route_deferred_fill(&s.device, &s.cmd, 2u) ==
            VK_SUCCESS,
         "a two-buffer submit refuses under AUTO");
   CHECK(!s.cmd.fill_route_active, "a two-buffer submit still routes");
   check_untouched(&s, "a two-buffer submit");
   scene_release(&s);
}

/* GPU_ONLY turns a decline into a refusal, which is the whole content of
 * the policy: a caller that asked for the device is told the device did not
 * run rather than handed a host result. */
static void
test_gpu_only_refuses_rather_than_falling_back(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.execution_policy = R3V_EXECUTION_GPU_ONLY;
   s.device.submit_hazard_accepted = false;
   const VkResult result =
      r3v_native_cmd_buffer_route_deferred_fill(&s.device, &s.cmd, 1u);
   CHECK(result == R3V_NATIVE_REFUSAL_RESULT,
         "GPU_ONLY over a closed submission gate returns %d", result);
   CHECK(!s.cmd.fill_route_active, "the refused route still claimed");
   check_untouched(&s, "GPU_ONLY over a closed submission gate");
   scene_release(&s);

   /* Under AUTO the same world declines silently and the host carries it,
    * so the refusal above belongs to the policy rather than to the gate. */
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.submit_hazard_accepted = false;
   CHECK(r3v_native_cmd_buffer_route_deferred_fill(&s.device, &s.cmd, 1u) ==
            VK_SUCCESS,
         "AUTO over a closed submission gate refuses");
   scene_release(&s);
}

/* GPU_ONLY on the admitted path: the policy that requires the device must
 * reach the device.  The prepared record carries device_submission = false
 * because no ioctl has run yet, so a policy check applied to it would refuse
 * the very route the policy asks for. */
static void
test_gpu_only_reaches_the_admitted_route(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.execution_policy = R3V_EXECUTION_GPU_ONLY;
   VkResult armed_result = VK_SUCCESS;
   const bool claimed = route(&s, &armed_result);
   CHECK(armed_result == VK_SUCCESS,
         "GPU_ONLY on the admitted path returns %d", armed_result);
   CHECK(claimed, "GPU_ONLY does not route the admitted cell");
   scene_release(&s);
}

/* Host exclusion, as a counter rather than an inspection.  The routed
 * record leaves the destination as it found it and moves nothing; the same
 * record unrouted fills the destination and moves the counter by one. */
static void
test_host_exclusion_counter(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   if (!route(&s, NULL)) {
      CHECK(false, "the routed leg does not route");
      scene_release(&s);
      return;
   }
   CHECK(r3v_native_cmd_buffer_execute_deferred_copies(
            &s.device, &s.cmd, s.copy->group) == VK_SUCCESS,
         "the routed leg's transfer execution failed");
   CHECK(s.device.host_semantic_writes == 0,
         "the routed leg moved the host-write counter to %llu",
         (unsigned long long)s.device.host_semantic_writes);
   unsigned changed = 0;
   for (uint32_t i = 0; i < CELL_ALLOCATION_BYTES; i++)
      changed += s.storage[i] != CELL_SENTINEL;
   CHECK(changed == 0, "the routed leg wrote %u bytes on the host", changed);
   scene_release(&s);

   /* The known-bad leg: the same record with the route gate closed. */
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = NULL;
   CHECK(!route(&s, NULL), "the known-bad leg still routes");
   CHECK(r3v_native_cmd_buffer_execute_deferred_copies(
            &s.device, &s.cmd, s.copy->group) == VK_SUCCESS,
         "the known-bad leg's transfer execution failed");
   CHECK(s.device.host_semantic_writes == 1,
         "the known-bad leg moved the host-write counter to %llu",
         (unsigned long long)s.device.host_semantic_writes);
   unsigned filled = 0;
   for (uint32_t i = CELL_FILL_OFFSET;
        i < CELL_FILL_OFFSET + CELL_FILL_BYTES; i += 4) {
      uint32_t word;
      memcpy(&word, s.storage + i, sizeof(word));
      filled += word == CELL_FILL_VALUE;
   }
   CHECK(filled == CELL_FILL_BYTES / 4,
         "the known-bad leg filled %u of %u dwords", filled,
         CELL_FILL_BYTES / 4);
   CHECK(s.storage[CELL_FILL_OFFSET - 1] == CELL_SENTINEL &&
            s.storage[CELL_FILL_OFFSET + CELL_FILL_BYTES] == CELL_SENTINEL,
         "the known-bad leg wrote outside the recorded interval");
   scene_release(&s);
}

/* A reset command buffer carries no routed record.  The record describes
 * copies the reset drops, so a record surviving it would report the next
 * recording as one a GPU route performs, and the submission boundary's
 * policy accounting reads exactly that flag. */
static void
test_reset_drops_the_routed_record(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   if (!route(&s, NULL)) {
      CHECK(false, "the routed record does not route");
      scene_release(&s);
      return;
   }
   r3v_native_cmd_buffer_release_recording(&s.cmd);
   CHECK(!s.cmd.fill_route_active,
         "the reset command buffer still carries a routed record");
   CHECK(s.cmd.fill_route_provenance.route_id == R300_OPERATION_ROUTE_NONE &&
            s.cmd.fill_route_provenance.operation_id ==
               R300_OPERATION_ID_NONE,
         "the reset command buffer still carries a provenance");
   scene_release(&s);
}

/* The routed record follows its transport.  The walk stops where the
 * transport stopped, device_submission moves with the phase, and every
 * record it produces holds to the policy that admitted it. */
static void
test_record_follows_its_transport(const struct reference *ref)
{
   static const struct {
      const char *name;
      bool ioctl_accepted;
      bool completion_retired;
      enum r3v_execution_phase phase;
   } legs[] = {
      { "a rejected ioctl", false, false,
        R3V_EXECUTION_PHASE_IOCTL_ENTERED },
      { "an accepted ioctl with no completion", true, false,
        R3V_EXECUTION_PHASE_IOCTL_ACCEPTED },
      { "a retired completion", true, true,
        R3V_EXECUTION_PHASE_COMPLETION_RETIRED },
   };

   for (unsigned i = 0; i < sizeof(legs) / sizeof(legs[0]); i++) {
      struct scene s;
      if (!scene_init(&s, ref)) {
         CHECK(false, "the scene does not build");
         return;
      }
      if (!route(&s, NULL)) {
         CHECK(false, "%s: the record does not route", legs[i].name);
         scene_release(&s);
         return;
      }
      CHECK(s.cmd.fill_route_provenance.phase ==
               R3V_EXECUTION_PHASE_PREPARED,
            "%s: the route left the record past preparation", legs[i].name);
      r3v_native_fill_route_record_transport(&s.cmd, legs[i].ioctl_accepted,
                                             legs[i].completion_retired);
      CHECK(s.cmd.fill_route_active,
            "%s: the walk dropped the record", legs[i].name);
      CHECK(s.cmd.fill_route_provenance.phase == legs[i].phase,
            "%s: the record reports phase %s", legs[i].name,
            r3v_execution_phase_name(s.cmd.fill_route_provenance.phase));
      CHECK(s.cmd.fill_route_provenance.device_submission,
            "%s: the record reports no submission past the ioctl entry",
            legs[i].name);
      const char *reason = NULL;
      CHECK(r3v_execution_provenance_valid(&s.cmd.fill_route_provenance,
                                           R3V_EXECUTION_GPU_ONLY, &reason),
            "%s: the record fails its own policy: %s", legs[i].name,
            reason != NULL ? reason : "unnamed");
      scene_release(&s);
   }

   /* A command buffer no route claimed carries no record to walk. */
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   s.device.compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] = NULL;
   CHECK(!route(&s, NULL), "the unclaimed leg still routes");
   r3v_native_fill_route_record_transport(&s.cmd, true, true);
   CHECK(s.cmd.fill_route_provenance.phase == R3V_EXECUTION_PHASE_PREPARED &&
            !s.cmd.fill_route_provenance.device_submission,
         "an unclaimed command buffer's record was walked");
   scene_release(&s);
}

/* A command buffer is submittable more than once, and the install is
 * scoped to one submission: the authorization, the declared identity, and
 * the one-shot evidence directory each describe one.  A second route call
 * over the same command buffer returns it to its recorded shape and runs
 * the whole admission again, so the second submission is authorized on its
 * own terms rather than carrying the first's.
 */
static void
test_resubmission_readmits(const struct reference *ref)
{
   struct scene s;
   if (!scene_init(&s, ref)) {
      CHECK(false, "the scene does not build");
      return;
   }
   if (!route(&s, NULL)) {
      CHECK(false, "the first submission does not route");
      scene_release(&s);
      return;
   }
   char first_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(s.cmd.ib, s.cmd.ib_size_dwords, first_digest);

   /* The same world admits the second submission on its own terms, and the
    * stream it builds is the one the first built. */
   CHECK(route(&s, NULL), "the second submission does not route");
   char second_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(s.cmd.ib, s.cmd.ib_size_dwords,
                               second_digest);
   CHECK(strcmp(first_digest, second_digest) == 0,
         "the second submission built a different stream");

   /* A spent evidence directory refuses the next submission, and the
    * command buffer is left in its recorded shape for the host store loop.
    * A stale install surviving into this call would leave a stream behind
    * and fail here. */
   s.fixture.attempt_token_present = true;
   CHECK(!route(&s, NULL),
         "a spent attempt token still routes the next submission");
   check_untouched(&s, "a spent attempt token on resubmission");
   scene_release(&s);
}

static void
test_expected_pitch_declaration_parses_fail_closed(void)
{
   /* The declaration is read on the gate pass, so a value the driver
    * cannot read must survive as a recorded refusal rather than as an
    * absent assertion. */
   static const struct {
      const char *value;
      uint32_t pitch;
      bool malformed;
   } arms[] = {
      { NULL, 0u, false },      { "4096", 4096u, false },
      { "16320", 16320u, false }, { "16384", 0u, true },
      { "", 0u, true },         { "257", 0u, true },
      { "0", 0u, true },
      { "abc", 0u, true },      { "64x", 0u, true },
      { "9999999999", 0u, true },
   };
   for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
      struct r3v_native_device device;
      memset(&device, 0, sizeof(device));
      if (arms[i].value != NULL)
         setenv("R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES", arms[i].value, 1);
      else
         unsetenv("R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES");
      r3v_native_device_refresh_delivery_gates(&device);
      CHECK(device.rb2d_v2_expected_pitch_bytes == arms[i].pitch,
            "value %s parses to pitch %u, expected %u",
            arms[i].value != NULL ? arms[i].value : "(unset)",
            device.rb2d_v2_expected_pitch_bytes, arms[i].pitch);
      CHECK(device.rb2d_v2_expected_pitch_malformed == arms[i].malformed,
            "value %s malformed=%d, expected %d",
            arms[i].value != NULL ? arms[i].value : "(unset)",
            device.rb2d_v2_expected_pitch_malformed, arms[i].malformed);
   }
   unsetenv("R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES");
}

int
main(void)
{
   struct reference ref;
   if (!build_reference(&ref)) {
      fprintf(stderr, "FAIL: the reference submission does not build\n");
      return 1;
   }

   test_attended_cell_routes(&ref);
   test_windowed_route_routes(&ref);
   test_both_rb2d_gates_open_refuse(&ref);
   test_declines_leave_the_command_buffer_untouched(&ref);
   test_gpu_only_refuses_rather_than_falling_back(&ref);
   test_gpu_only_reaches_the_admitted_route(&ref);
   test_host_exclusion_counter(&ref);
   test_reset_drops_the_routed_record(&ref);
   test_record_follows_its_transport(&ref);
   test_resubmission_readmits(&ref);
   test_expected_pitch_declaration_parses_fail_closed();

   if (failures != 0) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("r3v-native-fill-route: all checks passed\n");
   return 0;
}
