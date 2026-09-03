/*
 * SPDX-License-Identifier: MIT
 *
 * Attended rasterizer interpolation probe on RS485M (ATI Radeon Xpress
 * 1150, CHIP_RS480, R300-class US/PFS fixed VLIW): two render passes draw one
 * carrier whose three vertices hold unequal reciprocal clip W and one
 * TEX0 varying, the first pass through the smooth fragment interface
 * and the second through the NoPerspective interface an open probe gate
 * turns into a candidate control word.  The two recorded passes differ
 * in exactly one dword -- RS_INST_0's TEX_ADJ bit (AMD R3xx 3D
 * Registers, RS_INST_[0-15]) or GB_SELECT's W_SELECT bit (AMD R3xx 3D
 * Registers, GB_SELECT) -- so a census of the candidate target against
 * the registered interpolation models names what that one bit does on
 * this silicon.  The control target carries the premise: a
 * perspective-correct classification there is what makes the
 * candidate's classification a statement about the bit rather than
 * about the cell.  The runner's verdict is the classification it
 * prints; a class other than the documented reading is a finding, not a
 * failure.  Every stage prints and flushes before it runs, and the
 * operator supplies the one probe gate the run names.
 */

#include "r3v_native.h"
#include "r3v_interpolation_lowering.h"
#include "r3v_shader_interface.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_watchdog_guard.h"

#include "amd/r300/common/r300_first_draw_state.h"
#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/r300_noperspective_mixed_carrier_fs_block.h"
#include "amd/r300/common/r300_noperspective_reciprocal_fs_block.h"
#include "amd/r300/common/r300_noperspective_mixed_carrier_plan.h"
#include "amd/r300/common/r300_noperspective_q_lane_fs_block.h"
#include "amd/r300/common/r300_noperspective_q_lane_plan.h"
#include "amd/r300/common/r300_r2vb_producer_fs_block.h"
#include "amd/r300/common/r300_rs_tex_adj_probe.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_bo.h"
#include "r3v_native_multi_pass_arms.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* Every printed line also lands in the evidence directory's run.txt, so
 * the retained bundle carries the report the operator read. */
static FILE *run_log = NULL;

static void
emit(const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vprintf(fmt, args);
   va_end(args);
   fflush(stdout);
   if (run_log != NULL) {
      va_start(args, fmt);
      vfprintf(run_log, fmt, args);
      va_end(args);
      fflush(run_log);
   }
}

static void
stage(const char *name)
{
   emit("[stage] %s\n", name);
}

static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

static bool
gate_open(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0;
}

static bool
gate_present(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && value[0] != '\0';
}

/* Whether a dword block occurs contiguously inside a stream. */
static bool
ib_contains_block(const uint32_t *ib, uint32_t ib_dwords,
                  const uint32_t *block, uint32_t block_dwords)
{
   for (uint32_t i = 0; i + block_dwords <= ib_dwords; i++)
      if (memcmp(&ib[i], block, block_dwords * sizeof(uint32_t)) == 0)
         return true;
   return false;
}

/* The census over one image, printed in full: the counts are the
 * finding whether or not the classification names a model. */
static void
report_census(const char *label,
              const struct r300_rs_tex_adj_probe_census *c)
{
   emit("[census] %s judged=%u unjudged_interior=%u unchanged=%u "
        "control_supplied=%d\n",
        label, c->judged, c->unjudged_interior, c->unchanged,
        (int)c->control_supplied);
   for (unsigned m = 0; m < R300_RS_TEX_ADJ_PROBE_MODEL_COUNT; m++)
      emit("[census] %s model=%s match=%u max_deviation=%u\n", label,
           r300_rs_tex_adj_probe_model_name(m), c->match[m],
           c->max_deviation[m]);
   emit("[census] %s classification=%s\n", label,
        r300_rs_tex_adj_probe_classification_name(
           r300_rs_tex_adj_probe_classify(c)));
}

/* The register a PKT0 write at ib[index] targets, or UINT32_MAX when
 * the index sits outside every register write.  The walk mirrors the
 * packet grammar the stream check reads: a type-0 header names a base
 * register and a count, and RADEON_ONE_REG_WR holds the base across
 * the payload instead of advancing it by one register per dword. */
static uint32_t
register_at(const uint32_t *ib, uint32_t dwords, uint32_t index)
{
   uint32_t i = 0;
   while (i < dwords) {
      const uint32_t header = ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFFu) + 1u;
      if (kind == 0) {
         const uint32_t base = (header & 0x3FFFu) * 4u;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         if (index > i && index <= i + count)
            return base + (one_reg ? 0u : 4u * (index - i - 1u));
         i += 1u + count;
      } else if (kind == 3) {
         i += 1u + count;
      } else {
         i += 1u;
      }
   }
   return UINT32_MAX;
}

/* The two draw packets' header indices in a two-pass stream, and the
 * dword just past the first draw packet, so a state check runs over one
 * pass's own span: a span opening inside a draw's payload would read
 * payload dwords as packet headers. */
static bool
draw_indices(const uint32_t *ib, uint32_t dwords, uint32_t out[2],
             uint32_t *first_draw_end)
{
   unsigned draws = 0;
   uint32_t i = 0;
   out[0] = out[1] = UINT32_MAX;
   *first_draw_end = UINT32_MAX;
   while (i < dwords) {
      const uint32_t header = ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFFu) + 1u;
      const uint32_t next = i + ((kind == 0 || kind == 3) ? 1u + count : 1u);
      if (kind == 3 && r300_first_draw_is_draw_packet(header)) {
         if (draws < 2)
            out[draws] = i;
         if (draws == 0)
            *first_draw_end = next;
         draws++;
      }
      i = next;
   }
   return draws == 2 && *first_draw_end != UINT32_MAX;
}

/* One pass's public objects: the image, its memory, its view, and its
 * framebuffer over the shared render pass. */
struct pass_target {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkFramebuffer framebuffer;
};

/* The mixed oracle over one target: red and green (the Smooth s, t)
 * match perspective within one quantum on every judged pixel with
 * affine matching none of their separated pixels; blue and alpha (the
 * NoPerspective s, t) match affine within one quantum on every judged
 * pixel with perspective matching none of their separated pixels; each
 * of the four channels separates the models on some judged pixel; no
 * judged pixel holds the sentinel, and (when the caller supplies the
 * count) none equals the control. */
static bool
judge_mixed(const struct r300_triangle_render_shape *shape,
            const float *candidate_records, const uint32_t *pixels,
            uint32_t size_bytes, uint32_t unchanged, bool flat_location0,
            struct r300_rs_tex_adj_probe_channel_census *ch, bool *clauses)
{
   if (r300_rs_tex_adj_probe_channel_census(shape, candidate_records, pixels,
                                            size_bytes, ch) != 0)
      return false;
   bool separated = ch->judged != 0;
   bool smooth_exact = true, smooth_affine_zero = true;
   bool noperspective_exact = true, noperspective_perspective_zero = true;
   /* A Flat location 0: the logical records replicate the provoking
    * vertex's s, t, so red and green are one constant that both models
    * reproduce and no pixel separates; the constant must match both
    * models on every judged pixel. */
   for (unsigned c = flat_location0 ? 2 : 0; c < 4; c++)
      separated &= ch->separated[c] != 0;
   for (unsigned c = 0; c < 2; c++) {
      smooth_exact &= ch->perspective_match[c] == ch->judged &&
                      ch->perspective_max_deviation[c] <= 1u;
      if (flat_location0) {
         smooth_exact &= ch->affine_match[c] == ch->judged &&
                         ch->affine_max_deviation[c] <= 1u;
         smooth_affine_zero &= ch->separated[c] == 0;
      } else {
         smooth_affine_zero &= ch->affine_on_separated[c] == 0;
      }
      noperspective_exact &= ch->affine_match[2 + c] == ch->judged &&
                             ch->affine_max_deviation[2 + c] <= 1u;
      noperspective_perspective_zero &=
         ch->perspective_on_separated[2 + c] == 0;
   }
   const bool unchanged_zero = unchanged == 0 && ch->sentinel == 0;
   clauses[0] = separated;
   clauses[1] = smooth_exact;
   clauses[2] = smooth_affine_zero;
   clauses[3] = noperspective_exact;
   clauses[4] = noperspective_perspective_zero;
   clauses[5] = unchanged_zero;
   return separated && smooth_exact && smooth_affine_zero &&
          noperspective_exact && noperspective_perspective_zero &&
          unchanged_zero;
}

