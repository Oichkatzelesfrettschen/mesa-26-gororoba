/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the rasterizer interpolation probe: the control plan
 * reproduces the varying cell's bytes, each candidate differs from
 * them in one dword, every mutation refuses validation and localizes
 * to its predicted register or stream position, the per-draw stream
 * check separates a pass from its predecessor, the registered models
 * separate over the judged interior, and the census names each model
 * from the image that model predicts and nothing else.
 */

#include "r300_rs_tex_adj_probe.h"
#include "r300_first_draw_state.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
   do {                                                                    \
      if (!(cond)) {                                                       \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                 #cond);                                                   \
         failures++;                                                       \
      }                                                                    \
   } while (0)

/* The contract's GB_SELECT word (r300_first_draw_state.c). */
#define GB_SELECT_BASE 0x00000000u

static uint32_t
contract_word(uint32_t reg)
{
   struct r300_first_draw_contract contract;
   if (r300_tcl_bypass_triangle_reference_contract(&contract) != 0)
      return 0xffffffffu;
   for (uint32_t i = 0; i < contract.count; i++)
      if (contract.entries[i].reg == reg)
         return contract.entries[i].value;
   return 0xffffffffu;
}

/* Number of differing dwords between two streams, and the first
 * differing index. */
static uint32_t
stream_delta(const struct r300_tcl_bypass_triangle_ib *a,
             const struct r300_tcl_bypass_triangle_ib *b, uint32_t *first)
{
   if (a->ib_size_dwords != b->ib_size_dwords) {
      *first = 0;
      return UINT32_MAX;
   }
   uint32_t n = 0;
   *first = UINT32_MAX;
   for (uint32_t i = 0; i < a->ib_size_dwords; i++) {
      if (a->ib[i] != b->ib[i]) {
         if (*first == UINT32_MAX)
            *first = i;
         n++;
      }
   }
   return n;
}

static void
test_control_is_the_varying_cell(void)
{
   struct r300_rs_tex_adj_probe_plan control;
   r300_rs_tex_adj_probe_plan_control(&control);
   CHECK(r300_rs_tex_adj_probe_plan_validate(&control) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_rs_count(&control) ==
         (R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN));
   CHECK(r300_rs_tex_adj_probe_plan_rs_inst(&control, 0) ==
         (R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(0)));
   CHECK(!r300_rs_tex_adj_probe_plan_writes_rs_inst_1(&control));
   CHECK(r300_rs_tex_adj_probe_plan_gb_select(&control, GB_SELECT_BASE) ==
         GB_SELECT_BASE);
   CHECK(contract_word(R300_GB_SELECT) == GB_SELECT_BASE);

   struct r300_tcl_bypass_triangle_ib legacy, probe;
   CHECK(r300_tcl_bypass_triangle_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &legacy) == 0);
   CHECK(r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &control, &probe) == 0);
   uint32_t first;
   CHECK(stream_delta(&legacy, &probe, &first) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &control, GB_SELECT_BASE, legacy.ib, legacy.ib_size_dwords) == 1);
   r300_tcl_bypass_triangle_release(&legacy);
   r300_tcl_bypass_triangle_release(&probe);
}

/* Each candidate differs from the control stream in exactly one dword,
 * the register word the candidate names, and the control check refuses
 * the candidate stream while the candidate check admits it. */
