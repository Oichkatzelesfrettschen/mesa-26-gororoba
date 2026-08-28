/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner: builds the exact cell an attended run
 * would submit, reports its digest and every arming factor, and stops at
 * the authorization boundary.  The runner performs no ioctl and creates
 * no Vulkan device, so running it is safe on the target host.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_compute_identity_carrier.h"
#include "amd/r300/common/r300_fragment_binary.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native_render_shape_args.h"
#include "r3v_native_msaa_arms.h"
#include "r3v_native_multi_pass_arms.h"
#include "r3v_native_sampled_arms.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --extent selects bounded cell dimensions for extent-specific emission and
 * the resulting IB digest.
 */
static uint32_t cell_width = R300_TRIANGLE_TARGET_WIDTH;
static uint32_t cell_height = R300_TRIANGLE_TARGET_HEIGHT;
/* --varying selects the varying triangle cell: position-plus-varying
 * records through the pass-through fragment binary, the cell the
 * computed-varying pipeline records, with its own digest.
 */
static bool cell_varying = false;
/* --compute-identity selects the compute identity carrier cell: the
 * fetched producer pass alone over the reference sixteen records, the
 * stream the identity verb's GPU route installs for one dispatch, with
 * its own digest and cell kind.
 */
static bool cell_compute_identity = false;
/* --shape selects the render-shape triangle cell: the qualified cell
 * over a declared extent, pitch, lane order, and fragment constant,
 * with its own digest and cell kind.
 */
static bool cell_render_shape = false;
static struct r300_triangle_render_shape render_shape;
/* --sampled selects the sampled triangle cell: the varying vertex path
 * with the sampled fragment binary and the TX unit-0 block over one
 * arm's texture geometry.  --sampled and --sampled-bgra name the two
 * uniform 16x16 arms, and --sampled-arm names any arm in the table.
 */
static bool cell_sampled = false;
static const struct r3v_sampled_arm *cell_sampled_arm = NULL;
/* --composed selects the composed render-then-sample cell: the
 * reference shape rendered at the allocation base, then sampled into a
 * second target at the declared byte offset.
 */
static bool cell_composed = false;
static uint32_t cell_composed_sample_offset;
/* --msaa selects the multisample resolve cell at a declared sample
 * count: the reference shape rendered into the sample-expanded surface,
 * then the extent covered again under AARESOLVE_MODE_RESOLVE.
 */
static bool cell_msaa = false;
static bool cell_msaa_clear = false;
static uint32_t cell_msaa_sample_count;
/* --multi-pass selects the two-pass cell: two reference render-shape
 * cells, the second with its own vertex page, color target, and
 * fragment constant, emitted in the bound form the recorder installs.
 */
static bool cell_multi_pass = false;

static int
cell_emit(struct r300_tcl_bypass_triangle_ib *cell)
{
   if (cell_composed) {
      struct r300_triangle_composed_render_sample composed;
      r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
      r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
      composed.sample.target_offset = cell_composed_sample_offset;
      const int emitted =
         r300_tcl_bypass_triangle_composed_render_sample_emit(&composed, cell);
      if (emitted != 0)
         return emitted;
      /* The recorded cell binds its payloads to the merged relocation
       * indices, so the digest that authorizes a submission is the bound
       * cell's; an unbound report would name a stream the recorder never
       * installs and the armed run would refuse on the mismatch.
       */
      const int bound = r300_tcl_bypass_triangle_bind_reloc_indices(
         cell, r300_tcl_bypass_triangle_composed_slot_index, R300_TRIANGLE_SLOT_COUNT);
      if (bound != 0)
         r300_tcl_bypass_triangle_release(cell);
      return bound;
   }
   if (cell_multi_pass) {
      struct r300_triangle_multi_pass mp;
      r3v_native_multi_pass_reference(&mp);
      return r300_tcl_bypass_triangle_multi_pass_emit(&mp, cell);
   }
   if (cell_msaa) {
      struct r300_triangle_msaa_resolve msaa;
      r3v_native_msaa_reference_cleared(&msaa, cell_msaa_sample_count,
                                        cell_msaa_clear);
      const int emitted =
         r300_tcl_bypass_triangle_msaa_resolve_emit(&msaa, cell);
      if (emitted != 0)
         return emitted;
      /* The recorder binds its payloads to the merged relocation
       * indices, so the digest that authorizes a submission is the
       * bound cell's.
       */
      const int bound = r300_tcl_bypass_triangle_bind_reloc_indices(
         cell, r300_tcl_bypass_triangle_msaa_slot_index,
         R300_TRIANGLE_SLOT_COUNT);
      if (bound != 0)
         r300_tcl_bypass_triangle_release(cell);
      return bound;
   }
   if (cell_sampled)
      return r300_tcl_bypass_triangle_sampled_emit(
         R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, 1,
         r3v_sampled_arm_texture_offset(cell_sampled_arm),
         cell_sampled_arm->width, cell_sampled_arm->height,
         r3v_sampled_arm_row_pitch_texels(cell_sampled_arm),
         cell_sampled_arm->lanes, cell);
   if (cell_render_shape)
      return r300_tcl_bypass_triangle_render_shape_emit(&render_shape, cell);
   return cell_varying
             ? r300_tcl_bypass_triangle_varying_extent_emit(cell_width,
                                                            cell_height, cell)
             : r300_tcl_bypass_triangle_extent_emit(cell_width, cell_height,
                                                    cell);
}

