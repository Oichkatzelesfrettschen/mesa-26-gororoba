/* SPDX-License-Identifier: MIT */

#include "r300_rs_tex_adj_probe.h"

#include "r300_first_draw_state.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <math.h>
#include <string.h>

void
r300_rs_tex_adj_probe_plan_control(struct r300_rs_tex_adj_probe_plan *out)
{
   memset(out, 0, sizeof(*out));
   out->candidate = R300_RS_TEX_ADJ_PROBE_CONTROL;
   out->rs_instruction = 0;
   out->rs_source = R300_RS_TEX_ADJ_PROBE_SOURCE_TEX0;
}

void
r300_rs_tex_adj_probe_plan_tex_adj(struct r300_rs_tex_adj_probe_plan *out)
{
   r300_rs_tex_adj_probe_plan_control(out);
   out->candidate = R300_RS_TEX_ADJ_PROBE_TEX_ADJ;
}

void
r300_rs_tex_adj_probe_plan_w_select_one(
   struct r300_rs_tex_adj_probe_plan *out)
{
   r300_rs_tex_adj_probe_plan_control(out);
   out->candidate = R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE;
}

int
r300_rs_tex_adj_probe_plan_validate(
   const struct r300_rs_tex_adj_probe_plan *plan)
{
   if (plan == NULL)
      return -EINVAL;
   if (plan->candidate != R300_RS_TEX_ADJ_PROBE_CONTROL &&
       plan->candidate != R300_RS_TEX_ADJ_PROBE_TEX_ADJ &&
       plan->candidate != R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE)
      return -EINVAL;
   if (plan->rs_instruction != 0)
      return -EINVAL;
   if (plan->rs_source != R300_RS_TEX_ADJ_PROBE_SOURCE_TEX0)
      return -EINVAL;
   return 0;
}

uint32_t
r300_rs_tex_adj_probe_plan_rs_count(
   const struct r300_rs_tex_adj_probe_plan *plan)
{
   /* Four interpolated texture components, no rasterized colors, the
    * high-resolution output the legacy varying cell declares. */
   (void)plan;
   return R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN;
}

uint32_t
r300_rs_tex_adj_probe_plan_rs_ip_0(
   const struct r300_rs_tex_adj_probe_plan *plan)
{
   if (plan->rs_source == R300_RS_TEX_ADJ_PROBE_SOURCE_COLOR0)
      return R300_RS_COL_PTR(0) | R300_RS_COL_FMT(R300_RS_COL_FMT_RGBA);
   return R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3);
}

uint32_t
r300_rs_tex_adj_probe_plan_rs_inst(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t instruction)
{
   uint32_t word;
   if (plan->rs_source == R300_RS_TEX_ADJ_PROBE_SOURCE_COLOR0)
      word = R300_RS_INST_COL_ID(0) | R300_RS_INST_COL_CN_WRITE |
             R300_RS_INST_COL_ADDR(0);
   else
      word = R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
             R300_RS_INST_TEX_ADDR(0);
   if (instruction != 0)
      word = 0;
   if (plan->candidate == R300_RS_TEX_ADJ_PROBE_TEX_ADJ &&
       instruction == plan->rs_instruction)
      word |= R300_RS_INST_TEX_ADJ;
   return word;
}

uint32_t
r300_rs_tex_adj_probe_plan_gb_select(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t base)
{
   const uint32_t word = base & ~(uint32_t)R300_GB_W_SELECT_1;
   if (plan->candidate == R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE)
      return word | R300_GB_W_SELECT_1;
   return word;
}

bool
r300_rs_tex_adj_probe_plan_writes_rs_inst_1(
   const struct r300_rs_tex_adj_probe_plan *plan)
{
   return plan->candidate == R300_RS_TEX_ADJ_PROBE_TEX_ADJ &&
          plan->rs_instruction == 1;
}

int
r300_rs_tex_adj_probe_plan_apply_contract(
   const struct r300_rs_tex_adj_probe_plan *plan,
   struct r300_first_draw_contract *contract)
{
   if (plan == NULL || contract == NULL)
      return -EINVAL;
   for (uint32_t i = 0; i < contract->count; i++) {
      if (contract->entries[i].reg == R300_GB_SELECT) {
         return r300_first_draw_contract_set_entry(
            contract, R300_GB_SELECT,
            r300_rs_tex_adj_probe_plan_gb_select(
               plan, contract->entries[i].value));
      }
   }
   return -EINVAL;
}