static void
test_candidates_differ_in_one_word(void)
{
   struct r300_rs_tex_adj_probe_plan control, tex_adj, w_select;
   r300_rs_tex_adj_probe_plan_control(&control);
   r300_rs_tex_adj_probe_plan_tex_adj(&tex_adj);
   r300_rs_tex_adj_probe_plan_w_select_one(&w_select);
   CHECK(r300_rs_tex_adj_probe_plan_validate(&tex_adj) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_validate(&w_select) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_rs_inst(&tex_adj, 0) ==
         (r300_rs_tex_adj_probe_plan_rs_inst(&control, 0) |
          R300_RS_INST_TEX_ADJ));
   CHECK(r300_rs_tex_adj_probe_plan_gb_select(&w_select, GB_SELECT_BASE) ==
         (GB_SELECT_BASE | R300_GB_W_SELECT_1));

   struct r300_tcl_bypass_triangle_ib base, cand;
   CHECK(r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &control, &base) == 0);
   const struct {
      const struct r300_rs_tex_adj_probe_plan *plan;
      uint32_t reg;
   } candidates[2] = {
      { &tex_adj, R300_RS_INST_0 },
      { &w_select, R300_GB_SELECT },
   };
   for (unsigned c = 0; c < 2; c++) {
      CHECK(r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
               R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
               false, 1u, candidates[c].plan, &cand) == 0);
      uint32_t first;
      CHECK(stream_delta(&base, &cand, &first) == 1);
      /* The differing dword is the payload of a type-0 packet naming
       * the candidate's register: the header sits one dword before
       * a single-register write, or further back inside a run. */
      bool named = false;
      for (uint32_t back = 1; back <= 16 && back <= first; back++) {
         const uint32_t header = cand.ib[first - back];
         if ((header >> 30) != 0)
            continue;
         const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
         if (back > count)
            continue;
         const uint32_t reg = (header & 0x3FFF) * 4 +
                              ((header & RADEON_ONE_REG_WR) ? 0 : 4 * (back - 1));
         if (reg == candidates[c].reg) {
            named = true;
            break;
         }
      }
      CHECK(named);
      CHECK(r300_rs_tex_adj_probe_plan_stream_check(
               candidates[c].plan, GB_SELECT_BASE, cand.ib,
               cand.ib_size_dwords) == 1);
      CHECK(r300_rs_tex_adj_probe_plan_stream_check(
               &control, GB_SELECT_BASE, cand.ib, cand.ib_size_dwords) == -1);
      CHECK(r300_rs_tex_adj_probe_plan_stream_check(
               candidates[c].plan, GB_SELECT_BASE, base.ib,
               base.ib_size_dwords) == -1);
      r300_tcl_bypass_triangle_release(&cand);
   }
   r300_tcl_bypass_triangle_release(&base);
}

/* Bit set on the wrong RS instruction and RS source away from TEX0:
 * each refuses validation, the plan form still emits, and the
 * candidate check refuses the mutated stream at its draw. */
static void
test_plan_mutations_refuse_and_localize(void)
{
   struct r300_rs_tex_adj_probe_plan tex_adj, wrong_inst, wrong_source;
   r300_rs_tex_adj_probe_plan_tex_adj(&tex_adj);
   wrong_inst = tex_adj;
   wrong_inst.rs_instruction = 1;
   wrong_source = tex_adj;
   wrong_source.rs_source = R300_RS_TEX_ADJ_PROBE_SOURCE_COLOR0;
   CHECK(r300_rs_tex_adj_probe_plan_validate(&wrong_inst) == -EINVAL);
   CHECK(r300_rs_tex_adj_probe_plan_validate(&wrong_source) == -EINVAL);
   CHECK(r300_rs_tex_adj_probe_plan_validate(NULL) == -EINVAL);

   /* Wrong instruction: RS_INST_0 loses the bit and RS_INST_1 gains
    * it, an instruction RS_INST_COUNT 0 never runs. */
   CHECK(r300_rs_tex_adj_probe_plan_writes_rs_inst_1(&wrong_inst));
   CHECK((r300_rs_tex_adj_probe_plan_rs_inst(&wrong_inst, 0) &
          R300_RS_INST_TEX_ADJ) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_rs_inst(&wrong_inst, 1) ==
         R300_RS_INST_TEX_ADJ);
   struct r300_tcl_bypass_triangle_ib ib;
   CHECK(r300_tcl_bypass_triangle_rs_tex_adj_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &wrong_inst, &ib) == -EINVAL);
   CHECK(r300_tcl_bypass_triangle_rs_tex_adj_plan_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &wrong_inst, &ib) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &tex_adj, GB_SELECT_BASE, ib.ib, ib.ib_size_dwords) == -1);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &wrong_inst, GB_SELECT_BASE, ib.ib, ib.ib_size_dwords) == 1);
   r300_tcl_bypass_triangle_release(&ib);

   /* Wrong source: RS_IP_0 reads color pointer 0 and RS_INST_0 writes
    * through COL_CN_WRITE while the carrier lands in TEX0. */
   CHECK(r300_rs_tex_adj_probe_plan_rs_ip_0(&wrong_source) ==
         (R300_RS_COL_PTR(0) | R300_RS_COL_FMT(R300_RS_COL_FMT_RGBA)));
   CHECK((r300_rs_tex_adj_probe_plan_rs_inst(&wrong_source, 0) &
          R300_RS_INST_COL_CN_WRITE) != 0);
   CHECK(r300_tcl_bypass_triangle_rs_tex_adj_plan_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &wrong_source, &ib) == 0);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &tex_adj, GB_SELECT_BASE, ib.ib, ib.ib_size_dwords) == -1);
   r300_tcl_bypass_triangle_release(&ib);
}