/* The selected cell's words, heap-allocated: the triangle family
 * through cell_emit, or the compute identity carrier's reference pass.
 */
static int
cell_words(uint32_t **words, uint32_t *count)
{
   if (cell_compute_identity) {
      struct r300_r2vb_fetched_producer_ib pass;
      if (r300_compute_identity_carrier_reference_emit(&pass) != 0)
         return 1;
      *words = malloc((size_t)pass.ib_size_dwords * 4);
      if (*words == NULL) {
         r300_r2vb_fetched_producer_release(&pass);
         return 1;
      }
      memcpy(*words, pass.ib, (size_t)pass.ib_size_dwords * 4);
      *count = pass.ib_size_dwords;
      r300_r2vb_fetched_producer_release(&pass);
      return 0;
   }
   struct r300_tcl_bypass_triangle_ib cell;
   if (cell_emit(&cell) != 0)
      return 1;
   *words = malloc((size_t)cell.ib_size_dwords * 4);
   if (*words == NULL) {
      r300_tcl_bypass_triangle_release(&cell);
      return 1;
   }
   memcpy(*words, cell.ib, (size_t)cell.ib_size_dwords * 4);
   *count = cell.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&cell);
   return 0;
}

/* Builds the cell at the selected extent and returns its IB digest,
 * the content an authorization declares through
 * R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The recorder's
 * r3v_native_record_tcl_bypass_triangle_carrier() installs the same
 * emitted IB through r3v_native_cmd_buffer_install_ib(), and the
 * r3v_native_queue_submit() path recomputes r300_triangle_ib_digest_hex()
 * from the installed IB.  The emission is identical, so the armed digest
 * names the bytes submitted by that path.  Symbol discovery uses
 * `rg --fixed-strings SYMBOL src/amd/r300/` for
 * `r3v_native_record_tcl_bypass_triangle_carrier()`,
 * `r3v_native_cmd_buffer_install_ib()`, `r3v_native_queue_submit()`, and
 * `r300_triangle_ib_digest_hex()`.  The repository-relative search root
 * covers the definitions and call sites that establish this chain.
 */