struct probe_register {
   uint32_t reg;
   uint32_t value;
};

#define PROBE_REGISTER_MAX 7u

static unsigned
probe_registers(const struct r300_rs_tex_adj_probe_plan *plan,
                uint32_t gb_select_base,
                struct probe_register out[PROBE_REGISTER_MAX])
{
   unsigned n = 0;
   out[n].reg = R300_GB_SELECT;
   out[n++].value = r300_rs_tex_adj_probe_plan_gb_select(plan, gb_select_base);
   out[n].reg = R300_VAP_VSM_VTX_ASSM;
   out[n++].value = R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
   out[n].reg = R300_RS_COUNT;
   out[n++].value = r300_rs_tex_adj_probe_plan_rs_count(plan);
   out[n].reg = R300_RS_INST_COUNT;
   out[n++].value = 0;
   out[n].reg = R300_RS_IP_0;
   out[n++].value = r300_rs_tex_adj_probe_plan_rs_ip_0(plan);
   out[n].reg = R300_RS_INST_0;
   out[n++].value = r300_rs_tex_adj_probe_plan_rs_inst(plan, 0);
   if (r300_rs_tex_adj_probe_plan_writes_rs_inst_1(plan)) {
      out[n].reg = R300_RS_INST_1;
      out[n++].value = r300_rs_tex_adj_probe_plan_rs_inst(plan, 1);
   }
   return n;
}

int
r300_rs_tex_adj_probe_plan_stream_check(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t gb_select_base,
   const uint32_t *ib, uint32_t ib_dwords)
{
   if (plan == NULL || ib == NULL)
      return -EINVAL;
   struct probe_register regs[PROBE_REGISTER_MAX];
   const unsigned n = probe_registers(plan, gb_select_base, regs);
   bool written[PROBE_REGISTER_MAX] = { false };
   uint32_t state[PROBE_REGISTER_MAX] = { 0 };
   int draws = 0;
   bool pass_consumed = false;
   uint32_t i = 0;
   while (i < ib_dwords) {
      const uint32_t header = ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (kind == 0) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = base + (one_reg ? 0 : 4 * k);
            for (unsigned e = 0; e < n; e++) {
               if (regs[e].reg != reg)
                  continue;
               /* The first probe register a cell writes after an
                * earlier draw opens that cell's pass: the previous
                * pass's words count for nothing from here. */
               if (pass_consumed) {
                  memset(written, 0, sizeof(written));
                  pass_consumed = false;
               }
               state[e] = ib[i + 1 + k];
               written[e] = true;
            }
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         if (r300_first_draw_is_draw_packet(header)) {
            for (unsigned e = 0; e < n; e++) {
               if (!written[e] || state[e] != regs[e].value)
                  return -(1 + draws);
            }
            draws++;
            pass_consumed = true;
         }
         i += 1 + count;
      } else {
         i += 1;
      }
   }
   return draws;
}

/* Reciprocal clip W per vertex: strongly unequal so the perspective
 * weights W_i / sum(l_j W_j) differ from the affine l_i across the
 * whole interior. */
const float r300_rs_tex_adj_probe_reciprocal_w[3] = { 1.0f, 0.25f, 0.5f };

/* TEX0 (s, t, r, q) per vertex, with s, t, r <= q at each vertex so a
 * projective s/q reading stays in [0, 1], and q distinct per vertex. */
const float r300_rs_tex_adj_probe_tex0[12] = {
   0.25f, 0.75f, 0.5f,  1.0f,
   0.4f,  0.1f,  0.3f,  0.5f,
   0.1f,  0.2f,  0.05f, 0.25f,
};

void
r300_rs_tex_adj_probe_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS])
{
   float window[6];
   r300_tcl_bypass_triangle_window_vertices(shape->width, shape->height,
                                            window);
   for (unsigned i = 0; i < 3; i++) {
      out[i * 8 + 0] = window[i * 2 + 0];
      out[i * 8 + 1] = window[i * 2 + 1];
      out[i * 8 + 2] = 0.0f;
      out[i * 8 + 3] = r300_rs_tex_adj_probe_reciprocal_w[i];
      for (unsigned c = 0; c < 4; c++)
         out[i * 8 + 4 + c] = r300_rs_tex_adj_probe_tex0[i * 4 + c];
   }
}

