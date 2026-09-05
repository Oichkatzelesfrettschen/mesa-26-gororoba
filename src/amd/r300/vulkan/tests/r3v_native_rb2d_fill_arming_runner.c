/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the named RB2D fill cells: builds the
 * exact stream the route submits for the selected cell,
 * derives every arming factor and the declared submission identity, and
 * stops at the authorization boundary.  The runner creates no Vulkan
 * object, opens no DRM node, allocates no buffer object, issues no ioctl,
 * and writes nothing into the evidence directory, so running it on the
 * attended board changes no state that a later submission reads.
 *
 * The board is resolved the way the physical device resolves it -- the PCI
 * tuple and the DMI product name -- but from sysfs files alone.
 * R3V_NATIVE_RUNNER_SYSFS_ROOT names the tree (default /sys) and
 * R3V_NATIVE_RUNNER_PCI_SLOT the device (default 0000:01:05.0, the RS485M
 * IGP on the Dell Vostro 1000), so a calibration run reads a fixture tree
 * and the attended run reads the machine.
 *
 * The submission identity binds the destination buffer object by the name
 * the kernel gives it for the submitting process.  The runner takes that
 * name as R3V_NATIVE_RUNNER_DESTINATION_HANDLE rather than allocating
 * anything to learn it; an undeclared handle leaves the identity
 * uncomputed and refuses.
 */

#include "r3v_fill_route.h"
#include "r3v_native_arming.h"
#include "r3v_public_rb2d_fill_oracle.h"

#include "amd/r300/common/r300_chip_identity.h"
#include "amd/r300/common/r300_operation_route.h"
#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_legalize.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "git_sha1.h"
#include "util/mesa-blake3.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define RUNNER_DEFAULT_SYSFS_ROOT "/sys"
#define RUNNER_DEFAULT_PCI_SLOT "0000:01:05.0"

/* The retained names one submission leaves behind; a directory carrying
 * any of them is spent for a fresh attempt even when no token exists. */
static const char *const retained_names[] = {
   "ib.bin", "relocs.bin", "manifest.json", "submit_relocs.bin",
   "submit_manifest.json", "attempt.token",
};

/* Windows one cell's stream may carry; the legalizer's own maximum, so
 * the runner sizes storage the way the route does. */
#define RUNNER_MAX_WINDOWS R300_RB2D_LEGALIZE_MAX_WINDOWS