static int
cell_digest(char out[BLAKE3_OUT_LEN * 2 + 1], uint32_t *ib_dwords)
{
   uint32_t *words = NULL;
   uint32_t count = 0;
   if (cell_words(&words, &count) != 0)
      return 1;
   r300_triangle_ib_digest_hex(words, count, out);
   *ib_dwords = count;
   free(words);
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

/* Writes the selected cell's serialized bytes -- reference cell, varying
 * cell, compute identity carrier, or render shape, per cell_words -- the
 * independent comparison source for a recorded-IB manifest: the emission
 * is the direct reference construction, so equality with a retained
 * ib.bin proves the recording route reproduced the qualified cell.
 */
static int
emit_selected_ib(const char *path)
{
   uint32_t *words = NULL;
   uint32_t count = 0;
   if (cell_words(&words, &count) != 0) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   int status = 0;
   uint8_t *bytes = malloc((size_t)count * 4);
   FILE *out = bytes != NULL ? fopen(path, "wb") : NULL;
   if (out == NULL) {
      fprintf(stderr, "emit-ib: cannot write %s\n", path);
      status = 2;
   } else {
      r300_triangle_ib_serialize(words, count, bytes);
      const size_t written = fwrite(bytes, 1, (size_t)count * 4, out);
      const int close_error = fclose(out);
      if (written != (size_t)count * 4 || close_error != 0) {
         fprintf(stderr, "emit-ib: short write to %s\n", path);
         status = 2;
      }
   }
   free(bytes);
   free(words);
   return status;
}

int
main(int argc, char **argv)
{
   int argi = 1;
   if (argc >= argi + 1 && strcmp(argv[argi], "--varying") == 0) {
      cell_varying = true;
      argi += 1;
   } else if (argc >= argi + 1 &&
              strcmp(argv[argi], "--compute-identity") == 0) {
      cell_compute_identity = true;
      argi += 1;
   } else if (argc >= argi + 1 && strcmp(argv[argi], "--sampled") == 0) {
      cell_sampled = true;
      cell_sampled_arm = r3v_sampled_arm_find("rgba");
      argi += 1;
   } else if (argc >= argi + 1 &&
              strcmp(argv[argi], "--sampled-bgra") == 0) {
      cell_sampled = true;
      cell_sampled_arm = r3v_sampled_arm_find("bgra");
      argi += 1;
   } else if (argc >= argi + 2 &&
              strcmp(argv[argi], "--composed") == 0) {
      cell_composed = true;
      cell_composed_sample_offset =
         (uint32_t)strtoul(argv[argi + 1], NULL, 0);
      argi += 2;
   } else if (argc >= argi + 1 && strcmp(argv[argi], "--multi-pass") == 0) {
      cell_multi_pass = true;
      argi += 1;
   } else if (argc >= argi + 2 && (strcmp(argv[argi], "--msaa") == 0 ||
                                   strcmp(argv[argi], "--msaa-clear") == 0)) {
      cell_msaa = true;
      cell_msaa_clear = strcmp(argv[argi], "--msaa-clear") == 0;
      cell_msaa_sample_count = (uint32_t)strtoul(argv[argi + 1], NULL, 0);
      argi += 2;
   } else if (argc >= argi + 2 &&
              strcmp(argv[argi], "--sampled-arm") == 0) {
      cell_sampled = true;
      cell_sampled_arm = r3v_sampled_arm_find(argv[argi + 1]);
      if (cell_sampled_arm == NULL) {
         fprintf(stderr, "unknown sampled arm %s\n", argv[argi + 1]);
         return 2;
      }
      argi += 2;
   } else if (argc >= argi + 1 + R3V_RENDER_SHAPE_ARGC &&
              strcmp(argv[argi], "--shape") == 0) {
      if (!r3v_render_shape_parse(&argv[argi + 1], &render_shape))
         return 2;
      cell_render_shape = true;
      argi += 1 + R3V_RENDER_SHAPE_ARGC;
      if (argc >= argi + 2 && strcmp(argv[argi], "--offset") == 0) {
         if (!r3v_render_shape_parse_offset(argv[argi], argv[argi + 1],
                                            &render_shape))
            return 2;
         argi += 2;
      }
   }
   if (argc >= argi + 3 && strcmp(argv[argi], "--extent") == 0) {
      /* Authorization input parses fail-closed: the value is judged in
       * the unnarrowed type against errno, the end pointer, and the
       * admitted bounds before any assignment, so a declaration
       * congruent to an admitted extent modulo 2^32 refuses instead of
       * authorizing the wrong cell.
       */
      const unsigned long bounds[2] = { R300_TRIANGLE_TARGET_WIDTH,
                                        R300_TRIANGLE_TARGET_HEIGHT };
      unsigned long parsed[2];
      for (int axis = 0; axis < 2; axis++) {
         const char *text = argv[argi + 1 + axis];
         /* C23 and POSIX.1-2024 strtoul(3) accept leading white space
          * and an optional sign, so the decimal token is vetted before
          * the numeric parse.
          */
         bool digits_only = text[0] != '\0';
         for (const char *c = text; *c != '\0'; c++) {
            if (*c < '0' || *c > '9')
               digits_only = false;
         }
         if (!digits_only) {
            fprintf(stderr,
                    "extent outside the admitted 1..%u x 1..%u\n",
                    R300_TRIANGLE_TARGET_WIDTH,
                    R300_TRIANGLE_TARGET_HEIGHT);
            return 2;
         }
         char *end = NULL;
         errno = 0;
         parsed[axis] = strtoul(text, &end, 10);
         if (errno != 0 || end == text || *end != '\0' ||
             parsed[axis] < 1 || parsed[axis] > bounds[axis]) {
            fprintf(stderr,
                    "extent outside the admitted 1..%u x 1..%u\n",
                    R300_TRIANGLE_TARGET_WIDTH,
                    R300_TRIANGLE_TARGET_HEIGHT);
            return 2;
         }
      }
      cell_width = (uint32_t)parsed[0];
      cell_height = (uint32_t)parsed[1];
      argi += 3;
   }

   if (argc == argi + 2 && strcmp(argv[argi], "--emit-ib") == 0)
      return emit_selected_ib(argv[argi + 1]);

   if (cell_width != R300_TRIANGLE_TARGET_WIDTH ||
       cell_height != R300_TRIANGLE_TARGET_HEIGHT) {
      fprintf(stderr,
              "non-maximum extent is available only with --emit-ib; "
              "arming reports use the 64x64 reference cell\n");
      return 2;
   }

   /* The runner takes the evidence directory an attended run would use;
    * its freshness is itself an arming factor.
    */
   if (argc != argi + 1) {
      fprintf(stderr,
              "usage: %s [--varying|--compute-identity|--sampled|"
              "--sampled-bgra|--sampled-arm <name>|--composed <offset>|"
              "--msaa <sample-count>|"
              "--shape <w> <h> "
              "<pitch> <bgra|rgba> <r> <g> <b> <a> [--offset <bytes>]] "
              "[--extent <w> <h>] "
              "<evidence-directory> | [--varying|--compute-identity|"
              "--shape ...] [--extent <w> <h>] --emit-ib <path>\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[argi];

   char digest[BLAKE3_OUT_LEN * 2 + 1];
   uint32_t ib_dwords = 0;
   if (cell_digest(digest, &ib_dwords) != 0) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }

   /* The chip identity an attended run would enumerate is supplied
    * rather than probed, so the runner opens no device node.
    */
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
   r3v_native_arming_collect(&facts, vendor_id, device_id,
                             cell_msaa
                                ? R3V_NATIVE_CELL_KIND_TRIANGLE_MSAA_RESOLVE
                             : cell_composed
                                ? R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE
                             : cell_compute_identity
                                ? R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER
                             : cell_sampled
                                ? R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED
                             : cell_render_shape
                                ? R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE
                                : R3V_NATIVE_CELL_KIND_TRIANGLE,
                             digest, evidence_dir, kernel, sizeof(kernel),
                             module, sizeof(module));

   printf("r3v native arming report\n");
   if (cell_render_shape) {
      printf("  render shape           ");
      r3v_render_shape_print(stdout, &render_shape);
      printf("  draw dword 0x%08x color bytes %u\n",
             r300_tcl_bypass_triangle_render_shape_draw_dword(&render_shape),
             r300_tcl_bypass_triangle_render_shape_color_bytes(&render_shape));
   }
   printf("  cell                   %u IB dwords, blake3 %s\n", ib_dwords,
          digest);
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("bundle digest", facts.authorized_ib_blake3,
          facts.actual_ib_blake3);
   printf("  %-22s declared=0x%04x:0x%04x%-22s observed=0x%04x:0x%04x%-20s "
          "%s\n",
          "chip identity", R3V_NATIVE_ARMING_PCI_VENDOR,
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