void
r300_rs_tex_adj_probe_clip_vertices(
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS])
{
   static const float ndc[6] = { -0.75f, -0.75f, 0.75f, -0.75f, 0.0f, 0.75f };
   for (unsigned i = 0; i < 3; i++) {
      const float w = 1.0f / r300_rs_tex_adj_probe_reciprocal_w[i];
      out[i * 8 + 0] = ndc[i * 2 + 0] * w;
      out[i * 8 + 1] = ndc[i * 2 + 1] * w;
      out[i * 8 + 2] = 0.0f;
      out[i * 8 + 3] = w;
      for (unsigned c = 0; c < 4; c++)
         out[i * 8 + 4 + c] = r300_rs_tex_adj_probe_tex0[i * 4 + c];
   }
}

const char *
r300_rs_tex_adj_probe_model_name(enum r300_rs_tex_adj_probe_model model)
{
   switch (model) {
   case R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE: return "perspective";
   case R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE: return "affine";
   case R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q: return "projective-q";
   case R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER: return "shifted-center";
   default: return "unknown";
   }
}

static double
edge(double ax, double ay, double bx, double by, double px, double py)
{
   return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Barycentric weights of (px, py) over the record triple's window
 * positions; false outside the triangle or on a degenerate one. */
static bool
barycentric(const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
            double px, double py, double l[3])
{
   const double ax = records[0], ay = records[1];
   const double bx = records[8], by = records[9];
   const double cx = records[16], cy = records[17];
   const double area = edge(ax, ay, bx, by, cx, cy);
   if (area == 0.0)
      return false;
   l[0] = edge(bx, by, cx, cy, px, py) / area;
   l[1] = edge(cx, cy, ax, ay, px, py) / area;
   l[2] = 1.0 - l[0] - l[1];
   return l[0] >= 0.0 && l[1] >= 0.0 && l[2] >= 0.0;
}

bool
r300_rs_tex_adj_probe_model_value(
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   enum r300_rs_tex_adj_probe_model model, unsigned shift, float px,
   float py, float value[4])
{
   double sx = px, sy = py;
   if (model == R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER) {
      sx += (shift & 1u) ? 0.5 : 0.0;
      sy += (shift & 2u) ? 0.5 : 0.0;
   }
   double l[3];
   if (!barycentric(records, sx, sy, l))
      return false;
   double interp[4];
   if (model == R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE) {
      for (unsigned c = 0; c < 4; c++)
         interp[c] = l[0] * records[4 + c] + l[1] * records[12 + c] +
                     l[2] * records[20 + c];
   } else {
      const double w0 = records[3], w1 = records[11], w2 = records[19];
      const double denominator = l[0] * w0 + l[1] * w1 + l[2] * w2;
      if (!(denominator > 0.0))
         return false;
      for (unsigned c = 0; c < 4; c++)
         interp[c] = (l[0] * w0 * records[4 + c] +
                      l[1] * w1 * records[12 + c] +
                      l[2] * w2 * records[20 + c]) / denominator;
   }
   if (model == R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q) {
      if (!(interp[3] > 0.0))
         return false;
      for (unsigned c = 0; c < 3; c++)
         interp[c] /= interp[3];
   }
   for (unsigned c = 0; c < 4; c++)
      value[c] = (float)interp[c];
   return true;
}

static uint32_t
unorm8(float value)
{
   if (!(value > 0.0f))
      return 0;
   if (value >= 1.0f)
      return 255;
   return (uint32_t)(value * 255.0f + 0.5f);
}

/* B8G8R8A8: byte 0 blue, 1 green, 2 red, 3 alpha; the payload is
 * (s, t, r, q) read as RGBA. */
static uint32_t
pack_dword(const float value[4])
{
   return unorm8(value[2]) | (unorm8(value[1]) << 8) |
          (unorm8(value[0]) << 16) | (unorm8(value[3]) << 24);
}

static uint32_t
channel_byte(uint32_t dword, unsigned channel)
{
   static const unsigned shift_of_channel[4] = { 16, 8, 0, 24 };
   return (dword >> shift_of_channel[channel]) & 0xffu;
}

/* Signed distance of (px, py) inside the record triangle, the minimum
 * over its three edges, positive inside. */
static double
signed_margin(const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
              double px, double py)
{
   const double v[6] = { records[0], records[1], records[8],
                         records[9], records[16], records[17] };
   const double area = edge(v[0], v[1], v[2], v[3], v[4], v[5]);
   const double sign = area > 0.0 ? 1.0 : -1.0;
   double margin = INFINITY;
   for (unsigned e = 0; e < 3; e++) {
      const double ax = v[e * 2], ay = v[e * 2 + 1];
      const double bx = v[((e + 1) % 3) * 2], by = v[((e + 1) % 3) * 2 + 1];
      const double length = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
      if (length == 0.0)
         return -INFINITY;
      const double d = sign * edge(ax, ay, bx, by, px, py) / length;
      if (d < margin)
         margin = d;
   }
   return margin;
}

static bool
footprint(const struct r300_triangle_render_shape *shape,
          uint32_t size_bytes, uint32_t *footprint_bytes)
{
   const uint64_t bytes =
      (uint64_t)shape->target_offset +
      (uint64_t)shape->pitch_pixels *
         (shape->height + R300_TRIANGLE_CANARY_ROWS) * 4u;
   if (bytes > UINT32_MAX || size_bytes < bytes)
      return false;
   *footprint_bytes = (uint32_t)bytes;
   return true;
}

/* A pixel is judged when it sits two pixels inside the triangle (the
 * coverage oracle's margin, so the verdict does not ride the fill
 * rule) and the perspective and affine predictions separate by
 * R300_RS_TEX_ADJ_PROBE_SEPARATION in some channel. */
static bool
judged_pixel(const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
             uint32_t x, uint32_t y, uint32_t *perspective_dword,
             uint32_t *affine_dword)
{
   const float px = (float)x + 0.5f, py = (float)y + 0.5f;
   if (signed_margin(records, px, py) < 2.0)
      return false;
   float p[4], a[4];
   if (!r300_rs_tex_adj_probe_model_value(
          records, R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE, 0, px, py, p) ||
       !r300_rs_tex_adj_probe_model_value(
          records, R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE, 0, px, py, a))
      return false;
   *perspective_dword = pack_dword(p);
   *affine_dword = pack_dword(a);
   for (unsigned c = 0; c < 4; c++) {
      const uint32_t pb = channel_byte(*perspective_dword, c);
      const uint32_t ab = channel_byte(*affine_dword, c);
      const uint32_t d = pb > ab ? pb - ab : ab - pb;
      if (d >= R300_RS_TEX_ADJ_PROBE_SEPARATION)
         return true;
   }
   return false;
}

int
r300_rs_tex_adj_probe_expected(
   const struct r300_triangle_render_shape *shape,
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   enum r300_rs_tex_adj_probe_model model, uint32_t *pixels,
   uint32_t size_bytes)
{
   if (shape == NULL || records == NULL || pixels == NULL ||
       model >= R300_RS_TEX_ADJ_PROBE_MODEL_COUNT)
      return -EINVAL;
   uint32_t footprint_bytes;
   if (!footprint(shape, size_bytes, &footprint_bytes))
      return -EINVAL;
   for (uint32_t i = 0; i < footprint_bytes / 4u; i++)
      pixels[i] = R300_TRIANGLE_COLOR_SENTINEL;
   uint32_t *rows = pixels + shape->target_offset / 4u;
   for (uint32_t y = 0; y < shape->height; y++) {
      for (uint32_t x = 0; x < shape->width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         float value[4];
         if (signed_margin(records, px, py) <= 0.0 ||
             !r300_rs_tex_adj_probe_model_value(records, model, 0, px, py,
                                                value))
            continue;
         rows[y * shape->pitch_pixels + x] = pack_dword(value);
      }
   }
   return 0;
}

static bool
matches(uint32_t observed, uint32_t predicted, uint32_t *max_deviation)
{
   bool within = true;
   for (unsigned c = 0; c < 4; c++) {
      const uint32_t o = channel_byte(observed, c);
      const uint32_t p = channel_byte(predicted, c);
      const uint32_t d = o > p ? o - p : p - o;
      if (d > *max_deviation)
         *max_deviation = d;
      if (d > R300_RS_TEX_ADJ_PROBE_TOLERANCE)
         within = false;
   }
   return within;
}

int
r300_rs_tex_adj_probe_census(
   const struct r300_triangle_render_shape *shape,
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   const uint32_t *pixels, const uint32_t *control, uint32_t size_bytes,
   struct r300_rs_tex_adj_probe_census *out)
{
   if (shape == NULL || records == NULL || pixels == NULL || out == NULL)
      return -EINVAL;
   uint32_t footprint_bytes;
   if (!footprint(shape, size_bytes, &footprint_bytes))
      return -EINVAL;
   memset(out, 0, sizeof(*out));
   out->control_supplied = control != NULL;
   const uint32_t *rows = pixels + shape->target_offset / 4u;
   const uint32_t *control_rows =
      control != NULL ? control + shape->target_offset / 4u : NULL;
   for (uint32_t y = 0; y < shape->height; y++) {
      for (uint32_t x = 0; x < shape->width; x++) {
         const float px = (float)x + 0.5f, py = (float)y + 0.5f;
         if (signed_margin(records, px, py) <= 0.0)
            continue;
         uint32_t perspective_dword, affine_dword;
         if (!judged_pixel(records, x, y, &perspective_dword,
                           &affine_dword)) {
            out->unjudged_interior++;
            continue;
         }
         out->judged++;
         const uint32_t observed = rows[y * shape->pitch_pixels + x];
         if (matches(observed, perspective_dword,
                     &out->max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE]))
            out->match[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE]++;
         if (matches(observed, affine_dword,
                     &out->max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE]))
            out->match[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE]++;
         float value[4];
         if (r300_rs_tex_adj_probe_model_value(
                records, R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q, 0, px, py,
                value) &&
             matches(observed, pack_dword(value),
                     &out->max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q]))
            out->match[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q]++;
         /* Any of the three shifted centers within tolerance matches;
          * the unshifted center is the perspective row, so this row
          * stays independent of it.  The recorded deviation is the
          * smallest over the shifts. */
         bool shifted = false;
         uint32_t best = UINT32_MAX;
         for (unsigned s = 1; s < 4; s++) {
            if (!r300_rs_tex_adj_probe_model_value(
                   records, R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER, s,
                   px, py, value))
               continue;
            uint32_t deviation = 0;
            if (matches(observed, pack_dword(value), &deviation))
               shifted = true;
            if (deviation < best)
               best = deviation;
         }
         if (shifted)
            out->match[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER]++;
         if (best != UINT32_MAX &&
             best > out->max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER])
            out->max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER] =
               best;
         if (control_rows != NULL &&
             control_rows[y * shape->pitch_pixels + x] == observed)
            out->unchanged++;
      }
   }
   return 0;
}

