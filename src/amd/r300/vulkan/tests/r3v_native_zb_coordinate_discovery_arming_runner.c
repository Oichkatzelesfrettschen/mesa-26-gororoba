/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for coordinate-selectable tiled depth
 * discovery: builds the exact single-pixel cell an attended run would
 * submit, reports its digest and every arming factor, and stops at the
 * authorization boundary.  The runner performs no ioctl and creates no
 * Vulkan device.
 *
 * The scenario digest binds coordinate, layout, pitch, base, allocation,
 * and packed values.  The IB and initial-image digests independently bind
 * the stream and host fill, because neither alone identifies all scenario
 * fields.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_discovery_cell.h"
#include "amd/r300/common/r300_reg.h"

#include "util/mesa-blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct layout_option {
   const char *name;
   enum r300_zb_coordinate_discovery_layout layout;
};

static const struct layout_option layout_options[] = {
   { "microtiled", R300_ZB_COORDINATE_DISCOVERY_MICROTILED },
   { "macrotiled", R300_ZB_COORDINATE_DISCOVERY_MACROTILED },
};

struct arm_option {
   const char *name;
   uint32_t depth_function;
   bool depth_write;
};

static const struct arm_option arm_options[] = {
   { "measure", R300_ZS_ALWAYS, true },
   { "writes_disabled", R300_ZS_ALWAYS, false },
   { "never", R300_ZS_NEVER, true },
};

static bool
parse_u32(const char *text, uint32_t *out)
{
   char *end = NULL;
   if (text == NULL || text[0] == '\0' ||
       (text[0] == '0' && text[1] != '\0'))
      return false;
   for (const char *cursor = text; *cursor != '\0'; cursor++)
      if (*cursor < '0' || *cursor > '9')
         return false;
   unsigned long value = strtoul(text, &end, 10);
   if (end == text || *end != '\0' || value > UINT32_MAX)
      return false;
   *out = (uint32_t)value;
   return true;
}

static int
scenario_digest(const struct r300_zb_depth_discovery_scenario *scenario,
                char out[BLAKE3_OUT_LEN * 2 + 1])
{
   char declaration[512];
   const int length = r300_zb_coordinate_discovery_declaration(
      scenario, declaration, sizeof(declaration));
   if (length < 0)
      return 1;
   struct mesa_blake3 ctx;
   blake3_hash digest;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, declaration, (size_t)length);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
   return 0;
}

/* Builds the cell and returns its IB digest, the content an
 * authorization declares through R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The
 * reference emission is the same construction the recorder installs and
 * the queue recomputes, so the armed digest names the submitted bytes.
 */
static int
cell_digest(const struct r300_zb_depth_discovery_scenario *scenario,
            const struct arm_option *arm, char out[BLAKE3_OUT_LEN * 2 + 1],
            uint32_t *ib_dwords)
{
   struct r300_zb_depth_discovery_ib cell;
   if (r300_zb_depth_discovery_reference_emit(scenario, arm->depth_function,
                                              arm->depth_write, &cell) != 0)
      return 1;
   if (r300_zb_depth_discovery_validate_reloc_sites(&cell) != 0) {
      r300_zb_depth_discovery_release(&cell);
      return 1;
   }
   const struct r300_zb_depth_discovery_params declared = {
      .scenario = scenario,
      .depth_function = arm->depth_function,
      .depth_write = arm->depth_write,
   };
   /* The stream the digest names must also be the state the run
    * requires, so the runner reports no digest for a cell whose own
    * state check refuses it. */
   if (r300_zb_depth_discovery_check_state(&declared, cell.ib,
                                           cell.ib_size_dwords) != 0) {
      r300_zb_depth_discovery_release(&cell);
      return 1;
   }

   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, out);
   *ib_dwords = cell.ib_size_dwords;
   r300_zb_depth_discovery_release(&cell);
   return 0;
}