struct cell {
   const struct r3v_public_rb2d_fill_cell *declared;
   struct r300_rb2d_window windows[RUNNER_MAX_WINDOWS];
   struct r300_rb2d_fill_rect rects[RUNNER_MAX_WINDOWS *
                                    R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   uint32_t window_count;
   uint32_t rect_count;
   uint32_t pitch_bytes;
   enum r300_rb2d_format format;
   uint32_t *ib;
   uint32_t ib_dwords;
   struct r3v_fill_route_reloc_site sites[RUNNER_MAX_WINDOWS];
   uint32_t site_count;
   char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
};

/* The same construction the route performs: the legalizer lowers the
 * cell's interval on the cell's carrier under the cell's contract, and
 * the emitter writes the stream the submission would carry.  The evidence
 * floors are the cell's own -- a route-receipt cell asks the carrier for a
 * silicon receipt, a carrier-qualification cell asks PLANNED for the pitch
 * it exists to qualify -- and the contract floor is the kernel replay the
 * legalization differential holds.  The runner then asserts the cell's
 * declared carrier, window, and site counts against what came out, so a
 * declaration that disagrees with the lowering refuses here rather than at
 * the board. */
static bool
build_cell(struct cell *c, const struct r3v_public_rb2d_fill_cell *declared)
{
   memset(c, 0, sizeof(*c));
   c->declared = declared;
   const bool qualification =
      declared->evidence_scope ==
      R3V_PUBLIC_RB2D_FILL_SCOPE_CARRIER_QUALIFICATION;
   const struct r300_rb2d_legalize_request request = {
      .byte_offset = declared->fill_offset,
      .byte_size = declared->fill_bytes,
      .pattern = declared->fill_value,
      .bo_size = declared->allocation_bytes,
      .usage = R300_RB2D_USAGE_FILL_BUFFER,
      .contract = declared->contract,
      .minimum_evidence = qualification
                             ? R300_RB2D_PITCH_EVIDENCE_PLANNED
                             : R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT,
      .minimum_contract_evidence =
         declared->contract == R300_RB2D_CONTRACT_CONST_FILL_V1
            ? R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT
            : R300_RB2D_CONTRACT_EVIDENCE_KERNEL_REPLAY,
      .pinned_pitch_bytes = declared->pinned_pitch_bytes,
   };
   struct r300_rb2d_legalize_result result;
   c->window_count = r300_rb2d_legalize_linear_span(
      &request, c->windows, RUNNER_MAX_WINDOWS, &result);
   if (c->window_count == 0u) {
      fprintf(stderr, "legalization refused: %s (span %s, window %s)\n",
              r300_rb2d_legalize_refusal_name(result.refusal),
              r300_rb2d_span_refusal_name(result.span_refusal),
              r300_rb2d_window_refusal_name(result.window_refusal));
      return false;
   }
   c->pitch_bytes = result.pitch_bytes;
   c->format = result.format;
   c->ib_dwords = result.ib_dwords;
   if (c->pitch_bytes != declared->expected_pitch_bytes ||
       c->window_count != declared->expected_window_count ||
       result.relocation_sites != declared->expected_relocation_sites) {
      fprintf(stderr,
              "cell %s declares pitch %u, %u windows, %u sites; the "
              "legalization produced pitch %u, %u windows, %u sites\n",
              declared->name, declared->expected_pitch_bytes,
              declared->expected_window_count,
              declared->expected_relocation_sites, c->pitch_bytes,
              c->window_count, result.relocation_sites);
      return false;
   }
   for (uint32_t w = 0; w < c->window_count; w++) {
      memcpy(c->rects + c->rect_count, c->windows[w].rects,
             c->windows[w].rect_count * sizeof(c->rects[0]));
      c->rect_count += c->windows[w].rect_count;
   }
   c->ib = calloc(c->ib_dwords, sizeof(*c->ib));
   if (c->ib == NULL)
      return false;
   struct r300_rb2d_legalized_ib emitted;
   if (r300_rb2d_legalize_emit(c->windows, c->window_count, c->ib,
                               c->ib_dwords, &emitted) != 0 ||
       emitted.ib_size_dwords != c->ib_dwords ||
       emitted.site_count != c->window_count) {
      fprintf(stderr, "emission did not produce the stream it was sized to\n");
      return false;
   }
   for (uint32_t r = 0; r < emitted.site_count; r++) {
      c->sites[r].ib_index = emitted.sites[r].ib_index;
      c->sites[r].slot = emitted.sites[r].slot;
   }
   c->site_count = emitted.site_count;
   r300_triangle_ib_digest_hex(c->ib, c->ib_dwords, c->ib_digest);
   return true;
}

/* The kind the route installs for a cell: the sealed V1 fill, the windowed
 * route, or the carrier qualification.  The arming digest binds the kind,
 * so the runner names the same one the route would. */
static enum r3v_native_cell_kind
cell_kind(const struct r3v_public_rb2d_fill_cell *declared)
{
   if (declared->contract == R300_RB2D_CONTRACT_CONST_FILL_V1)
      return R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC;
   if (declared->evidence_scope ==
       R3V_PUBLIC_RB2D_FILL_SCOPE_CARRIER_QUALIFICATION)
      return R3V_NATIVE_CELL_KIND_RB2D_CARRIER_QUALIFICATION;
   return R3V_NATIVE_CELL_KIND_RB2D_FILL_V2_ROUTE;
}

/* The kind's report spelling, held to the plan registry's names by
 * r3v_native_rb2d_fill_arming_runner_check. */
static const char *
cell_kind_name(const struct r3v_public_rb2d_fill_cell *declared)
{
   switch (cell_kind(declared)) {
   case R3V_NATIVE_CELL_KIND_RB2D_FILL_V2_ROUTE:
      return "rb2d_fill_v2_route";
   case R3V_NATIVE_CELL_KIND_RB2D_CARRIER_QUALIFICATION:
      return "rb2d_carrier_qualification";
   default:
      return "rb2d_fill_public";
   }
}

/* The stream-shape mutations the canonical table names, each applied to a
 * legalized window list and each refused by a different check.  The four
 * are the ways a multi-window stream can be wrong while every window it
 * still carries looks well formed on its own, so a single window checker
 * cannot see them: the coverage oracle, the emitter's one-site-per-window
 * rule, the in-order cursor, and the window checker each own one.
 */
enum window_mutation {
   WINDOW_MUTATION_NONE = 0,
   WINDOW_MUTATION_DROPPED_SECOND_WINDOW,
   WINDOW_MUTATION_SECOND_SITE_ABSENT,
   WINDOW_MUTATION_SWAPPED_WINDOW_ORDER,
   WINDOW_MUTATION_SECOND_BASE_OFF_GRID,
};

static const struct {
   const char *id;
   enum window_mutation mutation;
   const char *refused_by;
} window_mutations[] = {
   { "dropped_second_window", WINDOW_MUTATION_DROPPED_SECOND_WINDOW,
     "coverage oracle" },
   { "second_window_site_absent", WINDOW_MUTATION_SECOND_SITE_ABSENT,
     "one relocation site per window" },
   { "swapped_window_order", WINDOW_MUTATION_SWAPPED_WINDOW_ORDER,
     "in-order coverage cursor" },
   { "second_window_base_off_grid", WINDOW_MUTATION_SECOND_BASE_OFF_GRID,
     "r300_rb2d_window_check" },
};

/* The byte set the windows cover, walked in emission order against the
 * interval the cell declares.  A gap, an overlap, a short list, or a
 * reordering all break the cursor, which is what makes this the check the
 * per-window invariants cannot replace. */
static bool
windows_cover_interval(const struct r300_rb2d_window *windows,
                       uint32_t window_count, uint64_t begin, uint64_t size,
                       const char **why)
{
   uint64_t cursor = begin;
   for (uint32_t w = 0; w < window_count; w++) {
      const struct r300_rb2d_window *win = &windows[w];
      for (uint32_t i = 0; i < win->rect_count; i++) {
         const struct r300_rb2d_fill_rect *r = &win->rects[i];
         for (uint32_t row = 0; row < r->height; row++) {
            const uint64_t start = win->bo_base +
                                   (uint64_t)(r->y + row) * win->pitch_bytes +
                                   (uint64_t)r->x * win->cpp;
            if (start != cursor) {
               *why = "a rectangle row does not continue the interval";
               return false;
            }
            cursor += (uint64_t)r->width * win->cpp;
         }
      }
   }
   if (cursor != begin + size) {
      *why = "the covered bytes fall short of the interval";
      return false;
   }
   return true;
}

/* Applies one mutation and returns the check that refuses it, or NULL when
 * the mutated stream was admitted, which is the defect this lane exists to
 * catch. */
static const char *
refuse_mutated_stream(const struct cell *c, enum window_mutation mutation,
                      const char **detail)
{
   struct r300_rb2d_window windows[RUNNER_MAX_WINDOWS];
   uint32_t count = c->window_count;
   memcpy(windows, c->windows, sizeof(windows[0]) * count);
   *detail = "";

   switch (mutation) {
   case WINDOW_MUTATION_DROPPED_SECOND_WINDOW:
      count--;
      break;
   case WINDOW_MUTATION_SWAPPED_WINDOW_ORDER: {
      const struct r300_rb2d_window first = windows[0];
      windows[0] = windows[count - 1];
      windows[count - 1] = first;
      break;
   }
   case WINDOW_MUTATION_SECOND_BASE_OFF_GRID:
      windows[count - 1].bo_base += 4u;
      break;
   case WINDOW_MUTATION_SECOND_SITE_ABSENT:
   case WINDOW_MUTATION_NONE:
      break;
   }

   for (uint32_t w = 0; w < count; w++) {
      const enum r300_rb2d_window_refusal refusal =
         r300_rb2d_window_check(&windows[w], c->declared->allocation_bytes);
      if (refusal != R300_RB2D_WINDOW_OK) {
         *detail = r300_rb2d_window_refusal_name(refusal);
         return "r300_rb2d_window_check";
      }
   }

   uint32_t words[RUNNER_MAX_WINDOWS * 64u];
   struct r300_rb2d_legalized_ib emitted;
   if (r300_rb2d_legalize_emit(windows, count, words,
                               (uint32_t)(sizeof(words) / sizeof(words[0])),
                               &emitted) != 0) {
      *detail = "the emitter refused the window list";
      return "r300_rb2d_legalize_emit";
   }
   /* The site count the emitter produces is one per window by
    * construction, so a stream declaring fewer sites than windows is a
    * stream whose destination is bound fewer times than it is rebased. */
   const uint32_t declared_sites =
      mutation == WINDOW_MUTATION_SECOND_SITE_ABSENT ? count - 1u : count;
   if (emitted.site_count != declared_sites) {
      *detail = "the emitted site count differs from the declared one";
      return "one relocation site per window";
   }

   const char *why = "";
   if (!windows_cover_interval(windows, count, c->declared->fill_offset,
                               c->declared->fill_bytes, &why)) {
      *detail = why;
      return count < c->window_count ? "coverage oracle"
                                     : "in-order coverage cursor";
   }
   return NULL;
}

static void
read_first_line(const char *path, char *out, size_t size)
{
   out[0] = '\0';
   FILE *f = fopen(path, "re");
   if (f == NULL)
      return;
   if (fgets(out, (int)size, f) != NULL) {
      size_t length = strlen(out);
      while (length > 0 &&
             (out[length - 1] == '\n' || out[length - 1] == '\r'))
         out[--length] = '\0';
   }
   fclose(f);
}

static bool
read_hex16(const char *root, const char *slot, const char *leaf,
           uint16_t *out)
{
   char path[512];
   char line[64];
   snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/%s", root, slot,
            leaf);
   read_first_line(path, line, sizeof(line));
   if (line[0] == '\0')
      return false;
   char *end = NULL;
   errno = 0;
   unsigned long value = strtoul(line, &end, 16);
   if (errno != 0 || end == line || *end != '\0' || value > 0xffffu)
      return false;
   *out = (uint16_t)value;
   return true;
}