enum r300_rs_tex_adj_probe_classification
r300_rs_tex_adj_probe_classify(const struct r300_rs_tex_adj_probe_census *c)
{
   if (c == NULL || c->judged == 0)
      return R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED;
   /* The four analytic models in exclusivity order: a model is named
    * only when it alone matches every judged pixel.  The shifted-center
    * model contains the unshifted perspective one (shift 0), so a
    * perspective match reads as perspective, and shifted-center names
    * only a census perspective misses. */
   const bool full[R300_RS_TEX_ADJ_PROBE_MODEL_COUNT] = {
      c->match[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] == c->judged,
      c->match[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] == c->judged,
      c->match[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q] == c->judged,
      c->match[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER] == c->judged,
   };
   if (full[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q]) {
      if (!c->control_supplied)
         return R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE;
      if (c->unchanged == c->judged)
         return R300_RS_TEX_ADJ_PROBE_CLASS_UNCHANGED;
      return R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE_PERTURBED;
   }
   if (full[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER])
      return R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE;
   if (full[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER])
      return R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q;
   if (full[R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE] &&
       !full[R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q])
      return R300_RS_TEX_ADJ_PROBE_CLASS_SHIFTED_CENTER;
   return R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED;
}

const char *
r300_rs_tex_adj_probe_classification_name(
   enum r300_rs_tex_adj_probe_classification cls)
{
   switch (cls) {
   case R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE: return "perspective";
   case R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE: return "affine";
   case R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q: return "projective-q";
   case R300_RS_TEX_ADJ_PROBE_CLASS_SHIFTED_CENTER: return "shifted-center";
   case R300_RS_TEX_ADJ_PROBE_CLASS_UNCHANGED: return "unchanged";
   case R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE_PERTURBED:
      return "perspective-perturbed";
   default: return "unclassified";
   }
}
