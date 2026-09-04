/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the public RB2D fill cell: builds the
 * exact one-segment stream the route submits for the attended fill,
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

#include "amd/r300/common/r300_chip_identity.h"
#include "amd/r300/common/r300_operation_route.h"
#include "amd/r300/common/r300_rb2d_fill.h"
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

/* The attended cell the route's document declares. */
#define CELL_ALLOCATION_BYTES (64u * 1024u)
#define CELL_FILL_OFFSET 12u
#define CELL_FILL_BYTES 4992u
#define CELL_FILL_VALUE 0x11223344u

#define RUNNER_DEFAULT_SYSFS_ROOT "/sys"
#define RUNNER_DEFAULT_PCI_SLOT "0000:01:05.0"

/* The retained names one submission leaves behind; a directory carrying
 * any of them is spent for a fresh attempt even when no token exists. */
static const char *const retained_names[] = {
   "ib.bin", "relocs.bin", "manifest.json", "submit_relocs.bin",
   "submit_manifest.json", "attempt.token",
};

struct cell {
   struct r300_rb2d_fill_plan plan;
   struct r300_rb2d_fill_rect rects[R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   uint32_t *ib;
   uint32_t ib_dwords;
   struct r300_rb2d_fill_reloc_site sites[R300_RB2D_FILL_SLOT_COUNT];
   uint32_t site_count;
   char ib_digest[BLAKE3_OUT_LEN * 2 + 1];
};

/* The same construction the route performs: one span on the 256-byte
 * carrier, planned, costed, emitted, and validated. */
static bool
build_cell(struct cell *c)
{
   memset(c, 0, sizeof(*c));
   const struct r300_rb2d_span span = {
      .byte_offset = CELL_FILL_OFFSET,
      .byte_size = CELL_FILL_BYTES,
      .value = CELL_FILL_VALUE,
   };
   const struct r300_rb2d_span_layout layout = {
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };
   enum r300_rb2d_span_refusal refusal = R300_RB2D_SPAN_OK;
   if (r300_rb2d_linear_span_plan(&span, &layout, CELL_ALLOCATION_BYTES,
                                  &c->plan, c->rects, 1, &refusal) != 1) {
      fprintf(stderr, "span plan refused: %s\n",
              r300_rb2d_span_refusal_name(refusal));
      return false;
   }
   if (!r300_rb2d_linear_span_dwords(&c->plan, 1, &c->ib_dwords) ||
       c->ib_dwords == 0) {
      fprintf(stderr, "span cost failed\n");
      return false;
   }
   c->ib = calloc(c->ib_dwords, sizeof(*c->ib));
   if (c->ib == NULL)
      return false;
   struct r300_rb2d_fill_ib emitted;
   if (r300_rb2d_fill_emit_into(&c->plan, c->ib, c->ib_dwords, &emitted) != 0 ||
       r300_rb2d_fill_validate_reloc_sites(&emitted) != 0) {
      fprintf(stderr, "emission or relocation-site validation failed\n");
      return false;
   }
   if (emitted.ib_size_dwords != c->ib_dwords) {
      fprintf(stderr, "emitted %u dwords into a %u-dword cost\n",
              emitted.ib_size_dwords, c->ib_dwords);
      return false;
   }
   c->site_count = emitted.reloc_site_count;
   memcpy(c->sites, emitted.reloc_sites, sizeof(c->sites));
   r300_triangle_ib_digest_hex(c->ib, c->ib_dwords, c->ib_digest);
   return true;
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
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--emit-ib") == 0 && i + 1 < argc) {
         emit_path = argv[++i];
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
              "usage: %s [--emit-ib <path>] <evidence-directory>\n",
              argv[0]);
      return 2;
   }

   struct cell c;
   if (!build_cell(&c)) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   if (emit_path != NULL && write_ib(&c, emit_path) != 0)
      return 2;

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
                                  R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC,
                                  c.ib_digest, evidence_dir, kernel,
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
   struct r3v_fill_route_reloc_site sites[R300_RB2D_FILL_SLOT_COUNT];
   for (uint32_t r = 0; r < c.site_count; r++) {
      sites[r].ib_index = c.sites[r].ib_index;
      sites[r].slot = c.sites[r].slot;
   }
   char identity[R3V_FILL_ROUTE_DIGEST_HEX_SIZE] = "";
   const char *identity_reason = NULL;
   enum r3v_fill_route_refusal identity_verdict =
      R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED;
   const char *declared_identity =
      getenv("R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3");
   if (handle_declared) {
      const struct r3v_fill_route_identity id = {
         .allocation_bytes = CELL_ALLOCATION_BYTES,
         .buffer_bytes = CELL_ALLOCATION_BYTES,
         .binding_offset = 0,
         .fill_offset = CELL_FILL_OFFSET,
         .fill_bytes = CELL_FILL_BYTES,
         .fill_value = CELL_FILL_VALUE,
         .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
         .format = (uint32_t)R300_RB2D_FORMAT_ARGB8888,
         .segment_count = 1,
         .rect_count = c.plan.rect_count,
         .rects = c.rects,
         .ib_dwords = c.ib_dwords,
         .ib = c.ib,
         .relocation_count = c.site_count,
         .reloc_sites = sites,
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
      r300_operation_route(R300_OPERATION_ROUTE_RB2D_CONST_FILL);

   printf("r3v native rb2d-fill arming report\n");
   printf("cell_kind=rb2d_fill_public\n");
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
   printf("allocation_bytes=%u\n", CELL_ALLOCATION_BYTES);
   printf("fill_offset=%u\n", CELL_FILL_OFFSET);
   printf("fill_bytes=%u\n", CELL_FILL_BYTES);
   printf("fill_value=0x%08x\n", CELL_FILL_VALUE);
   printf("pitch_bytes=%u\n", R300_RB2D_SPAN_PITCH_DIRECT_WRITE);
   printf("format=argb8888\n");
   printf("segment_count=1\n");
   printf("rect_count=%u\n", c.plan.rect_count);
   for (uint32_t r = 0; r < c.plan.rect_count; r++) {
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
