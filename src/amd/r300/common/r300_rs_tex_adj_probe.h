/*
 * SPDX-License-Identifier: MIT
 *
 * Rasterizer interpolation discriminator on RS482 (Radeon Xpress 200M,
 * CHIP_RS480, R300-class US/PFS fixed VLIW): one TEX0 varying rides a
 * carrier whose vertices hold unequal reciprocal clip W, and a pair of
 * passes differing in exactly one rasterizer control word separates
 * perspective-correct interpolation from every other model the target
 * bytes can realize.
 */

#ifndef R300_RS_TEX_ADJ_PROBE_H
#define R300_RS_TEX_ADJ_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct r300_first_draw_contract;
struct r300_triangle_render_shape;

/* The candidate control word.  RS_INST.TEX_ADJ (bit 22) is documented
 * as the choice between real and adjusted pixel centers for texture
 * coordinate sampling (AMD R3xx 3D Registers, RS_INST_[0-15]); GB_SELECT
 * W_SELECT (bit 4) is documented as the source of the outgoing 1/W,
 * value 1 selecting 1.0 "to disable perspective correct
 * colors/textures" (AMD R3xx 3D Registers, GB_SELECT).  Neither carries
 * a retained silicon classification on RS482, so the probe treats each
 * as an unidentified control until a census against the registered
 * models names it. */
enum r300_rs_tex_adj_probe_candidate {
   /* Every probe word at its contract value: the legacy varying cell's
    * exact bytes. */
   R300_RS_TEX_ADJ_PROBE_CONTROL = 0,
   /* RS_INST_0 with TEX_ADJ set. */
   R300_RS_TEX_ADJ_PROBE_TEX_ADJ,
   /* GB_SELECT with W_SELECT set to 1.0. */
   R300_RS_TEX_ADJ_PROBE_W_SELECT_ONE,
};

enum r300_rs_tex_adj_probe_source {
   /* RS_IP_0 reads texture pointer 0 with the identity channel
    * selects; RS_INST_0 writes it through TEX_CN_WRITE. */
   R300_RS_TEX_ADJ_PROBE_SOURCE_TEX0 = 0,
   /* RS_IP_0 reads color pointer 0 while the carrier lands in the
    * texture vector: a calibration mutation. */
   R300_RS_TEX_ADJ_PROBE_SOURCE_COLOR0,
};

struct r300_rs_tex_adj_probe_plan {
   enum r300_rs_tex_adj_probe_candidate candidate;
   /* The RS instruction that carries a TEX_ADJ candidate: 0 is the
    * one instruction RS_INST_COUNT runs; 1 writes RS_INST_1, which
    * never executes, a calibration mutation. */
   uint32_t rs_instruction;
   enum r300_rs_tex_adj_probe_source rs_source;
};

void r300_rs_tex_adj_probe_plan_control(
   struct r300_rs_tex_adj_probe_plan *out);
void r300_rs_tex_adj_probe_plan_tex_adj(
   struct r300_rs_tex_adj_probe_plan *out);
void r300_rs_tex_adj_probe_plan_w_select_one(
   struct r300_rs_tex_adj_probe_plan *out);

/* Admits the control plan and the two canonical candidates alone:
 * instruction 0 and the TEX0 source.  Returns 0 or -EINVAL. */
int r300_rs_tex_adj_probe_plan_validate(
   const struct r300_rs_tex_adj_probe_plan *plan);

/* Register words the plan realizes in the cell's RS block. */
uint32_t r300_rs_tex_adj_probe_plan_rs_count(
   const struct r300_rs_tex_adj_probe_plan *plan);
uint32_t r300_rs_tex_adj_probe_plan_rs_ip_0(
   const struct r300_rs_tex_adj_probe_plan *plan);
/* RS_INST_n for instruction index 0 or 1. */
uint32_t r300_rs_tex_adj_probe_plan_rs_inst(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t instruction);
/* GB_SELECT over the contract's word: W_SELECT alone changes. */
uint32_t r300_rs_tex_adj_probe_plan_gb_select(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t base);
/* True when the plan writes RS_INST_1 as well as RS_INST_0. */
bool r300_rs_tex_adj_probe_plan_writes_rs_inst_1(
   const struct r300_rs_tex_adj_probe_plan *plan);

/* Programs the plan's contract-carried word, GB_SELECT, into a resolved
 * first-draw contract; the RS words ride the cell's own RS block.
 * Returns 0, or -EINVAL when the contract lacks GB_SELECT. */
int r300_rs_tex_adj_probe_plan_apply_contract(
   const struct r300_rs_tex_adj_probe_plan *plan,
   struct r300_first_draw_contract *contract);

/* The per-pass state check: ahead of every draw packet, RS_COUNT,
 * RS_INST_COUNT, RS_IP_0, RS_INST_0, GB_SELECT, and VAP_VSM_VTX_ASSM
 * hold the plan's words, and RS_INST_1 holds its word when the plan
 * writes it.  A pass begins with the first probe register a cell writes
 * after an earlier draw, so a pass that inherits any word from the
 * previous pass fails at its own draw.  Returns the number of draw
 * packets checked, a negative -(1 + index) naming the first deviating
 * draw, or -EINVAL for a NULL argument or a truncated packet. */