int
main(int argc, char **argv)
{
   bool record_only = false;
   const char *waiver_path = NULL;
   enum r300_rs_tex_adj_probe_candidate candidate =
      R300_RS_TEX_ADJ_PROBE_TEX_ADJ;
   enum r3v_rs_probe_candidate route_candidate = R3V_RS_PROBE_TEX_ADJ;
   const char *candidate_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
   const char *other_gate = "R3V_NATIVE_RS_W_SELECT_PROBE";
   const char *candidate_word_name = "RS_INST_TEX_ADJ";
   /* The production route: every probe gate closed, the NoPerspective
    * interface selects R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT on
    * its own, and the recorded stream is the gated W_SELECT candidate's
    * byte for byte, so the census judges the public Vulkan NoPerspective
    * route rather than a gated word. */
   bool production = false;
   /* The forced reciprocal-carrier rung: every probe gate closed and
    * R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE=1, the NoPerspective
    * interface selects R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER, and
    * the recorded pass 1 is the TC1 carrier cell
    * (r300_noperspective_reciprocal_plan.h) over the same probe
    * records; the same census judges the US recovery affine. */
   bool carrier_route = false;
   /* The partial-clip rung of the carrier: the same TC1 cell over the
    * probe triangle with vertex 0 past the x = -w plane
    * (r300_rs_tex_adj_probe_partial_clip_vertices), so the driver's
    * clipper packs, cuts, and publishes a fan; the census judges the
    * visible part against the source-triangle models away from the
    * border the clip edge lies on. */
   bool partial = false;
   /* The q-lane rung: every gate closed, the NoPerspective interface a
    * vec3 (the probe attribute's s, t, r) under the narrow pass-through
    * program selects R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE, and
    * pass 1 records the q-lane cell (r300_noperspective_q_lane_plan.h)
    * over the same probe records: the varying cell's register words
    * under the xyz * rcp(w), alpha 1 US program.  The census judges the
    * candidate against the logical records (s, t, r, 1). */
   bool q_lane_route = false;
   /* The mixed carrier rung: every gate closed, the interface a Smooth
    * vec4 at location 0 beside a NoPerspective vec4 at location 1 (both
    * the probe attribute) under the (loc0.xy, loc1.xy) program selects
    * R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER, and pass 1
    * records the sixteen-dword three-vector cell
    * (r300_noperspective_mixed_carrier_plan.h).  The target is
    * (s, t) perspective in red and green beside (s, t) affine in blue
    * and alpha, so the census judges the candidate against the logical
    * records (s, t, s, t) one channel at a time. */
   bool mixed_route = false;
   /* Flat beside NoPerspective on the mixed cell: the interface a Flat
    * vec4 at location 0 beside a NoPerspective vec4 at location 1
    * under the same (loc0.xy, loc1.xy) program selects the mixed route
    * with flat_mask 1, the post-VS stage replicates the provoking
    * (first) vertex's vector across the triangle ahead of the packing,
    * and pass 1 records rung D's cell byte for byte.  The target is
    * the provoking vertex's (s, t) in red and green beside (s, t)
    * affine in blue and alpha. */
   bool flat_mixed_route = false;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0) {
         record_only = true;
      } else if (strcmp(argv[i], "--production") == 0) {
         production = true;
      } else if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc) {
         waiver_path = argv[++i];
      } else if (strcmp(argv[i], "--candidate") == 0 && i + 1 < argc) {
         const char *name = argv[++i];
         if (strcmp(name, "tex-adj") == 0) {
            candidate = R300_RS_TEX_ADJ_PROBE_TEX_ADJ;
            route_candidate = R3V_RS_PROBE_TEX_ADJ;
            candidate_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            other_gate = "R3V_NATIVE_RS_W_SELECT_PROBE";
            candidate_word_name = "RS_INST_TEX_ADJ";
         } else if (strcmp(name, "w-select") == 0) {
            candidate = R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE;
            route_candidate = R3V_RS_PROBE_W_SELECT_ONE;
            candidate_gate = "R3V_NATIVE_RS_W_SELECT_PROBE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "GB_SELECT_W_SELECT";
         } else if (strcmp(name, "reciprocal-carrier") == 0) {
            carrier_route = true;
            candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
            route_candidate = R3V_RS_PROBE_NONE;
            candidate_gate = "R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "RECIPROCAL_CARRIER_TC1";
         } else if (strcmp(name, "reciprocal-q-lane") == 0) {
            q_lane_route = true;
            candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
            route_candidate = R3V_RS_PROBE_NONE;
            candidate_gate = "R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "RECIPROCAL_Q_LANE_VEC3";
         } else if (strcmp(name, "mixed-reciprocal-carrier") == 0) {
            mixed_route = true;
            candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
            route_candidate = R3V_RS_PROBE_NONE;
            candidate_gate = "R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "MIXED_RECIPROCAL_CARRIER_TC2";
         } else if (strcmp(name, "flat-mixed-reciprocal-carrier") == 0) {
            mixed_route = true;
            flat_mixed_route = true;
            candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
            route_candidate = R3V_RS_PROBE_NONE;
            candidate_gate = "R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "FLAT_MIXED_RECIPROCAL_CARRIER_TC2";
         } else if (strcmp(name, "reciprocal-carrier-partial") == 0) {
            carrier_route = true;
            partial = true;
            candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
            route_candidate = R3V_RS_PROBE_NONE;
            candidate_gate = "R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE";
            other_gate = "R3V_NATIVE_RS_TEX_ADJ_PROBE";
            candidate_word_name = "RECIPROCAL_CARRIER_TC1_PARTIAL";
         } else {
            usage_error = true;
         }
      } else {
         usage_error = true;
      }
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--record-only] "
              "[--waiver <path>] [--candidate "
              "tex-adj|w-select|reciprocal-carrier|reciprocal-carrier-partial|"
              "reciprocal-q-lane|mixed-reciprocal-carrier|"
              "flat-mixed-reciprocal-carrier] "
              "[--production]  (production: w-select on the unclipped "
              "triangle or reciprocal-carrier-partial on the clipped one, "
              "every gate unset)\n",
              argv[0]);
      return 2;
   }
   /* The production forms: the w-select candidate on the unclipped
    * triangle, where the public selector resolves the direct GB
    * W_SELECT cell, and the reciprocal-carrier-partial candidate, where
    * it resolves the TC1 carrier cell for the partially clipped
    * triangle; both run with every gate unset. */
   const bool public_partial = production && carrier_route && partial;
   if (production && !public_partial &&
       (carrier_route || candidate != R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE)) {
      fprintf(stderr, "--production rides the w-select candidate or the "
              "reciprocal-carrier-partial candidate: the public selector "
              "resolves the direct GB W_SELECT cell on ACCEPT and the TC1 "
              "carrier cell on PARTIAL\n");
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* The operator arms the candidate: the runner reads the gate the
    * selected candidate names and refuses every other gate state, so
    * the recorded stream is the one the authorization names and the
    * probe pipeline's candidate comes from the environment the
    * authorization declares. */
   if (production || q_lane_route || mixed_route) {
      /* The production, q-lane, and mixed routes open on no gate: a
       * present probe or force gate would hand the NoPerspective
       * interface a candidate or the TC1 carrier instead of the route
       * under test. */
      if (gate_present(candidate_gate) || gate_present(other_gate)) {
         fprintf(stderr, "a gate is set; the %s route runs with %s and %s "
                 "unset\n",
                 q_lane_route ? "q-lane" : mixed_route ? "mixed" : "production",
                 candidate_gate, other_gate);
         return 2;
      }
   } else if (!gate_open(candidate_gate)) {
      fprintf(stderr,
              "%s=1 arms the %s candidate; export it before the run\n",
              candidate_gate, candidate_word_name);
      return 2;
   }
   if ((carrier_route || q_lane_route || mixed_route) &&
       gate_present("R3V_NATIVE_RS_W_SELECT_PROBE")) {
      fprintf(stderr, "R3V_NATIVE_RS_W_SELECT_PROBE is set; the %s "
              "route runs with every probe gate unset\n",
              q_lane_route ? "q-lane" : mixed_route ? "mixed" : "carrier");
      return 2;
   }
   const bool route_based =
      production || carrier_route || q_lane_route || mixed_route;
   /* The q-lane, carrier, and mixed cells differ from the control in
    * the US program (and, for the carriers, the record shape), so the
    * one-dword invariant belongs to the probe words alone. */
   const bool one_word_candidate =
      !carrier_route && !q_lane_route && !mixed_route;
   if (!production && !q_lane_route && !mixed_route &&
       gate_present(other_gate)) {
      fprintf(stderr, "%s is set; one cell carries one candidate\n",
              other_gate);
      return 2;
   }
   if (gate_present("R3V_NATIVE_FLAT_REPLICATION_PINNED")) {
      fprintf(stderr, "R3V_NATIVE_FLAT_REPLICATION_PINNED pins the "
              "replication route; the probe cell needs it unset\n");
      return 2;
   }
   if (gate_present("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") ||
       gate_present("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL") ||
       gate_present("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL")) {
      fprintf(stderr, "the probe cell runs on the CPU route; every "
              "R3V_NATIVE_R2VB_*_EXPERIMENTAL gate stays unset\n");
      return 2;
   }
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }

   if (!record_only) {
      char path[PATH_MAX];
      if (snprintf(path, sizeof(path), "%s/run.txt", evidence_dir) <
          (int)sizeof(path))
         run_log = fopen(path, "w");
      if (run_log == NULL) {
         fprintf(stderr, "the run log refused to open under %s\n",
                 evidence_dir);
         return 1;
      }
   }

   /* The stream the public route records: two varying passes at the
    * reference shape, the second alone carrying the candidate word,
    * bound at merged indices 2 and 3. */
   struct r300_triangle_multi_pass mp;
   if (q_lane_route)
      r3v_native_multi_pass_public_noperspective_q_lane_reference(&mp);
   else if (mixed_route)
      r3v_native_multi_pass_public_noperspective_mixed_carrier_reference(&mp);
   else if (carrier_route)
      r3v_native_multi_pass_public_noperspective_carrier_reference(&mp);
   else
      r3v_native_multi_pass_public_rs_tex_adj_probe_reference(&mp, candidate);
   struct r300_triangle_multi_pass control_mp;
   r3v_native_multi_pass_public_rs_tex_adj_probe_reference(
      &control_mp, R300_RS_TEX_ADJ_PROBE_CONTROL);

   struct r300_rs_tex_adj_probe_plan control_plan, candidate_plan;
   r300_rs_tex_adj_probe_plan_control(&control_plan);
   if (candidate == R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE)
      r300_rs_tex_adj_probe_plan_w_select_one(&candidate_plan);
   else
      r300_rs_tex_adj_probe_plan_tex_adj(&candidate_plan);
   if (r300_rs_tex_adj_probe_plan_validate(&control_plan) != 0 ||
       r300_rs_tex_adj_probe_plan_validate(&candidate_plan) != 0) {
      fprintf(stderr, "a probe plan refused validation\n");
      return 1;
   }

   /* GB_SELECT's contract value: the stream check judges the candidate
    * word against the base the first-draw contract establishes, so the
    * base comes from the contract rather than from a literal. */
   struct r300_first_draw_contract base_contract;
   if (r300_tcl_bypass_triangle_reference_contract(&base_contract) != 0) {
      fprintf(stderr, "the reference contract refused to resolve\n");
      return 1;
   }
   uint32_t gb_select_base = 0;
   bool gb_found = false;
   for (uint32_t i = 0; i < base_contract.count; i++) {
      if (base_contract.entries[i].reg == R300_GB_SELECT) {
         gb_select_base = base_contract.entries[i].value;
         gb_found = true;
      }
   }
   if (!gb_found) {
      fprintf(stderr, "the reference contract carries no GB_SELECT "
              "clause\n");
      return 1;
   }

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&mp.pass[0]);

   struct r300_tcl_bypass_triangle_ib armed, armed_control;
   if (r300_tcl_bypass_triangle_clip_space_multi_pass_emit(&mp, &armed) != 0) {
      fprintf(stderr, "the probe two-pass cell refused to emit\n");
      return 1;
   }
   if (r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
          &control_mp, &armed_control) != 0) {
      fprintf(stderr, "the control two-pass cell refused to emit\n");
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;

   /* The candidate is one bit of one register: the two streams that
    * differ only in pass 1's candidate field differ in exactly one
    * dword, and the enclosing PKT0 header names the register that
    * dword writes. */
   uint32_t differing_index = UINT32_MAX;
   uint32_t differing_count = 0;
   if (one_word_candidate && armed_control.ib_size_dwords != ib_dwords) {
      fprintf(stderr, "the candidate stream is %u dwords against the "
              "control's %u\n",
              ib_dwords, armed_control.ib_size_dwords);
      return 1;
   }
   for (uint32_t i = 0; i < MIN2(ib_dwords, armed_control.ib_size_dwords);
        i++) {
      if (armed.ib[i] != armed_control.ib[i]) {
         if (differing_count == 0)
            differing_index = i;
         differing_count++;
      }
   }
   const uint32_t differing_reg =
      differing_index == UINT32_MAX
         ? UINT32_MAX
         : register_at(armed.ib, ib_dwords, differing_index);

   emit("[shape] rasterizer probe two-draw, control then %s%s, two varying "
        "passes %ux%u pitch %u, binding (%u, %u), %u IB dwords, cell "
        "blake3 %.8s\n",
        candidate_word_name,
        public_partial ? " (public partial-clip fallback, TC1 carrier cell "
                          "selected at submission, no gate)"
        : production    ? " (production direct GB W_SELECT route, no gate)"
        : q_lane_route  ? " (reciprocal q-lane route, vec3, no gate)"
        : flat_mixed_route
                        ? " (mixed reciprocal carrier route, TC0 Flat "
                          "replicated on the host + TC1 NoPerspective + TC2 "
                          "carrier, no gate)"
        : mixed_route   ? " (mixed reciprocal carrier route, TC0 Smooth + "
                          "TC1 NoPerspective + TC2 carrier, no gate)"
        : carrier_route ? " (forced reciprocal carrier route, TC1 cell)"
                        : "",
        mp.pass[0].width, mp.pass[0].height,
        mp.pass[0].pitch_pixels, mp.second_vertex_index,
        mp.second_color_index, ib_dwords, digest);
   emit("[record] candidate-vs-control differing dwords=%u index=%u "
        "register=0x%04x control=0x%08x candidate=0x%08x\n",
        differing_count, differing_index, differing_reg,
        differing_index == UINT32_MAX ? 0u : armed_control.ib[differing_index],
        differing_index == UINT32_MAX ? 0u : armed.ib[differing_index]);
   /* The carrier cell is a record shape of its own -- PSC, VTX_SIZE,
    * RS block, and US program all move -- so the one-dword invariant
    * belongs to the probe words alone. */
   if (one_word_candidate &&
       (differing_count != 1 || differing_reg == UINT32_MAX)) {
      fprintf(stderr, "the candidate stream does not differ from the "
              "control in exactly one register dword\n");
      return 1;
   }

   /* The window records the driver's CPU projection of the clip
    * carrier reproduces: the models, the predicted images, and the
    * census all read these, and the carrier read-back after the
    * submission is what ties them to the bytes the device fetched. */
   float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   float clip_stream[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   if (partial) {
      r300_rs_tex_adj_probe_partial_vertices(&mp.pass[0], records);
      r300_rs_tex_adj_probe_partial_clip_vertices(clip_stream);
   } else {
      r300_rs_tex_adj_probe_vertices(&mp.pass[0], records);
      r300_rs_tex_adj_probe_clip_vertices(clip_stream);
   }
   emit("[geometry] %s: vertex 0 window x=%g (target width %u)\n",
        partial ? "partial clip, one plane x = -w" : "unclipped",
        records[0], mp.pass[0].width);
   /* The candidate's logical records: the q-lane program writes alpha
    * 1, so its models read (s, t, r, 1); every other candidate reads
    * the probe records whole. */
   float candidate_records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   memcpy(candidate_records, records, sizeof(candidate_records));
   if (q_lane_route)
      for (unsigned v = 0; v < 3; v++)
         candidate_records[v * 8 + 7] = 1.0f;
   /* The mixed program stores (smooth.s, smooth.t, noperspective.s,
    * noperspective.t): the logical records are (s, t, s, t), judged
    * perspective in red and green and affine in blue and alpha. */
   if (mixed_route)
      for (unsigned v = 0; v < 3; v++) {
         candidate_records[v * 8 + 6] = records[v * 8 + 4];
         candidate_records[v * 8 + 7] = records[v * 8 + 5];
      }
   /* The Flat mixed program's location 0 is the provoking (first)
    * vertex's s, t on every vertex: rung D's logical records with red
    * and green replicated. */
   float mixed_records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   memcpy(mixed_records, candidate_records, sizeof(mixed_records));
   if (flat_mixed_route)
      for (unsigned v = 1; v < 3; v++) {
         candidate_records[v * 8 + 4] = candidate_records[4];
         candidate_records[v * 8 + 5] = candidate_records[5];
      }

   emit("[models] registered outcomes: %s, %s, %s, %s (also reported as "
        "%s), %s, %s\n",
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_SHIFTED_CENTER),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE_PERTURBED),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_CLASS_UNCHANGED),
        r300_rs_tex_adj_probe_classification_name(
           R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED));

   /* The predicted images, retained ahead of the submission: one per
    * separable model, each the model's UNORM8 dword over the analytic
    * interior and the sentinel elsewhere. */
   static const struct {
      enum r300_rs_tex_adj_probe_model model;
      const char *file;
   } prediction[3] = {
      { R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE, "expected_perspective.bin" },
      { R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE, "expected_affine.bin" },
      { R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q,
        "expected_projective_q.bin" },
   };
   /* The affine prediction is the candidate's expectation and reads
    * its logical records; the perspective and projective ones are the
    * control's. */
   const float *prediction_records[3] = { records, candidate_records,
                                          records };
   uint32_t *expected[3] = { NULL, NULL, NULL };
   for (unsigned p = 0; p < 3; p++) {
      expected[p] = calloc(1, color_bytes);
      if (expected[p] == NULL ||
          r300_rs_tex_adj_probe_expected(&mp.pass[0], prediction_records[p],
                                         prediction[p].model, expected[p],
                                         color_bytes) != 0) {
         fprintf(stderr, "the %s prediction refused to generate\n",
                 prediction[p].file);
         return 1;
      }
      if (!record_only &&
          r3v_native_evidence_write_file(evidence_dir, prediction[p].file,
                                         expected[p], color_bytes) != 0) {
         fprintf(stderr, "prediction retention failed\n");
         return 1;
      }
   }

   /* The predictions against each other: each model's own image is
    * judged over the same pixels, and the pairwise match counts are the
    * separation the census's verdict rests on.  A model that matches
    * another model's image at any judged pixel would leave the census
    * unclassified by construction, so the numbers print here, ahead of
    * the ioctl, where a collapsed separation refuses the run. */
   struct r300_rs_tex_adj_probe_census predicted[3];
   for (unsigned p = 0; p < 3; p++) {
      if (r300_rs_tex_adj_probe_census(&mp.pass[0], prediction_records[p],
                                       expected[p], NULL, color_bytes,
                                       &predicted[p]) != 0) {
         fprintf(stderr, "the census refused the %s prediction\n",
                 prediction[p].file);
         return 1;
      }
      emit("[predict] %s judged=%u classification=%s match(perspective)=%u "
           "match(affine)=%u match(projective-q)=%u "
           "match(shifted-center)=%u\n",
           r300_rs_tex_adj_probe_model_name(prediction[p].model),
           predicted[p].judged,
           r300_rs_tex_adj_probe_classification_name(
              r300_rs_tex_adj_probe_classify(&predicted[p])),
           predicted[p].match[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE],
           predicted[p].match[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE],
           predicted[p].match[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q],
           predicted[p].match[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER]);
   }
   emit("[predict] tolerance=%u separation=%u UNORM8 quanta per channel\n",
        (unsigned)R300_RS_TEX_ADJ_PROBE_TOLERANCE,
        (unsigned)R300_RS_TEX_ADJ_PROBE_SEPARATION);
   emit("[predict] falsifier: a recorded stream whose digest differs from "
        "the emitter's, or whose plan registers do not hold their words "
        "ahead of their own pass's draw, refuses before any ioctl; a "
        "control target the census does not classify perspective refuses "
        "the premise, and the run is a finding about the control cell; a "
        "candidate census judging zero pixels carries no claim about the "
        "bit\n");

   /* The known-good and known-bad calibration of the census against the
    * predictions themselves: the perspective image classifies
    * perspective on every judged pixel and the affine image classifies
    * affine, so a predict/census pairing defect fails here rather than
    * on the one attended submission. */
   {
      const enum r300_rs_tex_adj_probe_classification expect[2] = {
         R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE,
         R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE,
      };
      bool calibrated = true;
      for (unsigned p = 0; p < 2; p++)
         calibrated &=
            predicted[p].judged != 0 &&
            r300_rs_tex_adj_probe_classify(&predicted[p]) == expect[p] &&
            predicted[p].match[prediction[p].model] == predicted[p].judged;
      emit("[calibration] census over the predictions: %s\n",
           calibrated ? "perspective and affine images classify as "
                        "themselves on every judged pixel"
                      : "a prediction does not classify as itself");
      if (!calibrated) {
         fprintf(stderr, "the census is not calibrated against its own "
                 "predictions; refusing ahead of the ioctl\n");
         return 1;
      }
   }
   /* The mixed rung's prediction: red and green from the perspective
    * image, blue and alpha from the affine image, both over the
    * logical records (s, t, s, t); the mixed oracle holds on it and
    * fails on each pure image, the known-good and known-bad
    * calibration of the per-channel verdict. */
   uint32_t *expected_mixed = NULL;
   if (mixed_route) {
      expected_mixed = calloc(1, color_bytes);
      uint32_t *perspective_logical = calloc(1, color_bytes);
      if (expected_mixed == NULL || perspective_logical == NULL ||
          r300_rs_tex_adj_probe_expected(
             &mp.pass[0], candidate_records,
             R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE, perspective_logical,
             color_bytes) != 0) {
         fprintf(stderr, "the mixed prediction refused to generate\n");
         return 1;
      }
      /* B8G8R8A8: red and green at bits 8..23, blue and alpha at bits
       * 0..7 and 24..31. */
      for (uint32_t i = 0; i < color_bytes / 4u; i++)
         expected_mixed[i] = (perspective_logical[i] & 0x00ffff00u) |
                             (expected[1][i] & 0xff0000ffu);
      struct r300_rs_tex_adj_probe_channel_census ch;
      bool clauses[6];
      const bool good = judge_mixed(&mp.pass[0], candidate_records,
                                    expected_mixed, color_bytes, 0,
                                    flat_mixed_route, &ch, clauses);
      const bool bad_perspective =
         judge_mixed(&mp.pass[0], candidate_records, perspective_logical,
                     color_bytes, 0, flat_mixed_route, &ch, clauses);
      const bool bad_affine = judge_mixed(&mp.pass[0], candidate_records,
                                          expected[1], color_bytes, 0,
                                          flat_mixed_route, &ch, clauses);
      /* Over the replicated records the pure affine image is the
       * flat-mixed prediction itself (a constant in red and green,
       * affine in blue and alpha), so under the Flat location it is an
       * alias the oracle must hold on; its known-bad is instead rung
       * D's own image, red and green interpolated from each vertex's
       * s, t, which a dropped Flat qualifier would render. */
      bool bad_interpolated = false;
      if (flat_mixed_route) {
         uint32_t *interpolated = calloc(1, color_bytes);
         uint32_t *rung_d = calloc(1, color_bytes);
         if (interpolated == NULL || rung_d == NULL ||
             r300_rs_tex_adj_probe_expected(
                &mp.pass[0], mixed_records,
                R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE, interpolated,
                color_bytes) != 0) {
            fprintf(stderr, "the interpolated prediction refused to "
                    "generate\n");
            return 1;
         }
         for (uint32_t i = 0; i < color_bytes / 4u; i++)
            rung_d[i] = (interpolated[i] & 0x00ffff00u) |
                        (expected[1][i] & 0xff0000ffu);
         bad_interpolated = judge_mixed(&mp.pass[0], candidate_records,
                                        rung_d, color_bytes, 0, true, &ch,
                                        clauses);
         free(interpolated);
         free(rung_d);
      }
      emit("[calibration] %s oracle over the predictions: %s image "
           "%s, pure perspective %s, pure affine %s%s; separated=(%u, %u, "
           "%u, %u) over %u judged\n",
           flat_mixed_route ? "flat-mixed" : "mixed",
           flat_mixed_route ? "flat-mixed" : "mixed",
           good ? "holds" : "fails", bad_perspective ? "holds" : "fails",
           bad_affine ? "holds" : "fails",
           flat_mixed_route
              ? (bad_interpolated ? " (an alias of the prediction), rung D "
                                    "interpolated image holds"
                                  : " (an alias of the prediction), rung D "
                                    "interpolated image fails")
              : "",
           ch.separated[0], ch.separated[1], ch.separated[2],
           ch.separated[3], ch.judged);
      free(perspective_logical);
      if (!good || bad_perspective || bad_affine != flat_mixed_route ||
          bad_interpolated) {
         fprintf(stderr, "the mixed oracle is not calibrated against its "
                 "own predictions; refusing ahead of the ioctl\n");
         return 1;
      }
      if (!record_only &&
          r3v_native_evidence_write_file(evidence_dir, "expected_mixed.bin",
                                         expected_mixed, color_bytes) != 0) {
         fprintf(stderr, "prediction retention failed\n");
         return 1;
      }
   }

   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared != NULL && declared[0] != '\0' &&
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s\n",
              declared, evidence_dir);
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", evidence_dir, 1);

   struct r3v_native_watchdog_guard guard = {0};
   if (!record_only) {
      stage("watchdog");
      if (r3v_native_watchdog_guard_open(&guard, waiver_path, evidence_dir,
                                         digest) != 0)
         return 2;
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return 1;
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   emit("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
        props.deviceID, props.deviceName);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS485M\n");
      return 1;
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(
      pdev,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &priority,
            },
      },
      NULL, &device);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", result);
      return 1;
   }

   r3v_native_watchdog_guard_install(&guard, device);

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateBuffer);
   LOAD_DEVICE(vkDestroyBuffer);
   LOAD_DEVICE(vkBindBufferMemory);
   LOAD_DEVICE(vkCreateImage);
   LOAD_DEVICE(vkDestroyImage);
   LOAD_DEVICE(vkGetImageMemoryRequirements);
   LOAD_DEVICE(vkBindImageMemory);
   LOAD_DEVICE(vkCreateImageView);
   LOAD_DEVICE(vkDestroyImageView);
   LOAD_DEVICE(vkCreateRenderPass);
   LOAD_DEVICE(vkDestroyRenderPass);
   LOAD_DEVICE(vkCreateFramebuffer);
   LOAD_DEVICE(vkDestroyFramebuffer);
   LOAD_DEVICE(vkCreateShaderModule);
   LOAD_DEVICE(vkDestroyShaderModule);
   LOAD_DEVICE(vkCreatePipelineLayout);
   LOAD_DEVICE(vkDestroyPipelineLayout);
   LOAD_DEVICE(vkCreateGraphicsPipelines);
   LOAD_DEVICE(vkDestroyPipeline);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkCmdBeginRenderPass);
   LOAD_DEVICE(vkCmdEndRenderPass);
   LOAD_DEVICE(vkCmdBindPipeline);
   LOAD_DEVICE(vkCmdBindVertexBuffers);
   LOAD_DEVICE(vkCmdDraw);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