/* Late write and pass inheritance, realized on the two-pass stream:
 * moving the candidate RS_INST_0 word behind its draw fails that draw,
 * a candidate pass that inherits the control's RS_INST_0 fails at its
 * own draw, and a control pass that inherits the candidate's fails at
 * its own. */
static void
test_stream_mutations(void)
{
   struct r300_rs_tex_adj_probe_plan control, tex_adj;
   r300_rs_tex_adj_probe_plan_control(&control);
   r300_rs_tex_adj_probe_plan_tex_adj(&tex_adj);
   struct r300_triangle_multi_pass mp;
   memset(&mp, 0, sizeof(mp));
   r300_tcl_bypass_triangle_render_shape_reference(&mp.pass[0]);
   r300_tcl_bypass_triangle_render_shape_reference(&mp.pass[1]);
   mp.pass[0].varying = true;
   mp.pass[1].varying = true;
   mp.pass[1].rs_tex_adj_candidate = R300_RS_TEX_ADJ_PROBE_TEX_ADJ;
   mp.second_vertex_index = 2;
   mp.second_color_index = 3;
   struct r300_tcl_bypass_triangle_ib two;
   CHECK(r300_tcl_bypass_triangle_multi_pass_emit(&mp, &two) == 0);

   /* Locate each pass's effective RS_INST_0 payload, the last write
    * ahead of its draw (the contract prefix writes the register once
    * more, earlier, at its position-only value), and the two draw
    * headers. */
   uint32_t inst_index[2] = { UINT32_MAX, UINT32_MAX };
   uint32_t draw_index[2] = { UINT32_MAX, UINT32_MAX };
   uint32_t draw_end[2] = { UINT32_MAX, UINT32_MAX };
   uint32_t last_inst = UINT32_MAX;
   unsigned draws = 0;
   uint32_t i = 0;
   while (i < two.ib_size_dwords) {
      const uint32_t header = two.ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (kind == 0) {
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            if (base + (one_reg ? 0 : 4 * k) == R300_RS_INST_0)
               last_inst = i + 1 + k;
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (r300_first_draw_is_draw_packet(header) && draws < 2) {
            inst_index[draws] = last_inst;
            draw_end[draws] = i + 1 + count;
            draw_index[draws++] = i;
         }
         i += 1 + count;
      } else {
         i += 1;
      }
   }
   CHECK(draws == 2 && inst_index[0] != UINT32_MAX &&
         inst_index[1] != UINT32_MAX);
   CHECK(inst_index[0] < draw_index[0] && draw_index[0] < inst_index[1] &&
         inst_index[1] < draw_index[1]);
   const uint32_t control_word = r300_rs_tex_adj_probe_plan_rs_inst(&control, 0);
   const uint32_t candidate_word = r300_rs_tex_adj_probe_plan_rs_inst(&tex_adj, 0);
   CHECK(two.ib[inst_index[0]] == control_word);
   CHECK(two.ib[inst_index[1]] == candidate_word);

   /* The intended pair: the control check admits pass 0 alone and the
    * candidate check pass 1 alone; a check over the whole stream
    * therefore names the other pass's draw. */
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &control, GB_SELECT_BASE, two.ib, draw_index[1]) == 1);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &control, GB_SELECT_BASE, two.ib, two.ib_size_dwords) == -2);
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &tex_adj, GB_SELECT_BASE, two.ib, two.ib_size_dwords) == -1);

   /* Candidate pass inheriting the control state: pass 1's RS_INST_0
    * carries the control word. */
   uint32_t *ib = malloc(two.ib_size_dwords * sizeof(uint32_t));
   CHECK(ib != NULL);
   if (ib == NULL) {
      r300_tcl_bypass_triangle_release(&two);
      return;
   }
   memcpy(ib, two.ib, two.ib_size_dwords * sizeof(uint32_t));
   ib[inst_index[1]] = control_word;
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &tex_adj, GB_SELECT_BASE, ib + draw_end[0],
            two.ib_size_dwords - draw_end[0]) == -1);

   /* Control pass inheriting the candidate state: pass 0's RS_INST_0
    * carries the candidate word. */
   memcpy(ib, two.ib, two.ib_size_dwords * sizeof(uint32_t));
   ib[inst_index[0]] = candidate_word;
   CHECK(r300_rs_tex_adj_probe_plan_stream_check(
            &control, GB_SELECT_BASE, ib, draw_index[1]) == -1);

   /* Bit written after the draw: pass 1's RS_INST_0 write moves behind
    * its draw packet by swapping the register write into the packet
    * that follows the draw.  Realized by leaving the pre-draw word at
    * the control value and appending a one-register write after the
    * stream's end. */
   uint32_t *late = malloc((two.ib_size_dwords + 2u) * sizeof(uint32_t));
   CHECK(late != NULL);
   if (late != NULL) {
      memcpy(late, two.ib, two.ib_size_dwords * sizeof(uint32_t));
      late[inst_index[1]] = control_word;
      late[two.ib_size_dwords] = (0u << 30) | (R300_RS_INST_0 >> 2);
      late[two.ib_size_dwords + 1] = candidate_word;
      CHECK(r300_rs_tex_adj_probe_plan_stream_check(
               &tex_adj, GB_SELECT_BASE, late + draw_end[0],
               two.ib_size_dwords + 2u - draw_end[0]) == -1);
      free(late);
   }
   free(ib);
   r300_tcl_bypass_triangle_release(&two);
}