int r300_rs_tex_adj_probe_plan_stream_check(
   const struct r300_rs_tex_adj_probe_plan *plan, uint32_t gb_select_base,
   const uint32_t *ib, uint32_t ib_dwords);

/* The probe carrier: three eight-dword records, window position at the
 * shape's extent with z = 0 and the reciprocal clip W the R300
 * software-transformed vertex convention carries in the position's
 * fourth lane, then the TEX0 payload (s, t, r, q).  The reciprocal W
 * values 1, 1/4, and 1/2 make the perspective-correct and the
 * framebuffer-linear interpolants of every channel differ across the
 * interior, and q varies per vertex with s, t, r <= q at each vertex,
 * so a projective s/q, t/q, r/q reading stays inside the UNORM8 range
 * and separates from both. */
#define R300_RS_TEX_ADJ_PROBE_RECORD_DWORDS 8u
#define R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS 24u
extern const float r300_rs_tex_adj_probe_reciprocal_w[3];
extern const float r300_rs_tex_adj_probe_tex0[12];
void r300_rs_tex_adj_probe_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS]);
/* The clip-space form of the same carrier for a vertex shader that
 * passes position through: (x * w, y * w, 0, w) with w the reciprocal
 * of the record's fourth lane, so the CPU projection lands the window
 * position above and a reciprocal W proportional to the record's. */
void r300_rs_tex_adj_probe_clip_vertices(
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS]);

/* The partial-clip form of the carrier: vertex 0 moves to NDC x
 * R300_RS_TEX_ADJ_PROBE_PARTIAL_NDC_X0, past the x = -w plane, so the
 * clipper cuts the triangle on that one plane into a quad and the
 * rasterizer draws a fan of two triangles.  The window records carry
 * vertex 0's projected position off the target (negative window x);
 * every model evaluates over the source triangle, which is exact for
 * the visible part because perspective interpolation and the Vulkan
 * clipped NoPerspective value both restrict to the source triangle's
 * own interpolants on each fan triangle. */
#define R300_RS_TEX_ADJ_PROBE_PARTIAL_NDC_X0 (-1.5f)
void r300_rs_tex_adj_probe_partial_vertices(
   const struct r300_triangle_render_shape *shape,
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS]);
void r300_rs_tex_adj_probe_partial_clip_vertices(
   float out[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS]);

/* Interpolation models the census registers ahead of submission.
 * Each is a function of the record triple at a pixel center. */
enum r300_rs_tex_adj_probe_model {
   /* sum(l_i a_i W_i) / sum(l_i W_i) with W the record's reciprocal
    * clip W: the value a perspective-correct rasterizer produces. */
   R300_RS_TEX_ADJ_PROBE_MODEL_PERSPECTIVE = 0,
   /* sum(l_i a_i): framebuffer-linear interpolation, the value
    * Vulkan NoPerspective requires. */
   R300_RS_TEX_ADJ_PROBE_MODEL_AFFINE,
   /* The perspective-correct (s, t, r, q) with s, t, r divided by q:
    * a projective texture-coordinate adjustment. */
   R300_RS_TEX_ADJ_PROBE_MODEL_PROJECTIVE_Q,
   /* The perspective-correct value at one of the three pixel
    * positions shifted by +0.5 in x, y, or both: the documented
    * "adjusted pixel centers" reading of TEX_ADJ with a positive
    * shift; the document fixes no sign, so a negative shift lands in
    * the census as unclassified. */
   R300_RS_TEX_ADJ_PROBE_MODEL_SHIFTED_CENTER,
   R300_RS_TEX_ADJ_PROBE_MODEL_COUNT,
};
const char *r300_rs_tex_adj_probe_model_name(
   enum r300_rs_tex_adj_probe_model model);

/* Evaluates one model at a pixel center as four RGBA channel values in
 * [0, 1] before UNORM8 conversion; shift selects the shifted-center
 * variant (0..3, ignored by the other models).  Returns false outside
 * the triangle. */
bool r300_rs_tex_adj_probe_model_value(
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   enum r300_rs_tex_adj_probe_model model, unsigned shift, float px,
   float py, float value[4]);

/* Writes the model's predicted target image over the shape's full
 * footprint: interior pixels take the model's UNORM8 dword and every
 * other pixel the sentinel.  size_bytes is the footprint length. */
int r300_rs_tex_adj_probe_expected(
   const struct r300_triangle_render_shape *shape,
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   enum r300_rs_tex_adj_probe_model model, uint32_t *pixels,
   uint32_t size_bytes);

/* Deviation tolerance in UNORM8 quanta per channel: the FP24
 * interpolators contribute well under a quantum, and the color path's
 * UNORM8 rounding mode against the reference's round-to-nearest at
 * most one, so two quanta covers the conversion. */