/* The initial depth image the recorder writes, hashed here so the
 * operator can compare a retained artifact against the experiment that
 * was armed.  It comes from the same constructor the recorder fills the
 * device allocation with, so the armed digest and the artifact's digest
 * are one construction rather than two loops that happen to agree. */
static int
initial_image_digest(const struct r300_zb_depth_discovery_scenario *scenario,
                     char out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_discovery_layout(scenario, &layout) != 0)
      return 1;
   uint8_t *bytes = malloc((size_t)scenario->allocation_bytes);
   if (bytes == NULL)
      return 1;
   if (r300_zb_depth_discovery_fill_initial(scenario, &layout, bytes) != 0) {
      free(bytes);
      return 1;
   }

   struct mesa_blake3 ctx;
   blake3_hash digest;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, bytes, (size_t)scenario->allocation_bytes);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
   free(bytes);
   return 0;
}

static void
report(const char *factor, const char *declared, const char *observed)
{
   const char *state =
      declared == NULL || declared[0] == '\0' ? "UNDECLARED"
      : observed != NULL && strcmp(declared, observed) == 0 ? "match"
                                                            : "MISMATCH";
   printf("  %-22s declared=%-34s observed=%-34s %s\n", factor,
          declared != NULL && declared[0] != '\0' ? declared : "(unset)",
          observed != NULL && observed[0] != '\0' ? observed : "(none)",
          state);
}