#define CHECK(call)                                        \
   do {                                                    \
      VkResult check_result = (call);                      \
      if (check_result != VK_SUCCESS) {                    \
         fprintf(stderr, "%s: %d\n", #call, check_result); \
         return 1;                                         \
      }                                                    \
   } while (0)

   /* Two color targets, each the 64x64 B8G8R8A8 surface whose memory
    * requirement is the cell footprint with the canary row; both carry
    * the sentinel before the submission, so every dword either target
    * holds afterward names its writer. */
   stage("targets");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = R3V_NATIVE_TARGET_FORMAT,
      .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   struct pass_target target[2] = { { VK_NULL_HANDLE } };
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImage(device, &image_info, NULL, &target[i].image));
      VkMemoryRequirements reqs;
      vkGetImageMemoryRequirements(device, target[i].image, &reqs);
      if (reqs.size != R3V_NATIVE_TARGET_MEMORY_BYTES ||
          reqs.size < color_bytes) {
         fprintf(stderr, "target %u requirement %llu is not the cell "
                 "footprint %u\n",
                 i, (unsigned long long)reqs.size, color_bytes);
         return 1;
      }
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = 0,
         },
         NULL, &target[i].memory));
      CHECK(vkBindImageMemory(device, target[i].image, target[i].memory, 0));
      void *map = NULL;
      CHECK(vkMapMemory(device, target[i].memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < reqs.size / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, target[i].memory);
   }

   /* The application's vertex records: the probe carrier in clip space,
    * three eight-dword records the two-attribute vertex module reads as
    * a pass-through position at location 0 and the TEX0 payload at
    * location 1.  Both passes draw the same three records, so the
    * carrier the driver projects is the same for each and the passes
    * differ in the one control word alone. */
   stage("vertex stream");
   for (unsigned v = 0; v < 3; v++) {
      const float *pos = &clip_stream[v * 8];
      const float w = 1.0f / r300_rs_tex_adj_probe_reciprocal_w[v];
      /* The partial form places vertex 0 alone past x = -w. */
      const bool x_inside = partial && v == 0
                               ? pos[0] < -pos[3]
                               : pos[0] >= -pos[3] && pos[0] <= pos[3];
      if (!(pos[3] == w) || !x_inside ||
          !(pos[1] >= -pos[3] && pos[1] <= pos[3]) ||
          !(pos[2] >= 0.0f && pos[2] <= pos[3])) {
         fprintf(stderr,
                 "carrier vertex %u (%g, %g, %g, %g) leaves the clip "
                 "volume\n",
                 v, pos[0], pos[1], pos[2], pos[3]);
         return 1;
      }
   }
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = 4096,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &vertex_memory));
   CHECK(vkCreateBuffer(device,
                        &(VkBufferCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = sizeof(clip_stream),
                           .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                        },
                        NULL, &vertex_buffer));
   CHECK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      memcpy(map, clip_stream, sizeof(clip_stream));
      vkUnmapMemory(device, vertex_memory);
   }

   stage("pipelines");
   VkRenderPass pass = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   CHECK(vkCreateRenderPass(
      device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments =
            &(VkAttachmentDescription){
               .format = R3V_NATIVE_TARGET_FORMAT,
               .samples = VK_SAMPLE_COUNT_1_BIT,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
               .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
         .subpassCount = 1,
         .pSubpasses =
            &(VkSubpassDescription){
               .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
               .colorAttachmentCount = 1,
               .pColorAttachments =
                  &(VkAttachmentReference){
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_GENERAL,
                  },
            },
      },
      NULL, &pass));
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImageView(
         device,
         &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = target[i].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = R3V_NATIVE_TARGET_FORMAT,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1,
                                  .layerCount = 1 },
         },
         NULL, &target[i].view));
      CHECK(vkCreateFramebuffer(
         device,
         &(VkFramebufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = pass,
            .attachmentCount = 1,
            .pAttachments = &target[i].view,
            .width = R3V_NATIVE_TARGET_WIDTH,
            .height = R3V_NATIVE_TARGET_HEIGHT,
            .layers = 1,
         },
         NULL, &target[i].framebuffer));
   }
   CHECK(vkCreatePipelineLayout(
      device,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      },
      NULL, &layout));

   /* One vertex module over both pipelines: position at location 0
    * passes through and the location-1 attribute becomes the location-0
    * varying, so the record shape the carrier carries is the probe's
    * eight dwords.  The fragment modules differ in the interpolation
    * qualifier alone -- the smooth interface takes no candidate and the
    * NoPerspective interface takes the gated one -- which is what makes
    * the two recorded passes differ in one control word. */
   /* The q-lane rung's candidate pipeline narrows the attribute to its
    * xyz as a vec3 varying under the vec3 narrow pass-through; the
    * control keeps the vec4 pair. */
   /* The mixed rung's candidate pipeline stores the attribute to both
    * locations under the (loc0.xy, loc1.xy) program. */
   const uint32_t *vertex_words[2] = {
      r3v_reference_vertex_two_attributes_spirv,
      q_lane_route ? r3v_reference_vertex_two_attributes_vec3_spirv
      : mixed_route
         ? r3v_reference_vertex_two_attributes_mixed_carrier_spirv
         : r3v_reference_vertex_two_attributes_spirv,
   };
   const size_t vertex_bytes[2] = {
      sizeof(r3v_reference_vertex_two_attributes_spirv),
      q_lane_route ? sizeof(r3v_reference_vertex_two_attributes_vec3_spirv)
      : mixed_route
         ? sizeof(r3v_reference_vertex_two_attributes_mixed_carrier_spirv)
         : sizeof(r3v_reference_vertex_two_attributes_spirv),
   };
   const uint32_t *fragment_words[2] = {
      r3v_reference_fragment_varying_spirv,
      q_lane_route ? r3v_reference_fragment_noperspective_vec3_spirv
      : flat_mixed_route ? r3v_reference_fragment_flat_mixed_carrier_spirv
      : mixed_route ? r3v_reference_fragment_mixed_carrier_spirv
                    : r3v_reference_fragment_noperspective_spirv,
   };
   const size_t fragment_bytes[2] = {
      sizeof(r3v_reference_fragment_varying_spirv),
      q_lane_route ? sizeof(r3v_reference_fragment_noperspective_vec3_spirv)
      : flat_mixed_route
         ? sizeof(r3v_reference_fragment_flat_mixed_carrier_spirv)
      : mixed_route ? sizeof(r3v_reference_fragment_mixed_carrier_spirv)
                    : sizeof(r3v_reference_fragment_noperspective_spirv),
   };
   VkPipeline pipeline[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   for (unsigned i = 0; i < 2; i++) {
      VkShaderModule vs = VK_NULL_HANDLE;
      CHECK(vkCreateShaderModule(
         device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = vertex_bytes[i],
            .pCode = vertex_words[i],
         },
         NULL, &vs));
      VkShaderModule fs = VK_NULL_HANDLE;
      CHECK(vkCreateShaderModule(
         device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = fragment_bytes[i],
            .pCode = fragment_words[i],
         },
         NULL, &fs));
      const VkGraphicsPipelineCreateInfo pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages =
            (VkPipelineShaderStageCreateInfo[]){
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                 .module = vs,
                 .pName = "main" },
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                 .module = fs,
                 .pName = "main" },
            },
         .pVertexInputState =
            &(VkPipelineVertexInputStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
               .vertexBindingDescriptionCount = 1,
               .pVertexBindingDescriptions =
                  &(VkVertexInputBindingDescription){
                     .binding = 0,
                     .stride = 32,
                     .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                  },
               .vertexAttributeDescriptionCount = 2,
               .pVertexAttributeDescriptions =
                  (VkVertexInputAttributeDescription[]){
                     { .location = 0,
                       .binding = 0,
                       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                       .offset = 0 },
                     { .location = 1,
                       .binding = 0,
                       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                       .offset = 16 },
                  },
            },
         .pInputAssemblyState =
            &(VkPipelineInputAssemblyStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            },
         .pViewportState =
            &(VkPipelineViewportStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
               .viewportCount = 1,
               .pViewports =
                  &(VkViewport){
                     .width = (float)R3V_NATIVE_TARGET_WIDTH,
                     .height = (float)R3V_NATIVE_TARGET_HEIGHT,
                     .maxDepth = 1.0f,
                  },
               .scissorCount = 1,
               .pScissors =
                  &(VkRect2D){
                     .extent = { R3V_NATIVE_TARGET_WIDTH,
                                 R3V_NATIVE_TARGET_HEIGHT },
                  },
            },
         .pRasterizationState =
            &(VkPipelineRasterizationStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
               .polygonMode = VK_POLYGON_MODE_FILL,
               .cullMode = VK_CULL_MODE_NONE,
               .lineWidth = 1.0f,
            },
         .pMultisampleState =
            &(VkPipelineMultisampleStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            },
         .pColorBlendState =
            &(VkPipelineColorBlendStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               .attachmentCount = 1,
               .pAttachments =
                  &(VkPipelineColorBlendAttachmentState){
                     .colorWriteMask =
                        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                  },
            },
         .layout = layout,
         .renderPass = pass,
      };
      CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_info, NULL, &pipeline[i]));
      vkDestroyShaderModule(device, fs, NULL);
      vkDestroyShaderModule(device, vs, NULL);
   }

   /* The route the two interfaces took, read off the pipelines and
    * re-derived through the selector so the reason is printed beside
    * the recorded field.  The control takes no candidate and the
    * candidate takes the gated one; any other pair leaves the run
    * without a control premise, so it refuses here. */
   {
      static const char *const candidate_name[3] = { "none", "tex-adj",
                                                     "w-select-one" };
      enum r3v_rs_probe_candidate recorded[2];
      const char *reason[2] = { NULL, NULL };
      for (unsigned i = 0; i < 2 && !route_based; i++) {
         VK_FROM_HANDLE(r3v_native_pipeline, native_pipeline, pipeline[i]);
         recorded[i] = native_pipeline->rs_probe_candidate;
         const struct r3v_rs_probe_query query = {
            .tex_adj_gate = gate_open("R3V_NATIVE_RS_TEX_ADJ_PROBE"),
            .w_select_gate = gate_open("R3V_NATIVE_RS_W_SELECT_PROBE"),
            .cpu_delivery = true,
            .triangle_list = true,
            .link = &native_pipeline->shader_interface,
            .rs_destination_available = native_pipeline->varying,
            .fragment_consumes_destination = native_pipeline->varying,
         };
         const enum r3v_rs_probe_candidate derived =
            r3v_rs_probe_candidate_select(&query, &reason[i]);
         if (derived != recorded[i]) {
            fprintf(stderr, "pipeline %u records candidate %u while the "
                    "selector derives %u\n",
                    i, (unsigned)recorded[i], (unsigned)derived);
            return 1;
         }
      }
      /* The production route: neither pipeline carries a probe
       * candidate; the Smooth interface replicates and the NoPerspective
       * interface selects the direct GB W_SELECT route, re-derived
       * through the route selector.  The deferred draws then carry the
       * W_SELECT_ONE control word from the route itself. */
      if (route_based) {
         static const char *const route_name[8] = {
            "replicate", "direct-ga-color0", "direct-gb-w-select",
            "unsupported", "reciprocal-carrier", "reciprocal-q-lane",
            "mixed-reciprocal-carrier", "w-select-or-reciprocal-carrier"
         };
         /* The public NoPerspective pipeline is created on the adaptive
          * route; the concrete cell is selected at submission and
          * reported after the recording below. */
         const enum r3v_interpolation_route expected_route =
            q_lane_route  ? R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE
            : mixed_route
               ? R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER
            : production
               ? R3V_INTERPOLATION_ROUTE_W_SELECT_OR_RECIPROCAL_CARRIER
            : carrier_route ? R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER
                            : R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT;
         enum r3v_interpolation_route route[2];
         for (unsigned i = 0; i < 2; i++) {
            VK_FROM_HANDLE(r3v_native_pipeline, native_pipeline,
                           pipeline[i]);
            recorded[i] = native_pipeline->rs_probe_candidate;
            route[i] = native_pipeline->interpolation_route;
            const struct r3v_interpolation_query query = {
               .cpu_delivery = true,
               .triangle_list = true,
               .clip_class = R3V_INTERPOLATION_CLIP_DEFERRED,
               .link = &native_pipeline->shader_interface,
               .rs_destination_available = native_pipeline->varying,
               .fragment_consumes_destination = native_pipeline->varying,
               .provoking_first_representable = true,
               .carrier_forced = carrier_route && !production,
               .narrow_passthrough_width = q_lane_route && i == 1 ? 3u : 0u,
               .mixed_carrier_fragment = mixed_route && i == 1,
            };
            const enum r3v_interpolation_route derived =
               r3v_interpolation_route_select(&query, &reason[i]);
            if (derived != route[i]) {
               fprintf(stderr, "pipeline %u records route %u while the "
                       "selector derives %u\n",
                       i, (unsigned)route[i], (unsigned)derived);
               return 1;
            }
         }
         emit("[route] %s control=%s (%s) noperspective=%s (%s) "
              "probe candidates=%s/%s\n",
              public_partial ? "public-partial-clip"
              : production ? "production" : q_lane_route ? "q-lane"
              : flat_mixed_route ? "flat-mixed-carrier"
              : mixed_route ? "mixed-carrier" : "forced-carrier",
              route_name[route[0]], reason[0], route_name[route[1]],
              reason[1], candidate_name[recorded[0]],
              candidate_name[recorded[1]]);
         if (recorded[0] != R3V_RS_PROBE_NONE ||
             recorded[1] != R3V_RS_PROBE_NONE ||
             route[0] != R3V_INTERPOLATION_ROUTE_REPLICATE ||
             route[1] != expected_route) {
            fprintf(stderr, "the route-based run needs the Smooth "
                    "interface on replication and the NoPerspective "
                    "interface on the %s route with no probe candidate\n",
                    route_name[expected_route]);
            if (!record_only)
               r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
            return 2;
         }
      } else {
         emit("[route] control=%s (%s) candidate=%s (%s)\n",
              candidate_name[recorded[0]],
              reason[0] != NULL ? reason[0] : "candidate selected",
              candidate_name[recorded[1]],
              reason[1] != NULL ? reason[1] : "candidate selected");
      }
      {
         VK_FROM_HANDLE(r3v_native_pipeline, native_control, pipeline[0]);
         VK_FROM_HANDLE(r3v_native_pipeline, native_candidate, pipeline[1]);
         emit("[interface] control noperspective_mask=0x%x candidate "
              "noperspective_mask=0x%x varying_mask=0x%x/0x%x\n",
              native_control->shader_interface.noperspective_mask,
              native_candidate->shader_interface.noperspective_mask,
              native_control->shader_interface.varying_mask,
              native_candidate->shader_interface.varying_mask);
      }
      if (!route_based && (recorded[0] != R3V_RS_PROBE_NONE ||
                           recorded[1] != route_candidate)) {
         fprintf(stderr, "the smooth interface must take no candidate and "
                 "the NoPerspective interface the armed one\n");
         if (!record_only)
            r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
         return 2;
      }
   }

   /* Two render passes with a draw each over the same three records:
    * pass 0 through the control pipeline into its own target and pass 1
    * through the candidate pipeline into its own. */
   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   CHECK(vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool));
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   CHECK(vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd));
   CHECK(vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           }));
   const float sentinel = (float)0xa5 / 255.0f;
   const VkClearValue clear = { .color = { .float32 = { sentinel, sentinel,
                                                        sentinel,
                                                        sentinel } } };
   for (unsigned i = 0; i < 2; i++) {
      vkCmdBeginRenderPass(
         cmd,
         &(VkRenderPassBeginInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass,
            .framebuffer = target[i].framebuffer,
            .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                        R3V_NATIVE_TARGET_HEIGHT } },
            .clearValueCount = 1,
            .pClearValues = &clear,
         },
         VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline[i]);
      vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &(VkDeviceSize){ 0 });
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
   }
   CHECK(vkEndCommandBuffer(cmd));

   /* The public pipeline's adaptive route resolves here, as the queue
    * resolves it ahead of the arming digest: the CPU vertex execution
    * judges the recorded triangle and the command buffer's IB becomes
    * the concrete cell's, so the digest below is the one the
    * submission arms and the [route] line names the cell the device
    * executes. */
   if (production) {
      VK_FROM_HANDLE(r3v_native_device, select_device, device);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, select_cmd, cmd);
      const bool adaptive_before =
         select_cmd->deferred_draw_count == 2 &&
         select_cmd->deferred_draws[1].adaptive_noperspective;
      const VkResult selected =
         r3v_native_cmd_buffer_select_deferred_routes(select_device,
                                                      select_cmd);
      const struct r3v_native_deferred_draw *selected_draw =
         &select_cmd->deferred_draws[1];
      const char *selected_name =
         selected_draw->noperspective_carrier   ? "reciprocal-carrier"
         : selected_draw->direct_noperspective  ? "direct-gb-w-select"
                                                : "unresolved";
      emit("[route] selected=%s (adaptive route judged %s at submission, "
           "result %d)\n",
           selected_name, public_partial ? "PARTIAL" : "ACCEPT",
           (int)selected);
      const bool expected_selection =
         selected == VK_SUCCESS && adaptive_before &&
         !selected_draw->adaptive_noperspective &&
         selected_draw->alternate_ib == NULL &&
         (public_partial
             ? selected_draw->noperspective_carrier &&
                  !selected_draw->direct_noperspective &&
                  selected_draw->rs_probe_candidate ==
                     (uint8_t)R3V_RS_PROBE_NONE &&
                  selected_draw->post_vs.reciprocal_carrier
             : selected_draw->direct_noperspective &&
                  !selected_draw->noperspective_carrier &&
                  selected_draw->rs_probe_candidate ==
                     (uint8_t)R3V_RS_PROBE_W_SELECT_ONE &&
                  !selected_draw->post_vs.reciprocal_carrier);
      if (!expected_selection) {
         fprintf(stderr, "the adaptive route did not resolve to the %s "
                 "cell for the %s triangle\n",
                 public_partial ? "TC1 carrier" : "direct GB W_SELECT",
                 public_partial ? "partially clipped" : "unclipped");
         if (!record_only)
            r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
         return 2;
      }
   }

   stage("recorded stream");
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, cmd);
   char recorded_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(native->ib, native->ib_size_dwords,
                               recorded_digest);
   emit("[record] kind=%d references=%u deferred_draws=%u ib_dwords=%u "
        "recorded blake3 %.8s emitted blake3 %.8s\n",
        (int)native->cell_kind, native->reference_count,
        native->deferred_draw_count, native->ib_size_dwords, recorded_digest,
        digest);
   const bool stream_agrees =
      native->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS &&
      native->reference_count == 4 && native->ib_size_dwords == ib_dwords &&
      strcmp(recorded_digest, digest) == 0;
   if (native->deferred_draw_count != 2 ||
       native->deferred_draws[0].rs_probe_candidate != R3V_RS_PROBE_NONE ||
       native->deferred_draws[1].rs_probe_candidate != route_candidate) {
      fprintf(stderr, "the two deferred draws do not carry the control "
              "then the candidate\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }
   if (!stream_agrees) {
      uint32_t differing = 0;
      const uint32_t common = ib_dwords < native->ib_size_dwords
                                 ? ib_dwords
                                 : native->ib_size_dwords;
      for (uint32_t i = 0; i < common; i++) {
         if (armed.ib[i] != native->ib[i]) {
            if (differing < 8)
               fprintf(stderr, "dword %u: recorded 0x%08x emitted 0x%08x\n",
                       i, native->ib[i], armed.ib[i]);
            differing++;
         }
      }
      fprintf(stderr, "%u differing dwords over %u common\n", differing,
              common);
      fprintf(stderr, "the public recording is not the authorized stream; "
              "refusing ahead of the ioctl\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }

   /* Each plan's registers ahead of its own pass's draw, read from the
    * recorded bytes: the control words stand ahead of the first draw,
    * the candidate words ahead of the second, and neither plan holds
    * across both draws.  The whole-stream candidate check therefore
    * names the first draw, -1. */
   uint32_t draw_index[2];
   uint32_t first_draw_end = 0;
   if (!draw_indices(native->ib, native->ib_size_dwords, draw_index,
                     &first_draw_end)) {
      fprintf(stderr, "the recorded stream does not carry two draw "
              "packets\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }
   const int control_draws = r300_rs_tex_adj_probe_plan_stream_check(
      &control_plan, gb_select_base, native->ib, draw_index[1]);
   /* The carrier pass is judged by the carrier plan's own stream
    * check -- every widened-record, RS, and GB_SELECT word ahead of the
    * second draw -- in place of the probe word check. */
   struct r300_noperspective_reciprocal_plan carrier_plan;
   r300_noperspective_reciprocal_plan_tc1(&carrier_plan);
   struct r300_noperspective_q_lane_plan q_lane_plan;
   r300_noperspective_q_lane_plan_init(&q_lane_plan, 3);
   struct r300_noperspective_mixed_carrier_plan mixed_plan;
   r300_noperspective_mixed_carrier_plan_first(&mixed_plan);
   const int candidate_whole =
      q_lane_route
         ? r300_noperspective_q_lane_plan_stream_check(
              &q_lane_plan, gb_select_base, native->ib,
              native->ib_size_dwords)
      : mixed_route
         ? r300_noperspective_mixed_carrier_plan_stream_check(
              &mixed_plan, gb_select_base, native->ib,
              native->ib_size_dwords)
      : carrier_route
         ? r300_noperspective_reciprocal_plan_stream_check(
              &carrier_plan, gb_select_base, native->ib,
              native->ib_size_dwords)
         : r300_rs_tex_adj_probe_plan_stream_check(
              &candidate_plan, gb_select_base, native->ib,
              native->ib_size_dwords);
   const int candidate_draws =
      q_lane_route
         ? r300_noperspective_q_lane_plan_stream_check(
              &q_lane_plan, gb_select_base, native->ib + first_draw_end,
              native->ib_size_dwords - first_draw_end)
      : mixed_route
         ? r300_noperspective_mixed_carrier_plan_stream_check(
              &mixed_plan, gb_select_base, native->ib + first_draw_end,
              native->ib_size_dwords - first_draw_end)
      : carrier_route
         ? r300_noperspective_reciprocal_plan_stream_check(
              &carrier_plan, gb_select_base, native->ib + first_draw_end,
              native->ib_size_dwords - first_draw_end)
         : r300_rs_tex_adj_probe_plan_stream_check(
              &candidate_plan, gb_select_base, native->ib + first_draw_end,
              native->ib_size_dwords - first_draw_end);
   /* The q-lane cell shares the control's register words, so its
    * register check passes both draws; the US program is what moves,
    * and the two passes are told apart by the block each carries:
    * the pass-through block ahead of the first draw and the q-lane
    * block ahead of the second. */
   const bool q_lane_blocks =
      !q_lane_route ||
      (ib_contains_block(native->ib, first_draw_end,
                         r300_r2vb_producer_fs_block,
                         sizeof(r300_r2vb_producer_fs_block) / 4u) &&
       !ib_contains_block(native->ib, first_draw_end,
                          r300_noperspective_q_lane_fs_block,
                          sizeof(r300_noperspective_q_lane_fs_block) / 4u) &&
       ib_contains_block(native->ib + first_draw_end,
                         native->ib_size_dwords - first_draw_end,
                         r300_noperspective_q_lane_fs_block,
                         sizeof(r300_noperspective_q_lane_fs_block) / 4u) &&
       !ib_contains_block(native->ib + first_draw_end,
                          native->ib_size_dwords - first_draw_end,
                          r300_r2vb_producer_fs_block,
                          sizeof(r300_r2vb_producer_fs_block) / 4u));
   /* The mixed cell: the pass-through block ahead of the first draw,
    * the mixed block alone ahead of the second. */
   const bool mixed_blocks =
      !mixed_route ||
      (ib_contains_block(native->ib, first_draw_end,
                         r300_r2vb_producer_fs_block,
                         sizeof(r300_r2vb_producer_fs_block) / 4u) &&
       !ib_contains_block(native->ib, first_draw_end,
                          r300_noperspective_mixed_carrier_fs_block,
                          sizeof(r300_noperspective_mixed_carrier_fs_block) /
                             4u) &&
       ib_contains_block(native->ib + first_draw_end,
                         native->ib_size_dwords - first_draw_end,
                         r300_noperspective_mixed_carrier_fs_block,
                         sizeof(r300_noperspective_mixed_carrier_fs_block) /
                            4u) &&
       !ib_contains_block(native->ib + first_draw_end,
                          native->ib_size_dwords - first_draw_end,
                          r300_r2vb_producer_fs_block,
                          sizeof(r300_r2vb_producer_fs_block) / 4u) &&
       !ib_contains_block(native->ib + first_draw_end,
                          native->ib_size_dwords - first_draw_end,
                          r300_noperspective_reciprocal_fs_block,
                          sizeof(r300_noperspective_reciprocal_fs_block) /
                             4u));
   emit("[state] gb_select_base=0x%08x control-over-first-pass=%d "
        "candidate-over-stream=%d candidate-over-second-pass=%d "
        "q_lane_blocks=%d mixed_blocks=%d\n",
        gb_select_base, control_draws, candidate_whole, candidate_draws,
        (int)q_lane_blocks, (int)mixed_blocks);
   if (control_draws != 1 || candidate_whole != (q_lane_route ? 2 : -1) ||
       candidate_draws != 1 || !q_lane_blocks || !mixed_blocks) {
      fprintf(stderr, "the recorded stream does not establish the control "
              "plan ahead of the first draw and the candidate plan ahead "
              "of the second; refusing ahead of the ioctl\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }

   /* The recorded stream and the reference list the winsys turns into
    * the relocation entries, retained ahead of the ioctl under names
    * the queue's own retention leaves alone: r3v_native_queue_submit
    * publishes ib.bin, relocs.bin, and manifest.json through
    * r3v_native_evidence_require_fresh, which refuses the submission
    * ahead of the ioctl when any of the three already exists.  The
    * attended run retains into the evidence directory the queue writes
    * to; the record-only pass retains into a fresh scratch directory
    * (publication is no-clobber link(), so a shared directory admits
    * one retention) and removes it after the check, so the same
    * freshness proof runs on the shim fixture. */
   char retain_dir[PATH_MAX];
   if (record_only) {
      snprintf(retain_dir, sizeof(retain_dir), "%s/record-XXXXXX",
               evidence_dir);
      if (mkdtemp(retain_dir) == NULL) {
         fprintf(stderr, "record scratch directory failed: %s\n",
                 strerror(errno));
         return 1;
      }
   } else {
      snprintf(retain_dir, sizeof(retain_dir), "%s", evidence_dir);
   }
   static const char *const runner_retention_names[] = {
      "recorded_ib.bin", "references.bin",
   };
   if (r3v_native_evidence_write_file(retain_dir, runner_retention_names[0],
                                      native->ib,
                                      native->ib_size_dwords * 4u) != 0 ||
       r3v_native_evidence_write_file(
          retain_dir, runner_retention_names[1], native->references,
          native->reference_count *
             (uint32_t)sizeof(native->references[0])) != 0) {
      fprintf(stderr, "stream retention failed\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 1;
   }
   static const char *const queue_retention_names[] = {
      "ib.bin", "relocs.bin", "manifest.json",
   };
   for (unsigned n = 0; n < ARRAY_SIZE(queue_retention_names); n++) {
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", retain_dir,
               queue_retention_names[n]);
      if (access(path, F_OK) == 0) {
         fprintf(stderr,
                 "%s already exists in the retention directory; the queue "
                 "publishes it and refuses ahead of the ioctl\n",
                 queue_retention_names[n]);
         if (!record_only)
            r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
         return 2;
      }
   }
   emit("[retain] %s %s under %s; %s %s %s fresh\n",
        runner_retention_names[0], runner_retention_names[1],
        record_only ? "record scratch" : "the evidence directory",
        queue_retention_names[0], queue_retention_names[1],
        queue_retention_names[2]);
   if (record_only) {
      for (unsigned n = 0; n < ARRAY_SIZE(runner_retention_names); n++) {
         char path[PATH_MAX];
         snprintf(path, sizeof(path), "%s/%s", retain_dir,
                  runner_retention_names[n]);
         unlink(path);
      }
      rmdir(retain_dir);
   }

   if (record_only) {
      emit("record: ACCEPTED\n");
      for (unsigned p = 0; p < 3; p++)
         free(expected[p]);
      r300_tcl_bypass_triangle_release(&armed);
      r300_tcl_bypass_triangle_release(&armed_control);
      vkDestroyCommandPool(device, pool, NULL);
      for (unsigned i = 0; i < 2; i++) {
         vkDestroyPipeline(device, pipeline[i], NULL);
         vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
         vkDestroyImageView(device, target[i].view, NULL);
         vkDestroyImage(device, target[i].image, NULL);
         vkFreeMemory(device, target[i].memory, NULL);
      }
      vkDestroyPipelineLayout(device, layout, NULL);
      vkDestroyRenderPass(device, pass, NULL);
      vkDestroyBuffer(device, vertex_buffer, NULL);
      vkFreeMemory(device, vertex_memory, NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: the two load-op clears realize on the host, the two
    * carriers fill, and one live DRM_RADEON_CS reaches the command
    * processor here, with the bounded completion wait after it. */
   stage("submit");
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   emit("[submit] vkQueueSubmit returned %d\n", result);

   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;
   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *control_map = NULL;
   void *candidate_map = NULL;
   CHECK(vkMapMemory(device, target[0].memory, 0, VK_WHOLE_SIZE, 0,
                     &control_map));
   CHECK(vkMapMemory(device, target[1].memory, 0, VK_WHOLE_SIZE, 0,
                     &candidate_map));
   if (r3v_native_evidence_write_file(evidence_dir, "control_target.bin",
                                      control_map, color_bytes) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "candidate_target.bin",
                                      candidate_map, color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   /* The carrier witness: each pass's carrier still holds the probe's
    * TEX0 payload and three reciprocal-W lanes proportional to the
    * probe's, so the models the census evaluates read the values the
    * device fetched.  The carrier is host memory the device only
    * reads, so the bytes are the host's own. */
   bool carrier_witness = true;
   {
      VK_FROM_HANDLE(r3v_native_device, native_device, device);
      static const char *const carrier_file[2] = { "control_carrier.bin",
                                                   "candidate_carrier.bin" };
      for (unsigned p = 0; p < 2; p++) {
         struct r3v_native_memory *carrier = native->owned_carriers[p];
         void *carrier_map = NULL;
         if (carrier == NULL ||
             radeon_drm_vk_bo_map(&native_device->drm, &carrier->bo,
                                  &carrier_map) != 0) {
            emit("[witness] pass %u carrier unreadable\n", p);
            carrier_witness = false;
            continue;
         }
         const float *carrier_records = carrier_map;
         /* The carrier pass publishes the TC1 shape: twelve dwords per
          * record, the payload premultiplied by the carrier lane, so
          * the witness judges payload / carrier against the probe's
          * TEX0 and the carrier lanes as the normalized clip w
          * (r300_noperspective_reciprocal_plan.h). */
         const bool carrier_pass = carrier_route && p == 1;
         /* The q-lane pass keeps the eight-dword record: xyz the
          * probe's s, t, r premultiplied by c and w the normalized
          * clip w (r300_noperspective_q_lane_plan.h). */
         const bool q_lane_pass = q_lane_route && p == 1;
         /* The mixed pass publishes sixteen dwords: TC0 the probe's
          * TEX0 verbatim, TC1 TEX0 premultiplied by c, TC2 (c, 0, 0,
          * 1) (r300_noperspective_mixed_carrier_plan.h). */
         const bool mixed_pass = mixed_route && p == 1;
         const unsigned stride =
            mixed_pass    ? R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS
            : carrier_pass ? R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS
                           : 8u;
         const unsigned fan_records =
            partial ? R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u
                    : 3u;
         if (r3v_native_evidence_write_file(evidence_dir, carrier_file[p],
                                            carrier_map,
                                            fan_records * stride * 4u) != 0) {
            fprintf(stderr, "carrier retention failed\n");
            return 1;
         }
         if (partial) {
            /* The clipper's fan: the quad's two triangles are six live
             * records, the rest padding.  A live record's window
             * position lies inside the source triangle; the control
             * pass carries the perspective-correct source value there
             * (the clipper blends in clip space), and the carrier pass
             * carries payload / carrier equal to the window-linear
             * source value, the Vulkan clipped NoPerspective value. */
            unsigned live = 0, exact = 0;
            bool proportional = true;
            double scale_product = 0.0;
            for (unsigned r = 0; r < fan_records; r++) {
               const float *rec = &carrier_records[r * stride];
               bool padding = rec[3] == 1.0f;
               for (unsigned k = 0; k < stride && padding; k++)
                  padding = k == 3 || rec[k] == 0.0f;
               if (padding)
                  continue;
               live++;
               float model[4];
               const bool inside = r300_rs_tex_adj_probe_model_value(
                  records,
                  carrier_pass ? R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE
                               : R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE,
                  0, rec[0], rec[1], model);
               bool match = inside;
               const float c = carrier_pass ? rec[8] : 1.0f;
               for (unsigned k = 0; k < 4 && match; k++)
                  match = fabsf(rec[4 + k] / c - model[k]) <= 1e-4f;
               if (carrier_pass) {
                  match &= c > 0.0f && c <= 1.0f && rec[9] == 0.0f &&
                           rec[10] == 0.0f && rec[11] == 1.0f;
                  /* c = s * w and the position lane k / w: their
                   * product is one constant over the fan. */
                  const double product = (double)c * rec[3];
                  if (scale_product == 0.0)
                     scale_product = product;
                  proportional &= fabs(product - scale_product) <=
                                  1e-5 * scale_product;
               }
               exact += match;
            }
            emit("[witness] pass %u stride=%u fan live=%u exact=%u "
                 "proportional=%d\n",
                 p, stride, live, exact, (int)proportional);
            carrier_witness &= live == 6 && exact == live && proportional;
            radeon_drm_vk_bo_unmap(&native_device->drm, &carrier->bo,
                                   carrier_map);
            continue;
         }
         bool payload_exact = true;
         for (unsigned v = 0; v < 3; v++) {
            if (mixed_pass) {
               const float c = carrier_records[v * stride + 12];
               const float w = 1.0f / r300_rs_tex_adj_probe_reciprocal_w[v];
               float w_max = 0.0f;
               for (unsigned b = 0; b < 3; b++)
                  w_max = fmaxf(w_max,
                                1.0f / r300_rs_tex_adj_probe_reciprocal_w[b]);
               payload_exact &= fabsf(c - w / w_max) <= 1e-6f && c > 0.0f &&
                                c <= 1.0f &&
                                carrier_records[v * stride + 13] == 0.0f &&
                                carrier_records[v * stride + 14] == 0.0f &&
                                carrier_records[v * stride + 15] == 1.0f;
               /* TC0: the vertex's own TEX0, or under the Flat
                * location the provoking vertex's on every record. */
               payload_exact &= memcmp(&carrier_records[v * stride + 4],
                                       &r300_rs_tex_adj_probe_tex0[
                                          flat_mixed_route ? 0 : v * 4],
                                       4 * sizeof(float)) == 0;
               for (unsigned k = 0; k < 4; k++) {
                  const float expected =
                     r300_rs_tex_adj_probe_tex0[v * 4 + k] * c;
                  payload_exact &=
                     fabsf(carrier_records[v * stride + 8 + k] - expected) <=
                     1e-6f * fmaxf(1.0f, fabsf(expected));
               }
               continue;
            }
            if (q_lane_pass) {
               const float c = carrier_records[v * stride + 7];
               const float w = 1.0f / r300_rs_tex_adj_probe_reciprocal_w[v];
               float w_max = 0.0f;
               for (unsigned b = 0; b < 3; b++)
                  w_max = fmaxf(w_max,
                                1.0f / r300_rs_tex_adj_probe_reciprocal_w[b]);
               payload_exact &= fabsf(c - w / w_max) <= 1e-6f && c > 0.0f &&
                                c <= 1.0f;
               for (unsigned k = 0; k < 3; k++) {
                  const float expected =
                     r300_rs_tex_adj_probe_tex0[v * 4 + k] * c;
                  payload_exact &=
                     fabsf(carrier_records[v * stride + 4 + k] - expected) <=
                     1e-6f * fmaxf(1.0f, fabsf(expected));
               }
               continue;
            }
            if (!carrier_pass) {
               payload_exact &= memcmp(&carrier_records[v * stride + 4],
                                       &r300_rs_tex_adj_probe_tex0[v * 4],
                                       4 * sizeof(float)) == 0;
               continue;
            }
            const float lane = carrier_records[v * stride + 8];
            const float w = 1.0f / r300_rs_tex_adj_probe_reciprocal_w[v];
            float w_max = 0.0f;
            for (unsigned b = 0; b < 3; b++)
               w_max = fmaxf(w_max,
                             1.0f / r300_rs_tex_adj_probe_reciprocal_w[b]);
            payload_exact &= fabsf(lane - w / w_max) <= 1e-6f &&
                             carrier_records[v * stride + 9] == 0.0f &&
                             carrier_records[v * stride + 10] == 0.0f &&
                             carrier_records[v * stride + 11] == 1.0f;
            for (unsigned c = 0; c < 4; c++) {
               const float expected =
                  r300_rs_tex_adj_probe_tex0[v * 4 + c] * lane;
               payload_exact &=
                  fabsf(carrier_records[v * stride + 4 + c] - expected) <=
                  1e-6f * fmaxf(1.0f, fabsf(expected));
            }
         }
         /* The projection scales every reciprocal W by one factor, so
          * the three ratios against the probe's lanes agree and the
          * lanes stay pairwise distinct. */
         const float ratio = carrier_records[3] /
                             r300_rs_tex_adj_probe_reciprocal_w[0];
         bool proportional = isfinite(ratio) && ratio != 0.0f;
         bool distinct = true;
         for (unsigned v = 0; v < 3; v++) {
            const float expected_w =
               ratio * r300_rs_tex_adj_probe_reciprocal_w[v];
            proportional &=
               fabsf(carrier_records[v * stride + 3] - expected_w) <=
               1e-5f * fabsf(expected_w);
            for (unsigned b = v + 1; b < 3; b++)
               distinct &= carrier_records[v * stride + 3] !=
                           carrier_records[b * stride + 3];
         }
         emit("[witness] pass %u stride=%u payload_exact=%d "
              "reciprocal_w=(%g, %g, %g) ratio=%g proportional=%d "
              "distinct=%d\n",
              p, stride, (int)payload_exact, carrier_records[3],
              carrier_records[stride + 3], carrier_records[2 * stride + 3],
              ratio,
              (int)proportional, (int)distinct);
         carrier_witness &= payload_exact && proportional && distinct;
         radeon_drm_vk_bo_unmap(&native_device->drm, &carrier->bo,
                                carrier_map);
      }
   }

   /* The control census carries the premise and the candidate census
    * the result: the control reads its own image against the models
    * alone, and the candidate reads its image with the control
    * supplied, so an unchanged target is separable from a perspective
    * one. */
   struct r300_rs_tex_adj_probe_census control_census, candidate_census;
   if (r300_rs_tex_adj_probe_census(&mp.pass[0], records, control_map, NULL,
                                    color_bytes, &control_census) != 0 ||
       r300_rs_tex_adj_probe_census(&mp.pass[1], candidate_records,
                                    candidate_map, control_map, color_bytes,
                                    &candidate_census) != 0) {
      fprintf(stderr, "the census refused a target\n");
      return 1;
   }
   report_census("control", &control_census);
   report_census("candidate", &candidate_census);
   /* The q-lane oracle beyond the classification: each of the three
    * payload channels separates the models on its own over some judged
    * pixel, affine lands within one quantum on every judged pixel
    * (max_deviation <= 1 with match == judged), perspective and the
    * control match no pixel, and the observed alpha byte is exactly 255
    * on every judged pixel. */
   bool q_lane_oracle = true;
   if (q_lane_route) {
      struct r300_rs_tex_adj_probe_channel_census channels;
      if (r300_rs_tex_adj_probe_channel_census(&mp.pass[1], candidate_records,
                                               candidate_map, color_bytes,
                                               &channels) != 0) {
         fprintf(stderr, "the channel census refused the candidate\n");
         return 1;
      }
      const bool separated = channels.separated[0] != 0 &&
                             channels.separated[1] != 0 &&
                             channels.separated[2] != 0;
      const bool affine_exact =
         candidate_census.judged != 0 &&
         candidate_census.match[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] ==
            candidate_census.judged &&
         candidate_census.max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] <=
            1u;
      const bool perspective_zero =
         candidate_census.match[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] == 0 &&
         candidate_census.unchanged == 0;
      const bool alpha_one = channels.alpha_one == channels.judged;
      q_lane_oracle = separated && affine_exact && perspective_zero &&
                      alpha_one;
      emit("[q-lane] judged=%u separated=(%u, %u, %u, %u) alpha_one=%u "
           "affine_exact=%d perspective_zero=%d oracle=%d\n",
           channels.judged, channels.separated[0], channels.separated[1],
           channels.separated[2], channels.separated[3], channels.alpha_one,
           (int)affine_exact, (int)perspective_zero, (int)q_lane_oracle);
   }

   /* The mixed oracle: red and green (the Smooth s, t) match
    * perspective within one quantum on every judged pixel with affine
    * matching none of their separated pixels; blue and alpha (the
    * NoPerspective s, t) match affine within one quantum on every
    * judged pixel with perspective matching none of their separated
    * pixels; each of the four channels separates the models on some
    * judged pixel; no judged pixel equals the control or the sentinel. */
   bool mixed_oracle = true;
   if (mixed_route) {
      struct r300_rs_tex_adj_probe_channel_census ch;
      bool clauses[6] = { false };
      mixed_oracle = judge_mixed(&mp.pass[1], candidate_records,
                                 candidate_map, color_bytes,
                                 candidate_census.unchanged,
                                 flat_mixed_route, &ch, clauses);
      emit("[%s] judged=%u separated=(%u, %u, %u, %u) "
           "perspective_match=(%u, %u, %u, %u) "
           "affine_match=(%u, %u, %u, %u) "
           "perspective_max_dev=(%u, %u, %u, %u) "
           "affine_max_dev=(%u, %u, %u, %u) "
           "affine_on_separated=(%u, %u, %u, %u) "
           "perspective_on_separated=(%u, %u, %u, %u) unchanged=%u "
           "sentinel=%u separated_all=%d smooth_exact=%d "
           "smooth_affine_zero=%d noperspective_exact=%d "
           "noperspective_perspective_zero=%d unchanged_zero=%d "
           "oracle=%d\n",
           flat_mixed_route ? "flat-mixed" : "mixed",
           ch.judged, ch.separated[0], ch.separated[1], ch.separated[2],
           ch.separated[3], ch.perspective_match[0], ch.perspective_match[1],
           ch.perspective_match[2], ch.perspective_match[3],
           ch.affine_match[0], ch.affine_match[1], ch.affine_match[2],
           ch.affine_match[3], ch.perspective_max_deviation[0],
           ch.perspective_max_deviation[1], ch.perspective_max_deviation[2],
           ch.perspective_max_deviation[3], ch.affine_max_deviation[0],
           ch.affine_max_deviation[1], ch.affine_max_deviation[2],
           ch.affine_max_deviation[3], ch.affine_on_separated[0],
           ch.affine_on_separated[1], ch.affine_on_separated[2],
           ch.affine_on_separated[3], ch.perspective_on_separated[0],
           ch.perspective_on_separated[1], ch.perspective_on_separated[2],
           ch.perspective_on_separated[3], candidate_census.unchanged,
           ch.sentinel, (int)clauses[0], (int)clauses[1], (int)clauses[2],
           (int)clauses[3], (int)clauses[4], (int)clauses[5],
           (int)mixed_oracle);
      /* The retained candidate image against the mixed prediction:
       * the dword-exact count is the receipt's tightest number. */
      uint32_t exact = 0, judged_pixels = 0;
      for (uint32_t y = 0; y < mp.pass[1].height; y++)
         for (uint32_t x = 0; x < mp.pass[1].width; x++) {
            const uint32_t i = mp.pass[1].target_offset / 4u +
                               y * mp.pass[1].pitch_pixels + x;
            if (expected_mixed[i] == R300_TRIANGLE_COLOR_SENTINEL)
               continue;
            judged_pixels++;
            exact += ((const uint32_t *)candidate_map)[i] ==
                     expected_mixed[i];
         }
      emit("[mixed] dword-exact against expected_mixed.bin: %u of %u "
           "predicted interior pixels\n", exact, judged_pixels);
   }
   free(expected_mixed);

   const enum r300_rs_tex_adj_probe_classification control_class =
      r300_rs_tex_adj_probe_classify(&control_census);
   const enum r300_rs_tex_adj_probe_classification candidate_class =
      r300_rs_tex_adj_probe_classify(&candidate_census);
   const bool premise =
      control_class == R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE &&
      control_census.judged != 0;
   emit("[oracle] control premise %s: the smooth interface's target "
        "classifies %s over %u judged pixels\n",
        premise ? "holds" : "fails",
        r300_rs_tex_adj_probe_classification_name(control_class),
        control_census.judged);
   emit("[oracle] candidate judged=%u unchanged=%u carrier_witness=%d\n",
        candidate_census.judged, candidate_census.unchanged,
        (int)carrier_witness);
   emit("[classification] %s=%s\n", candidate_word_name,
        r300_rs_tex_adj_probe_classification_name(candidate_class));

   const bool judged = candidate_census.judged != 0;
   for (unsigned p = 0; p < 3; p++)
      free(expected[p]);
   r300_tcl_bypass_triangle_release(&armed);
   r300_tcl_bypass_triangle_release(&armed_control);

   stage("teardown");
   vkUnmapMemory(device, target[0].memory);
   vkUnmapMemory(device, target[1].memory);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 2; i++) {
      vkDestroyPipeline(device, pipeline[i], NULL);
      vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
      vkDestroyImageView(device, target[i].view, NULL);
      vkDestroyImage(device, target[i].image, NULL);
      vkFreeMemory(device, target[i].memory, NULL);
   }
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   emit("[verdict] %s\n",
        !premise ? "the control target does not classify perspective; the "
                   "premise fails and the run is a finding about the "
                   "control cell, not about the candidate word"
        : !judged ? "the candidate census judged no pixel; the run carries "
                    "no claim about the candidate word"
        : !carrier_witness
           ? "the carriers do not hold the probe payload at proportional "
             "reciprocal W; the models were evaluated against records the "
             "device did not fetch"
        : flat_mixed_route
           ? (mixed_oracle
                 ? "the control cell interpolates perspective-correct and "
                   "the Flat-beside-NoPerspective pipeline's target carries "
                   "the provoking vertex's s, t as one constant in red and "
                   "green beside the NoPerspective s, t affine in blue and "
                   "alpha, each within one quantum on every judged pixel "
                   "under rung D's cell with no gate: the receipt of Flat "
                   "through host replication beside the mixed reciprocal "
                   "carrier on RS482"
                 : "the flat-mixed oracle does not hold; the [flat-mixed] "
                   "line names the failed clause")
        : mixed_route
           ? (mixed_oracle
                 ? "the control cell interpolates perspective-correct and "
                   "the mixed pipeline's target carries the Smooth s, t "
                   "perspective-correct in red and green beside the "
                   "NoPerspective s, t affine in blue and alpha, each "
                   "within one quantum on every judged pixel with the "
                   "competing model matching no separated pixel: the "
                   "receipt of the mixed reciprocal carrier -- TC0 "
                   "Smooth, TC1 premultiplied, TC2 carrier, three RS "
                   "vectors at VAP_VTX_SIZE 16 -- on RS482"
                 : "the mixed oracle does not hold; the [mixed] line "
                   "names the failed clause")
        : q_lane_route
           ? (q_lane_oracle
                 ? "the control cell interpolates perspective-correct and "
                   "the vec3 NoPerspective pipeline's target classifies "
                   "affine within one quantum on every judged pixel in "
                   "each of three separated channels with alpha exactly "
                   "1: the receipt of the q-lane carrier -- the varying "
                   "cell's words, xyz * rcp(w) US program -- on RS482"
                 : "the q-lane oracle does not hold; the classification "
                   "printed above stands and the [q-lane] line names the "
                   "failed clause")
        : public_partial
           ? "the control cell interpolates perspective-correct over the "
             "clipped fan and the public NoPerspective pipeline's target "
             "carries the classification printed above under the TC1 "
             "carrier cell the selector chose at submission with no gate; "
             "affine is the Vulkan NoPerspective value, so that "
             "classification is the receipt of the public partial-clip "
             "fallback on RS482"
        : production
           ? "the control cell interpolates perspective-correct and the "
             "public NoPerspective pipeline's target carries the "
             "classification printed above; affine is the Vulkan "
             "NoPerspective value, so that classification is the receipt "
             "of the direct GB W_SELECT route on RS482"
        : partial
           ? "the control cell interpolates perspective-correct over the "
             "clipped fan and the forced reciprocal-carrier pipeline's "
             "target carries the classification printed above; affine is "
             "the Vulkan NoPerspective value, so that classification is "
             "the receipt of the carrier through the clipper -- packed "
             "ahead of the cut, generated vertices at the clipped-edge "
             "value -- on RS482"
        : carrier_route
           ? "the control cell interpolates perspective-correct and the "
             "forced reciprocal-carrier pipeline's target carries the "
             "classification printed above; affine is the Vulkan "
             "NoPerspective value, so that classification is the receipt "
             "of the TC1 carrier cell -- widened record, second "
             "interpolator, RCP+MUL US program -- on RS482"
           : "the control cell interpolates perspective-correct and the "
             "candidate word's target carries the classification printed "
             "above; the statement is what that one bit does on RS482");
   if (run_log != NULL)
      fclose(run_log);
   /* The classification is the result, so the exit status names the
    * premise and the judged footprint alone: the control cell
    * interpolates perspective-correct and the candidate census judged
    * pixels.  A carrier the witness refuses prints above and rides the
    * verdict line, where its caveat belongs. */
   return (premise && judged && q_lane_oracle && mixed_oracle) ? 0 : 1;
}