#define R300_RS_TEX_ADJ_PROBE_TOLERANCE 2u
/* Pixels within this many pixels of the target's border are not judged:
 * a clip edge lies on the border, and the fill rule there is the
 * hardware's. */
#define R300_RS_TEX_ADJ_PROBE_BORDER_MARGIN 2.0
/* A pixel is judged when it sits at least the coverage oracle's margin
 * inside the triangle, the border margin inside the target, and the
 * perspective and affine predictions
 * differ by more than twice the tolerance in some channel, so no
 * observed dword can satisfy both. */
#define R300_RS_TEX_ADJ_PROBE_SEPARATION \
   (2u * R300_RS_TEX_ADJ_PROBE_TOLERANCE + 1u)

struct r300_rs_tex_adj_probe_census {
   /* Interior pixels the census judged. */
   uint32_t judged;
   /* Interior pixels the safe region excluded. */
   uint32_t unjudged_interior;
   /* Judged pixels each model matches within the tolerance on every
    * channel; for SHIFTED_CENTER any of its three shifts. */
   uint32_t match[R300_RS_TEX_ADJ_PROBE_MODEL_COUNT];
   /* Largest per-channel deviation from each model over judged pixels. */
   uint32_t max_deviation[R300_RS_TEX_ADJ_PROBE_MODEL_COUNT];
   /* Judged pixels whose dword equals the control image's, when a
    * control image is supplied: the "no observable effect" model. */
   uint32_t unchanged;
   bool control_supplied;
};

/* Runs the census over one target image.  control may be NULL. */
int r300_rs_tex_adj_probe_census(
   const struct r300_triangle_render_shape *shape,
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   const uint32_t *pixels, const uint32_t *control, uint32_t size_bytes,
   struct r300_rs_tex_adj_probe_census *out);

/* Per-channel view of the judged footprint: separated[c] counts the
 * judged pixels where channel c alone parts the perspective and affine
 * predictions by R300_RS_TEX_ADJ_PROBE_SEPARATION, and alpha_one the
 * judged pixels whose observed alpha byte is exactly 255.  A
 * multi-channel receipt names each channel's own separation count and
 * a constant-alpha program its exact alpha. */
struct r300_rs_tex_adj_probe_channel_census {
   uint32_t judged;
   uint32_t separated[4];
   uint32_t alpha_one;
   /* Per channel, the judged pixels whose observed byte lies within
    * R300_RS_TEX_ADJ_PROBE_TOLERANCE of each model's byte, and the
    * largest deviation from each model; a mixed-interpolation target
    * is judged one channel at a time against its own model. */
   uint32_t perspective_match[4];
   uint32_t affine_match[4];
   uint32_t perspective_max_deviation[4];
   uint32_t affine_max_deviation[4];
   /* Per channel, the separated pixels the competing model still
    * matches: zero when the channel's interpolation is decided. */
   uint32_t perspective_on_separated[4];
   uint32_t affine_on_separated[4];
   /* Judged pixels still holding the pre-submission sentinel dword
    * (R300_TRIANGLE_COLOR_SENTINEL), unwritten by the draw. */
   uint32_t sentinel;
};
int r300_rs_tex_adj_probe_channel_census(
   const struct r300_triangle_render_shape *shape,
   const float records[R300_RS_TEX_ADJ_PROBE_VERTEX_DWORDS],
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_rs_tex_adj_probe_channel_census *out);

/* The classification a census supports.  A model is named only when
 * it matches every judged pixel and no other model does; UNCHANGED is
 * named when the candidate image equals the control at every judged
 * pixel while the control itself is perspective; otherwise the census
 * is unclassified and its counts are the finding. */
enum r300_rs_tex_adj_probe_classification {
   R300_RS_TEX_ADJ_PROBE_UNCLASSIFIED = 0,
   R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE,
   R300_RS_TEX_ADJ_PROBE_CLASS_AFFINE,
   R300_RS_TEX_ADJ_PROBE_CLASS_PROJECTIVE_Q,
   R300_RS_TEX_ADJ_PROBE_CLASS_SHIFTED_CENTER,
   R300_RS_TEX_ADJ_PROBE_CLASS_UNCHANGED,
   /* Every judged pixel within tolerance of perspective while some
    * dword differs from the control: a sub-tolerance perturbation,
    * the signature a sample-position shift below the separation
    * leaves.  The rasterizer is deterministic over identical streams,
    * so a differing dword is the candidate word's effect. */
   R300_RS_TEX_ADJ_PROBE_CLASS_PERSPECTIVE_PERTURBED,
};
enum r300_rs_tex_adj_probe_classification
r300_rs_tex_adj_probe_classify(const struct r300_rs_tex_adj_probe_census *c);
const char *r300_rs_tex_adj_probe_classification_name(
   enum r300_rs_tex_adj_probe_classification cls);

#endif /* R300_RS_TEX_ADJ_PROBE_H */