/* The runner's own provider: the environment, uname, and the sysfs tree
 * the runner was pointed at.  Every fact stays a file read, so a fixture
 * tree drives the calibration legs and the machine drives the attended
 * one. */
struct runner_facts {
   const char *sysfs_root;
};

static const char *
runner_read_env(void *ctx, const char *name)
{
   (void)ctx;
   return getenv(name);
}

static void
runner_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   struct utsname host;
   out[0] = '\0';
   if (uname(&host) == 0)
      snprintf(out, size, "%s", host.release);
}

static void
runner_read_module_srcversion(void *ctx, char *out, size_t size)
{
   const struct runner_facts *r = ctx;
   char path[512];
   snprintf(path, sizeof(path), "%s/module/radeon/srcversion", r->sysfs_root);
   read_first_line(path, out, size);
}

static bool
runner_directory_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool
runner_file_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0;
}

static void
report(const char *factor, const char *declared, const char *observed)
{
   const char *state =
      declared == NULL || declared[0] == '\0' ? "UNDECLARED"
      : observed != NULL && strcmp(declared, observed) == 0 ? "match"
                                                            : "MISMATCH";
   printf("  %-24s declared=%-34s observed=%-34s %s\n", factor,
          declared != NULL && declared[0] != '\0' ? declared : "(unset)",
          observed != NULL && observed[0] != '\0' ? observed : "(none)",
          state);
}