/* Carrier mutations that collapse the discriminator: equal reciprocal
 * W leaves no judged pixel (perspective equals affine everywhere), and
 * constant q makes the projective model coincide with perspective so
 * the census cannot name projective-q alone. */
static void
test_carrier_and_oracle(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.varying = true;
   float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   r300_rs_tex_adj_probe_vertices(&shape, records);
   float clip[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   r300_rs_tex_adj_probe_clip_vertices(clip);
   /* The clip form projects to the window form: x / w over the
    * viewport equals the window x, and 1 / w the reciprocal lane. */
   for (unsigned v = 0; v < 3; v++) {
      const float w = clip[v * 8 + 3];
      const float x = (clip[v * 8 + 0] / w + 1.0f) * (float)shape.width / 2.0f;
      const float y = (clip[v * 8 + 1] / w + 1.0f) * (float)shape.height / 2.0f;
      CHECK(x > records[v * 8 + 0] - 1e-3f && x < records[v * 8 + 0] + 1e-3f);
      CHECK(y > records[v * 8 + 1] - 1e-3f && y < records[v * 8 + 1] + 1e-3f);
      CHECK(1.0f / w == records[v * 8 + 3]);
      for (unsigned c = 0; c < 4; c++)
         CHECK(clip[v * 8 + 4 + c] == records[v * 8 + 4 + c]);
   }
   /* s, t, r <= q at every vertex, q distinct, W distinct. */
   for (unsigned v = 0; v < 3; v++)
      for (unsigned c = 0; c < 3; c++)
         CHECK(records[v * 8 + 4 + c] <= records[v * 8 + 7]);
   CHECK(records[7] != records[15] && records[15] != records[23] &&
         records[7] != records[23]);
   CHECK(records[3] != records[11] && records[11] != records[19]);

   const uint32_t size = r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   uint32_t *image = malloc(size);
   uint32_t *control = malloc(size);
   CHECK(image != NULL && control != NULL);
   if (image == NULL || control == NULL) {
      free(image);
      free(control);
      return;
   }
   struct r300_rs_tex_adj_probe_census census;

   /* Each model's own image classifies as that model alone. */
   static const enum r300_rs_tex_adj_probe_classification class_of_model[] = {
      R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE,
      R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE,
      R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q,
   };
   for (unsigned m = 0; m < 3; m++) {
      CHECK(r300_rs_tex_adj_probe_expected(&shape, records, m, image, size) == 0);
      CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, NULL, size,
                                         &census) == 0);
      CHECK(census.judged > 100);
      CHECK(census.match[m] == census.judged);
      for (unsigned other = 0; other < 3; other++)
         if (other != m)
            CHECK(census.match[other] < census.judged);
      CHECK(r300_rs_tex_adj_probe_classify(&census) == class_of_model[m]);
      if (m == 0)
         memcpy(control, image, size);
   }
   /* The candidate image equal to the perspective control names
    * UNCHANGED; the affine image against that control names AFFINE
    * with zero unchanged pixels. */
   CHECK(r300_rs_tex_adj_probe_census(&shape, records, control, control, size,
                                      &census) == 0);
   CHECK(census.unchanged == census.judged);
   CHECK(r300_rs_tex_adj_probe_classify(&census) ==
         R300_RS_TEX_ADJ_PROBE_CLASS_UNCHANGED);
   CHECK(r300_rs_tex_adj_probe_expected(
            &shape, records, R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE, image,
            size) == 0);
   CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, control, size,
                                      &census) == 0);
   CHECK(census.unchanged == 0);
   CHECK(r300_rs_tex_adj_probe_classify(&census) ==
         R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE);
   /* A shifted-center image (shift 3, both axes) classifies as
    * shifted-center: it misses perspective on some judged pixel. */
   {
      uint32_t *rows = image + shape.target_offset / 4u;
      memcpy(image, control, size);
      for (uint32_t y = 0; y < shape.height; y++)
         for (uint32_t x = 0; x < shape.width; x++) {
            float value[4];
            if (r300_rs_tex_adj_probe_model_value(
                   records, R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER, 3,
                   (float)x + 0.5f, (float)y + 0.5f, value)) {
               uint32_t d = 0;
               for (unsigned c = 0; c < 4; c++) {
                  float v = value[c];
                  uint32_t q = !(v > 0.0f) ? 0 : v >= 1.0f ? 255
                                  : (uint32_t)(v * 255.0f + 0.5f);
                  static const unsigned shift_of[4] = { 16, 8, 0, 24 };
                  d |= q << shift_of[c];
               }
               rows[y * shape.pitch_pixels + x] = d;
            }
         }
      CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, control,
                                         size, &census) == 0);
      CHECK(census.match[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER] ==
            census.judged);
      CHECK(census.match[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] <
            census.judged);
      CHECK(r300_rs_tex_adj_probe_classify(&census) ==
            R300_RS_TEX_ADJ_PROBE_CLASS_SHIFTED_CENTER);
   }
   /* An image matching no model is unclassified. */
   for (uint32_t p = 0; p < size / 4u; p++)
      image[p] = R300_TRIANGLE_COLOR_SENTINEL;
   CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, control, size,
                                      &census) == 0);
   CHECK(r300_rs_tex_adj_probe_classify(&census) ==
         R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED);

   /* Equalized W: the discriminator collapses to zero judged pixels. */
   float equal_w[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   memcpy(equal_w, records, sizeof(equal_w));
   equal_w[3] = equal_w[11] = equal_w[19] = 1.0f;
   CHECK(r300_rs_tex_adj_probe_census(&shape, equal_w, control, NULL, size,
                                      &census) == 0);
   CHECK(census.judged == 0);
   CHECK(r300_rs_tex_adj_probe_classify(&census) ==
         R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED);

   /* Constant q: the projective model coincides with perspective, so
    * the perspective image matches both and classifies as neither. */
   float const_q[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   memcpy(const_q, records, sizeof(const_q));
   const_q[7] = const_q[15] = const_q[23] = 1.0f;
   CHECK(r300_rs_tex_adj_probe_expected(
            &shape, const_q, R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE, image,
            size) == 0);
   CHECK(r300_rs_tex_adj_probe_census(&shape, const_q, image, NULL, size,
                                      &census) == 0);
   CHECK(census.judged > 100);
   CHECK(census.match[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q] ==
         census.judged);
   CHECK(r300_rs_tex_adj_probe_classify(&census) ==
         R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED);

   free(image);
   free(control);
}

/* The partial-clip form: vertex 0 projects off the target, the models
 * over the source triangle stay separable on the visible part, each
 * model's own image classifies as that model alone, and no judged
 * pixel touches the border the clip edge lies on. */
static void
test_partial_carrier_and_oracle(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.varying = true;
   float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   r300_rs_tex_adj_probe_partial_vertices(&shape, records);
   float clip[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   r300_rs_tex_adj_probe_partial_clip_vertices(clip);
   CHECK(records[0] < 0.0f);
   CHECK(clip[0] < -clip[3]);
   for (unsigned v = 1; v < 3; v++)
      CHECK(clip[v * 8] >= -clip[v * 8 + 3] && clip[v * 8] <= clip[v * 8 + 3]);
   const float x0 = (clip[0] / clip[3] + 1.0f) * (float)shape.width / 2.0f;
   CHECK(fabsf(x0 - records[0]) < 1e-3f);

   const uint32_t size = r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   uint32_t *image = malloc(size);
   CHECK(image != NULL);
   if (image == NULL)
      return;
   struct r300_rs_tex_adj_probe_census census;
   static const enum r300_rs_tex_adj_probe_classification class_of_model[] = {
      R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE,
      R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE,
      R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q,
   };
   for (unsigned m = 0; m < 3; m++) {
      CHECK(r300_rs_tex_adj_probe_expected(&shape, records, m, image, size) == 0);
      /* The clip edge is the target's left border: every pixel of
       * the border columns keeps the sentinel or the fill rule's
       * value, and the census leaves them unjudged. */
      CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, NULL, size,
                                         &census) == 0);
      CHECK(census.judged > 100);
      CHECK(census.match[m] == census.judged);
      for (unsigned other = 0; other < 3; other++)
         if (other != m)
            CHECK(census.match[other] < census.judged);
      CHECK(r300_rs_tex_adj_probe_classify(&census) == class_of_model[m]);
      /* A border-column mutation leaves the census unchanged. */
      uint32_t *row = image + shape.target_offset / 4u;
      for (uint32_t y = 0; y < shape.height; y++)
         row[y * shape.pitch_pixels] ^= 0x00ffffffu;
      struct r300_rs_tex_adj_probe_census mutated;
      CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, NULL, size,
                                         &mutated) == 0);
      CHECK(mutated.match[m] == census.judged);
   }
   free(image);
}

