/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the depth address-discovery cell:
 * builds the exact single-pixel cell an attended run would submit,
 * reports its digest and every arming factor, and stops at the
 * authorization boundary.  The runner performs no ioctl and creates no
 * Vulkan device, so running it is safe on the target host.
 *
 * The IB digest authorizes a stream, and a stream is not an experiment
 * here.  The three linear scenarios differ in their stencil seed alone
 * and emit byte-identical IBs, so one authorization admits all three,
 * and the run that separates them is the one whose retained artifact
 * carries the initial-image digest this runner also reports.  The runner
 * prints both so the operator arms a stream and records an experiment.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_discovery_cell.h"
#include "amd/r300/common/r300_reg.h"

#include "util/mesa-blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scenario_option {
   const char *name;
   const struct r300_zb_depth_discovery_scenario *scenario;
};

static const struct scenario_option scenario_options[] = {
   { "z24_linear", &r300_zb_depth_discovery_z24_linear },
   { "z24_linear_seed_5a", &r300_zb_depth_discovery_z24_linear_seed_5a },
   { "z24_linear_seed_a5", &r300_zb_depth_discovery_z24_linear_seed_a5 },
   { "z24_microtiled", &r300_zb_depth_discovery_z24_microtiled },
   { "z24_macrotiled", &r300_zb_depth_discovery_z24_macrotiled },
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

/* The initial depth image the recorder would write, hashed here so the
 * operator can compare a retained artifact against the experiment that
 * was armed.  It is built from the scenario alone, which is what makes
 * it an independent record of the declaration rather than a readback. */
static int
initial_image_digest(const struct r300_zb_depth_discovery_scenario *scenario,
                     char out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_discovery_layout(scenario, &layout) != 0)
      return 1;
   uint32_t initial_word = 0;
   if (r300_zb_depth_discovery_initial_word(scenario, &initial_word) != 0)
      return 1;

   uint8_t *bytes = malloc((size_t)scenario->allocation_bytes);
   if (bytes == NULL)
      return 1;
   memset(bytes, R300_ZB_DISCOVERY_GUARD_FILL,
          (size_t)scenario->allocation_bytes);
   const uint32_t bpp = layout.bytes_per_pixel;
   for (uint64_t off = layout.base_offset_bytes;
        off < layout.base_offset_bytes + layout.storage_bytes; off += bpp)
      for (uint32_t i = 0; i < bpp; i++)
         bytes[off + i] = (uint8_t)(initial_word >> (8u * i));

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
   if (argc != 4) {
      fprintf(stderr,
              "usage: %s <evidence-directory> <scenario> <arm>\n"
              "  scenario: z24_linear | z24_linear_seed_5a | "
              "z24_linear_seed_a5 | z24_microtiled | z24_macrotiled\n"
              "  arm:      measure | writes_disabled | never\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const struct scenario_option *chosen_scenario = NULL;
   for (size_t i = 0; i < sizeof(scenario_options) / sizeof(*scenario_options);
        i++)
      if (strcmp(argv[2], scenario_options[i].name) == 0)
         chosen_scenario = &scenario_options[i];
   const struct arm_option *chosen_arm = NULL;
   for (size_t i = 0; i < sizeof(arm_options) / sizeof(*arm_options); i++)
      if (strcmp(argv[3], arm_options[i].name) == 0)
         chosen_arm = &arm_options[i];
   if (chosen_scenario == NULL || chosen_arm == NULL) {
      fprintf(stderr, "unknown scenario or arm\n");
      return 2;
   }
   const struct r300_zb_depth_discovery_scenario *scenario =
      chosen_scenario->scenario;

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

   printf("r3v native zb-depth-discovery arming report\n");
   printf("cell_kind=zb-depth-discovery\n");
   printf("ib_dwords=%u\n", ib_dwords);
   printf("ib_blake3=%s\n", digest);
   /* One IB digest covers every scenario whose stream is identical, so
    * the initial image is what names the experiment. */
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