static int
write_ib(const struct cell *c, const char *path)
{
   FILE *f = fopen(path, "wb");
   if (f == NULL) {
      fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
      return 2;
   }
   const size_t bytes = (size_t)c->ib_dwords * sizeof(*c->ib);
   const bool ok = fwrite(c->ib, 1, bytes, f) == bytes;
   if (fclose(f) != 0 || !ok) {
      fprintf(stderr, "short write to %s\n", path);
      return 2;
   }
   return 0;
}

int
main(int argc, char **argv)
{
   const char *evidence_dir = NULL;
   const char *emit_path = NULL;
   const char *cell_name = "v1_public";
   const char *mutation_id = NULL;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--emit-ib") == 0 && i + 1 < argc) {
         emit_path = argv[++i];
      } else if (strcmp(argv[i], "--cell") == 0 && i + 1 < argc) {
         cell_name = argv[++i];
      } else if (strcmp(argv[i], "--mutate-window") == 0 && i + 1 < argc) {
         mutation_id = argv[++i];
      } else if (argv[i][0] == '-') {
         fprintf(stderr, "unknown option %s\n", argv[i]);
         return 2;
      } else if (evidence_dir == NULL) {
         evidence_dir = argv[i];
      } else {
         fprintf(stderr, "one evidence directory\n");
         return 2;
      }
   }
   if (evidence_dir == NULL) {
      fprintf(stderr,
              "usage: %s [--cell <name>] [--mutate-window <id>] "
              "[--emit-ib <path>] <evidence-directory>\n",
              argv[0]);
      return 2;
   }
   const struct r3v_public_rb2d_fill_cell *declared =
      r3v_public_rb2d_fill_cell_by_name(cell_name);
   if (declared == NULL) {
      fprintf(stderr, "no cell is named %s\n", cell_name);
      return 2;
   }

   struct cell c;
   if (!build_cell(&c, declared)) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   if (emit_path != NULL && write_ib(&c, emit_path) != 0)
      return 2;

   /* The stream-shape lane stops here: it mutates the legalized window
    * list, names the check that refuses it, and never reads an arming
    * fact, so it needs no declaration and no board. */
   if (mutation_id != NULL) {
      for (size_t i = 0; i < sizeof(window_mutations) /
                                sizeof(window_mutations[0]);
           i++) {
         if (strcmp(mutation_id, window_mutations[i].id) != 0)
            continue;
         const char *detail = "";
         const char *refused_by =
            refuse_mutated_stream(&c, window_mutations[i].mutation, &detail);
         printf("cell=%s\n", declared->name);
         printf("window_mutation=%s\n", window_mutations[i].id);
         printf("window_mutation_refused_by=%s\n",
                refused_by != NULL ? refused_by : "(admitted)");
         printf("window_mutation_detail=%s\n", detail);
         printf("no submission attempted: this runner stops at the "
                "authorization boundary\n");
         if (refused_by == NULL ||
             strcmp(refused_by, window_mutations[i].refused_by) != 0) {
            fprintf(stderr,
                    "mutation %s is refused by %s; the table names %s\n",
                    window_mutations[i].id,
                    refused_by != NULL ? refused_by : "nothing",
                    window_mutations[i].refused_by);
            return 3;
         }
         return 0;
      }
      fprintf(stderr, "no window mutation is named %s\n", mutation_id);
      return 2;
   }

   const char *sysfs_root = getenv("R3V_NATIVE_RUNNER_SYSFS_ROOT");
   if (sysfs_root == NULL || sysfs_root[0] == '\0')
      sysfs_root = RUNNER_DEFAULT_SYSFS_ROOT;
   const char *pci_slot = getenv("R3V_NATIVE_RUNNER_PCI_SLOT");
   if (pci_slot == NULL || pci_slot[0] == '\0')
      pci_slot = RUNNER_DEFAULT_PCI_SLOT;

   /* The board, from the same four sysfs values and the DMI product name
    * the physical device reads.  An unreadable value stays zero, which
    * matches no row, so the platform resolves to none and refuses. */
   uint16_t vendor_id = 0, device_id = 0, subsystem_vendor = 0,
            subsystem_device = 0;
   const bool pci_read =
      read_hex16(sysfs_root, pci_slot, "vendor", &vendor_id) &&
      read_hex16(sysfs_root, pci_slot, "device", &device_id) &&
      read_hex16(sysfs_root, pci_slot, "subsystem_vendor",
                 &subsystem_vendor) &&
      read_hex16(sysfs_root, pci_slot, "subsystem_device",
                 &subsystem_device);
   char dmi_product[128];
   char dmi_path[512];
   snprintf(dmi_path, sizeof(dmi_path), "%s/class/dmi/id/product_name",
            sysfs_root);
   read_first_line(dmi_path, dmi_product, sizeof(dmi_product));
   const enum r300_platform_id platform_id =
      pci_read ? r300_platform_id_resolve(vendor_id, device_id,
                                          subsystem_vendor, subsystem_device,
                                          dmi_product)
               : R300_PLATFORM_ID_NONE;

   struct runner_facts runner = { .sysfs_root = sysfs_root };
   const struct r3v_native_arming_provider provider = {
      .read_env = runner_read_env,
      .read_kernel_release = runner_read_kernel_release,
      .read_module_srcversion = runner_read_module_srcversion,
      .directory_present = runner_directory_present,
      .file_present = runner_file_present,
      .ctx = &runner,
   };
   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect_from(&provider, &facts, platform_id, vendor_id,
                                  device_id,
                                  cell_kind(declared), c.ib_digest,
                                  evidence_dir, kernel,
                                  sizeof(kernel), module, sizeof(module));
   facts.nonmaximum_extent = false;

   /* Freshness beyond the token: every retained name the submission would
    * write, so a directory holding a previous run's object is spent. */
   bool evidence_fresh = facts.evidence_dir_present;
   const char *stale_name = NULL;
   for (size_t i = 0; i < sizeof(retained_names) / sizeof(*retained_names);
        i++) {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", evidence_dir, retained_names[i]);
      if (runner_file_present(NULL, path)) {
         evidence_fresh = false;
         if (stale_name == NULL)
            stale_name = retained_names[i];
      }
   }

   /* The submission identity over the declared destination name.  The
    * kernel names the object for the submitting process; the runner
    * declares rather than allocates, so an undeclared name computes no
    * identity. */
   const char *handle_env = getenv("R3V_NATIVE_RUNNER_DESTINATION_HANDLE");
   uint32_t destination_handle = 0;
   bool handle_declared = false;
   if (handle_env != NULL && handle_env[0] != '\0') {
      char *end = NULL;
      errno = 0;
      unsigned long value = strtoul(handle_env, &end, 0);
      handle_declared =
         errno == 0 && end != handle_env && *end == '\0' && value != 0 &&
         value <= UINT32_MAX;
      destination_handle = (uint32_t)value;
   }
   char identity[R3V_FILL_ROUTE_DIGEST_HEX_SIZE] = "";
   const char *identity_reason = NULL;
   enum r3v_fill_route_refusal identity_verdict =
      R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED;
   const char *declared_identity =
      getenv("R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3");
   if (handle_declared) {
      const struct r3v_fill_route_identity id = {
         .allocation_bytes = declared->allocation_bytes,
         .buffer_bytes = declared->allocation_bytes,
         .binding_offset = 0,
         .fill_offset = declared->fill_offset,
         .fill_bytes = declared->fill_bytes,
         .fill_value = declared->fill_value,
         .pitch_bytes = c.pitch_bytes,
         .format = (uint32_t)c.format,
         .segment_count = c.window_count,
         .rect_count = c.rect_count,
         .rects = c.rects,
         .ib_dwords = c.ib_dwords,
         .ib = c.ib,
         .relocation_count = c.site_count,
         .reloc_sites = c.sites,
         .read_domains = 0,
         .write_domain = R3V_FILL_ROUTE_DOMAIN_GTT,
         .destination_handle = destination_handle,
         .kernel_release = facts.running_kernel_release,
         .module_srcversion = facts.running_module_srcversion,
      };
      identity_verdict = r3v_fill_route_authority_check(
         &id, declared_identity, identity, &identity_reason);
   }

   const struct r300_operation_route_row *route =
      r300_operation_route(declared->route_id);

   printf("r3v native rb2d-fill arming report\n");
   printf("cell=%s\n", declared->name);
   printf("cell_kind=%s\n", cell_kind_name(declared));
   printf("evidence_scope=%s\n",
          declared->evidence_scope ==
                R3V_PUBLIC_RB2D_FILL_SCOPE_CARRIER_QUALIFICATION
             ? "carrier_qualification"
             : "route_receipt");
   printf("mesa_source=%s\n", MESA_GIT_SHA1);
   printf("route=%s\n", route != NULL ? route->name : "(none)");
   printf("route_state=%s\n",
          route != NULL ? r300_operation_route_state_name(route->state)
                        : "(none)");
   printf("route_unit=%s\n",
          route != NULL ? r300_execution_unit_name(route->unit) : "(none)");
   printf("route_executor=%s\n",
          route != NULL ? r300_operation_route_executor_name(route->executor)
                        : "(none)");
   printf("route_gate=%s\n",
          route != NULL && route->gate != NULL ? route->gate : "(none)");
   printf("allocation_bytes=%u\n", declared->allocation_bytes);
   printf("fill_offset=%u\n", declared->fill_offset);
   printf("fill_bytes=%u\n", declared->fill_bytes);
   printf("fill_value=0x%08x\n", declared->fill_value);
   printf("pitch_bytes=%u\n", c.pitch_bytes);
   printf("format=argb8888\n");
   printf("window_count=%u\n", c.window_count);
   printf("segment_count=%u\n", c.window_count);
   printf("rect_count=%u\n", c.rect_count);
   for (uint32_t r = 0; r < c.rect_count; r++) {
      printf("rect=%u,%u,%u,%u\n", c.rects[r].x, c.rects[r].y,
             c.rects[r].width, c.rects[r].height);
   }
   printf("ib_dwords=%u\n", c.ib_dwords);
   printf("ib_blake3=%s\n", c.ib_digest);
   printf("relocation_site_count=%u\n", c.site_count);
   for (uint32_t r = 0; r < c.site_count; r++)
      printf("relocation_site=%u,%u\n", c.sites[r].ib_index, c.sites[r].slot);
   printf("bo_role_schema=destination:write=gtt,read=none;"
          "completion:write=gtt,read=none\n");
   printf("destination_handle=%s\n", handle_declared ? handle_env : "(unset)");
   printf("fill_identity_blake3=%s\n",
          identity[0] != '\0' ? identity : "(uncomputed)");
   printf("sysfs_root=%s\n", sysfs_root);
   printf("pci_slot=%s\n", pci_slot);
   printf("pci_tuple=%04x:%04x subsystem=%04x:%04x\n", vendor_id, device_id,
          subsystem_vendor, subsystem_device);
   printf("dmi_product_name=%s\n", dmi_product[0] != '\0' ? dmi_product
                                                           : "(none)");
   printf("platform=%s\n",
          platform_id == R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M
             ? "DELL_VOSTRO1000_RS485M"
             : "NONE");
   printf("kernel_release=%s\n", kernel[0] != '\0' ? kernel : "(none)");
   printf("module_srcversion=%s\n", module[0] != '\0' ? module : "(none)");

   printf("  %-24s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("bundle digest", facts.authorized_ib_blake3,
          facts.actual_ib_blake3);
   printf("  %-24s %s\n", "board identity",
          platform_id == R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M
             ? "Dell Vostro 1000 RS485M"
          : pci_read ? "resolves to no qualified platform"
                     : "PCI tuple unreadable");
   report("kernel release", facts.authorized_kernel_release,
          facts.running_kernel_release);
   report("module srcversion", facts.authorized_module_srcversion,
          facts.running_module_srcversion);
   report("fill identity", declared_identity, identity);
   if (!handle_declared)
      printf("  %-24s UNDECLARED (R3V_NATIVE_RUNNER_DESTINATION_HANDLE)\n",
             "destination handle");
   printf("  %-24s %s\n", "evidence directory",
          !facts.evidence_dir_present ? "ABSENT"
          : evidence_fresh            ? "present, fresh"
                                      : "present, SPENT");
   if (stale_name != NULL)
      printf("  %-24s %s\n", "retained artifact", stale_name);
   printf("  %-24s %s\n", "one-shot token",
          facts.attempt_token_present ? "PRESENT (already attempted)"
                                      : "absent");

   /* The arming verdict over the stream, then the route's own identity
    * check: the route asks both, in that order, and refuses on either. */
   const enum r3v_native_arming_verdict arming =
      r3v_native_arming_evaluate(&facts);
   const char *verdict = r3v_native_arming_verdict_name(arming);
   bool armed = arming == R3V_NATIVE_ARMING_ARMED;
   if (armed && !evidence_fresh) {
      verdict = "evidence directory spent";
      armed = false;
   }
   if (armed && identity_verdict != R3V_FILL_ROUTE_ADMITTED) {
      verdict = identity_reason != NULL
                   ? identity_reason
                   : r3v_fill_route_refusal_name(identity_verdict);
      armed = false;
   }
   printf("verdict: %s\n", armed ? "ARMED" : verdict);
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   free(c.ib);
   return armed ? 0 : 1;
}