/* The per-channel census over the q-lane records (the probe's s, t, r
 * with alpha 1): each of the three payload channels separates the
 * models on some judged pixel, the constant alpha separates none, and
 * the affine prediction carries alpha 255 on every judged pixel while
 * an alpha-cleared image carries it on none. */
static void
test_channel_census(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   shape.varying = true;
   float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS];
   r300_rs_tex_adj_probe_vertices(&shape, records);
   for (unsigned v = 0; v < 3; v++)
      records[v * 8 + 7] = 1.0f;
   const uint32_t size = r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   uint32_t *image = malloc(size);
   CHECK(image != NULL);
   if (image == NULL)
      return;
   CHECK(r300_rs_tex_adj_probe_expected(&shape, records,
                                        R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE,
                                        image, size) == 0);
   struct r300_rs_tex_adj_probe_census census;
   CHECK(r300_rs_tex_adj_probe_census(&shape, records, image, NULL, size,
                                      &census) == 0);
   struct r300_rs_tex_adj_probe_channel_census channels;
   CHECK(r300_rs_tex_adj_probe_channel_census(&shape, records, image, size,
                                              &channels) == 0);
   CHECK(channels.judged == census.judged && channels.judged != 0);
   for (unsigned c = 0; c < 3; c++)
      CHECK(channels.separated[c] != 0);
   CHECK(channels.separated[3] == 0);
   CHECK(channels.alpha_one == channels.judged);
   for (uint32_t i = 0; i < size / 4u; i++)
      image[i] &= 0x00ffffffu;
   CHECK(r300_rs_tex_adj_probe_channel_census(&shape, records, image, size,
                                              &channels) == 0);
   CHECK(channels.alpha_one == 0);
   CHECK(r300_rs_tex_adj_probe_channel_census(NULL, records, image, size,
                                              &channels) == -EINVAL);
   free(image);
}

int
main(void)
{
   test_control_is_the_varying_cell();
   test_candidates_differ_in_one_word();
   test_plan_mutations_refuse_and_localize();
   test_stream_mutations();
   test_carrier_and_oracle();
   test_partial_carrier_and_oracle();
   test_channel_census();
   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return EXIT_FAILURE;
   }
   printf("r300-rs-tex-adj-probe: ok\n");
   return EXIT_SUCCESS;
}