int
main(int argc, char **argv)
{
   if (argc != 8) {
      fprintf(stderr,
              "usage: %s <evidence-directory> <layout> <x> <y> "
              "<pitch-pixels> <base-bytes> <arm>\n"
              "  layout: microtiled | macrotiled\n"
              "  x,y: 0..63; pitch-pixels: 64 | 96; "
              "base-bytes: 2048 | 4096\n"
              "  arm: measure | writes_disabled | never\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const struct layout_option *chosen_layout = NULL;
   for (size_t i = 0; i < sizeof(layout_options) / sizeof(*layout_options);
        i++)
      if (strcmp(argv[2], layout_options[i].name) == 0)
         chosen_layout = &layout_options[i];
   const struct arm_option *chosen_arm = NULL;
   for (size_t i = 0; i < sizeof(arm_options) / sizeof(*arm_options); i++)
      if (strcmp(argv[7], arm_options[i].name) == 0)
         chosen_arm = &arm_options[i];
   uint32_t pixel_x, pixel_y, pitch_pixels, base_bytes;
   if (chosen_layout == NULL || chosen_arm == NULL ||
       !parse_u32(argv[3], &pixel_x) || !parse_u32(argv[4], &pixel_y) ||
       !parse_u32(argv[5], &pitch_pixels) ||
       !parse_u32(argv[6], &base_bytes)) {
      fprintf(stderr, "invalid coordinate-discovery declaration\n");
      return 2;
   }

   struct r300_zb_coordinate_discovery configured;
   if (r300_zb_coordinate_discovery_init(
          chosen_layout->layout, pixel_x, pixel_y, pitch_pixels, base_bytes,
          &configured) != 0) {
      fprintf(stderr, "coordinate-discovery declaration refused\n");
      return 2;
   }
   const struct r300_zb_depth_discovery_scenario *scenario =
      &configured.scenario;

   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_discovery_layout(scenario, &layout) != 0) {
      fprintf(stderr, "scenario resolves no layout\n");
      return 2;
   }

   printf("scenario: %s arm=%s pixel=(%u,%u) initial=0x%06x/0x%02x "
          "marker=0x%06x\n",
          scenario->name, chosen_arm->name, scenario->pixel_x,
          scenario->pixel_y, scenario->initial_depth_code,
          scenario->initial_stencil, scenario->marker_depth_code);
   printf("allocation=%llu envelope=[%llu,%llu) unclaimed=%llu\n",
          (unsigned long long)scenario->allocation_bytes,
          (unsigned long long)layout.base_offset_bytes,
          (unsigned long long)(layout.base_offset_bytes +
                               layout.storage_bytes),
          (unsigned long long)(scenario->allocation_bytes -
                               layout.total_bytes));

   char scenario_blake3[BLAKE3_OUT_LEN * 2 + 1];
   if (scenario_digest(scenario, scenario_blake3) != 0) {
      fprintf(stderr, "scenario digest construction failed\n");
      return 2;
   }

   char digest[BLAKE3_OUT_LEN * 2 + 1];
   uint32_t ib_dwords = 0;
   if (cell_digest(scenario, chosen_arm, digest, &ib_dwords) != 0) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   char image_digest[BLAKE3_OUT_LEN * 2 + 1];
   if (initial_image_digest(scenario, image_digest) != 0) {
      fprintf(stderr, "initial image construction failed\n");
      return 2;
   }

   printf("layout=%s\n", chosen_layout->name);
   printf("pixel_x=%u\n", scenario->pixel_x);
   printf("pixel_y=%u\n", scenario->pixel_y);
   printf("pitch_pixels=%u\n", scenario->surface->pitch_pixels);
   printf("base_bytes=%u\n", scenario->guard_bytes);
   printf("arm=%s\n", chosen_arm->name);
   printf("allocation_bytes=%llu\n",
          (unsigned long long)scenario->allocation_bytes);
   printf("storage_bytes=%llu\n",
          (unsigned long long)layout.storage_bytes);
   printf("scenario_blake3=%s\n", scenario_blake3);

   /* The board identity an attended run would resolve is supplied
    * rather than probed, so the runner opens no device node. */
   const char *vendor_env = getenv("R3V_NATIVE_RUNNER_PCI_VENDOR");
   const char *device_env = getenv("R3V_NATIVE_RUNNER_PCI_DEVICE");
   uint32_t vendor_id = vendor_env != NULL
                           ? (uint32_t)strtoul(vendor_env, NULL, 0)
                           : R3V_NATIVE_ARMING_PCI_VENDOR;
   uint32_t device_id = device_env != NULL
                           ? (uint32_t)strtoul(device_env, NULL, 0)
                           : R3V_NATIVE_ARMING_PCI_DEVICE;

   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PLATFORM, vendor_id,
                             device_id,
                             R3V_NATIVE_CELL_KIND_ZB_DEPTH_DISCOVERY, digest,
                             evidence_dir, kernel, sizeof(kernel), module,
                             sizeof(module));

   printf("r3v native zb-coordinate-discovery arming report\n");
   printf("cell_kind=zb-coordinate-discovery\n");
   printf("ib_dwords=%u\n", ib_dwords);
   printf("ib_blake3=%s\n", digest);
   /* The three digests bind declaration, stream, and host fill as distinct
    * evidence surfaces. */
   printf("initial_image_blake3=%s\n", image_digest);
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("bundle digest", facts.authorized_ib_blake3, facts.actual_ib_blake3);
   printf("  %-22s declared=0x%04x:0x%04x%-22s observed=0x%04x:0x%04x%-20s "
          "%s\n",
          "board identity", R3V_NATIVE_ARMING_PCI_VENDOR,
          R3V_NATIVE_ARMING_PCI_DEVICE, "", vendor_id, device_id, "",
          vendor_id == R3V_NATIVE_ARMING_PCI_VENDOR &&
                device_id == R3V_NATIVE_ARMING_PCI_DEVICE
             ? "match"
             : "MISMATCH");
   report("kernel release", facts.authorized_kernel_release,
          facts.running_kernel_release);
   report("module srcversion", facts.authorized_module_srcversion,
          facts.running_module_srcversion);
   printf("  %-22s %s\n", "evidence directory",
          facts.evidence_dir_present ? "present" : "ABSENT");
   printf("  %-22s %s\n", "one-shot token",
          facts.attempt_token_present ? "PRESENT (already attempted)"
                                      : "absent");

   enum r3v_native_arming_verdict verdict =
      r3v_native_arming_evaluate(&facts);
   printf("verdict: %s\n", r3v_native_arming_verdict_name(verdict));
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   return verdict == R3V_NATIVE_ARMING_ARMED ? 0 : 1;
}
