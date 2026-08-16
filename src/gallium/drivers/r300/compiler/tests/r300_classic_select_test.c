/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_select.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
#include "radeon_program_constants.h"

/* Selection corpus criterion: every corpus shader covers the admitted
 * opcode subset or cleanly rejects with a named reason.  Shaders run through
 * r300_optimize_nir first, the same production lowering nir_to_rc receives,
 * so selection sees real post-lowering shapes (flrp arrives as a fmad chain,
 * not as flrp). */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static unsigned
count_op(const struct r300_classic_program *p, enum r300_classic_op op)
{
   unsigned n = 0;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link)
      if (i->op == op)
         n++;
   return n;
}

static nir_builder
fs_builder(const char *name)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   return nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "%s",
                                         name);
}

static nir_variable *
add_varying(nir_builder *b, const char *name)
{
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                          glsl_vec4_type(), name);
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   return in;
}

static nir_variable *
add_color_output(nir_builder *b)
{
   nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   return out;
}

/* Optimize with the production lowering, then select. */
static void
select_shader(void *ctx, nir_shader *s,
              struct r300_classic_select_result *result)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, &r300_screen(ps)->caps);

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   CHECK(r300_classic_select(ctx, s, t, NULL, 4, R300_FS_INPUT_INTERPOLATED, NULL, result), "selection ran");
   if (result->program) {
      char err[128] = {0};
      CHECK(r300_classic_program_validate(result->program, err, sizeof(err)),
            "selected program validates");
      if (err[0])
         fprintf(stderr, "  validator said: %s\n", err);
   }
}

static void
case_fmad(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_fmad");
   nir_variable *in = add_varying(&b, "in_color");
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 7, 7, 7, 7);
   nir_def *c2 = nir_imm_vec4(&b, 1, 1, 1, 1);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, v, c1, c2), 0xf);

   struct r300_classic_select_result r;
   select_shader(ctx, b.shader, &r);
   CHECK(r.program != NULL, "fmad shader selected");
   if (!r.program && r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   if (r.program) {
      if (count_op(r.program, R300C_OP_MAD) == 0)
         r300_classic_program_print(r.program, stderr);
      CHECK(count_op(r.program, R300C_OP_MAD) >= 1, "fmad selects MAD");
      CHECK(count_op(r.program, R300C_OP_EXPORT_COLOR) == 1,
            "color export selected");
      CHECK(r.immediates.count >= 1, "immediates materialized");
      CHECK(r.immediates.first_index == 4, "immediates follow driver consts");
   }
   ralloc_free(ctx);
}

static void
case_flrp_lowers_before_selection(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_flrp");
   nir_variable *in = add_varying(&b, "in_t");
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.25f, 0.25f, 0.25f, 0.25f);
   nir_def *c2 = nir_imm_vec4(&b, 0.75f, 0.75f, 0.75f, 0.75f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_flrp, c1, c2, v), 0xf);

   struct r300_classic_select_result r;
   select_shader(ctx, b.shader, &r);
   CHECK(r.program != NULL, "flrp shader selected after production lowering");
   if (!r.program && r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   if (r.program)
      CHECK(count_op(r.program, R300C_OP_MAD) >= 1,
            "lowered flrp selects MAD");
   ralloc_free(ctx);
}

static void
case_tex(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_tex");
   nir_variable *in = add_varying(&b, "in_uv");
   nir_variable *out = add_color_output(&b);

   const struct glsl_type *sampler2d =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler2d, "tex0");
   sampler->data.binding = 0;
   nir_deref_instr *deref = nir_build_deref_var(&b, sampler);

   nir_def *uv = nir_trim_vector(&b, nir_load_var(&b, in), 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, &tex->def, 0xf);

   struct r300_classic_select_result r;
   select_shader(ctx, b.shader, &r);
   CHECK(r.program != NULL, "tex shader selected");
   if (!r.program && r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   if (r.program)
      CHECK(count_op(r.program, R300C_OP_TEX) == 1, "tex selects TEX");
   ralloc_free(ctx);
}

/* Build a one-sample shader over the given sampler dim; returns the
 * selection result through *r. */
static void
select_tex_dim(void *ctx, enum glsl_sampler_dim dim, unsigned coord_comps,
               bool shadow, struct r300_classic_select_result *r)
{
   nir_builder b = fs_builder("classic_tex_dim");
   nir_variable *in = add_varying(&b, "in_uv");
   nir_variable *out = add_color_output(&b);

   const struct glsl_type *stype =
      glsl_sampler_type(dim, shadow, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               stype, "tex0");
   sampler->data.binding = 0;
   nir_deref_instr *deref = nir_build_deref_var(&b, sampler);

   nir_def *coord =
      nir_trim_vector(&b, nir_load_var(&b, in), coord_comps);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = dim;
   tex->is_shadow = shadow;
   tex->coord_components = coord_comps;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_var(&b, out, &tex->def, 0xf);

   select_shader(ctx, b.shader, r);
}

/* Every sampled target must reach the TX block with its own rc target --
 * a cube sample emitted as RC_TEXTURE_2D is silently wrong on hardware --
 * and shadow comparison stays in nir_to_rc. */
static void
case_tex_targets(void)
{
   {
      void *ctx = ralloc_context(NULL);
      struct r300_classic_select_result r;
      select_tex_dim(ctx, GLSL_SAMPLER_DIM_CUBE, 3, false, &r);
      CHECK(r.program != NULL, "cube tex selects");
      if (r.program) {
         bool cube_target = false;
         list_for_each_entry (struct r300_classic_instr, i,
                              &r.program->instrs, link)
            if (i->op == R300C_OP_TEX && i->tex_target == RC_TEXTURE_CUBE)
               cube_target = true;
         CHECK(cube_target, "cube tex carries RC_TEXTURE_CUBE");
      } else if (r.reject_reason) {
         fprintf(stderr, "  rejected: %s\n", r.reject_reason);
      }
      ralloc_free(ctx);
   }
   {
      void *ctx = ralloc_context(NULL);
      struct r300_classic_select_result r;
      select_tex_dim(ctx, GLSL_SAMPLER_DIM_2D, 2, true, &r);
      CHECK(r.program == NULL, "shadow tex rejects");
      CHECK(r.reject_reason &&
            strstr(r.reject_reason, "shadow") != NULL, "shadow reject named");
      ralloc_free(ctx);
   }
   {
      /* R300 cannot sample rectangles natively: the entry's backend-tex
       * lowering normalizes the coordinate by the texrect-factor state
       * constant and retargets the sample to 2D. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_select_result r;
      select_tex_dim(ctx, GLSL_SAMPLER_DIM_RECT, 2, false, &r);
      CHECK(r.program != NULL, "rect tex selects");
      if (r.reject_reason)
         fprintf(stderr, "  rejected: %s\n", r.reject_reason);
      if (r.program) {
         CHECK(r.states.count >= 1, "texrect factor in the state table");
         bool retargeted = true;
         list_for_each_entry (struct r300_classic_instr, i,
                              &r.program->instrs, link)
            if (i->op == R300C_OP_TEX && i->tex_target != RC_TEXTURE_2D)
               retargeted = false;
         CHECK(retargeted, "rect sample retargets to 2D");
      }
      ralloc_free(ctx);
   }
}

/* WPOS and FACE record into the semantics table; the shared post-frontend
 * machinery (wpos varying routing, rc_transform_fragment_face) consumes
 * them identically for both front ends. */
static void
case_wpos_face_semantics(void)
{
   static const struct {
      gl_varying_slot location;
      const char *what;
   } rows[] = {
      { VARYING_SLOT_FACE, "face" },
   };
   for (unsigned n = 0; n < ARRAY_SIZE(rows); n++) {
      void *ctx = ralloc_context(NULL);
      nir_builder b = fs_builder("classic_special_input");
      nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "in_special");
      in->data.location = rows[n].location;
      in->data.driver_location = 0;
      in->data.interpolation = rows[n].location == VARYING_SLOT_FACE
                                  ? INTERP_MODE_FLAT
                                  : INTERP_MODE_SMOOTH;
      nir_variable *out = add_color_output(&b);
      nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

      static struct r300_screen screen;
      struct pipe_screen *ps = fake_r300_screen(&screen);
      r300_optimize_nir(b.shader, &r300_screen(ps)->caps);

      struct r300_shader_semantics sem;
      r300_shader_semantics_reset(&sem);
      const struct r300_classic_target *t =
         r300_classic_target_get(false, false);
      struct r300_classic_select_result r;
      CHECK(r300_classic_select(ctx, b.shader, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, &sem, &r),
            "selection ran");
      CHECK(r.program != NULL, rows[n].what);
      if (r.reject_reason)
         fprintf(stderr, "  rejected: %s\n", r.reject_reason);
      CHECK(sem.face == 0, "FACE input records at face");
      ralloc_free(ctx);
   }
}

static void
case_passthrough(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_mov");
   nir_variable *in = add_varying(&b, "in_color");
   nir_variable *out = add_color_output(&b);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   struct r300_classic_select_result r;
   select_shader(ctx, b.shader, &r);
   CHECK(r.program != NULL, "passthrough selected");
   if (!r.program && r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   ralloc_free(ctx);
}

static void
case_control_flow_rejects(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_if");
   nir_variable *in = add_varying(&b, "in_v");
   nir_variable *out = add_color_output(&b);
   nir_def *x = nir_channel(&b, nir_load_var(&b, in), 0);

   nir_push_if(&b, nir_flt_imm(&b, x, 0.5f));
   nir_def *a = nir_imm_vec4(&b, 1, 0, 0, 1);
   nir_push_else(&b, NULL);
   nir_def *c = nir_imm_vec4(&b, 0, 1, 0, 1);
   nir_pop_if(&b, NULL);
   nir_store_var(&b, out, nir_if_phi(&b, a, c), 0xf);

   /* Skip the production optimizer: flattening this trivial if into fcsel
    * is exactly what it would do, and the case pins the selector's own
    * control-flow reject. */
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result r;
   CHECK(r300_classic_select(ctx, b.shader, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, NULL, &r), "selection ran");
   CHECK(r.program == NULL, "control flow rejected");
   CHECK(r.reject_reason && strstr(r.reject_reason, "control flow") != NULL,
         "control-flow reject named");
   ralloc_free(ctx);
}

static void
case_integer_op_rejects(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_ixor");
   nir_variable *in = add_varying(&b, "in_v");
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *a = nir_f2u32(&b, nir_channel(&b, v, 0));
   nir_def *c = nir_f2u32(&b, nir_channel(&b, v, 1));
   nir_def *f = nir_u2f32(&b, nir_ixor(&b, a, c));
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result r;
   CHECK(r300_classic_select(ctx, b.shader, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, NULL, &r), "selection ran");
   CHECK(r.program == NULL, "integer op rejected");
   /* The entry lowering carries most integer math to float; a bitwise op
    * with no FP24-exact lowering is the named remainder. */
   CHECK(r.reject_reason && strstr(r.reject_reason, "bitwise") != NULL,
         "integer reject named");
   ralloc_free(ctx);
}

/* The r300 varying-slot convention packs texcoords at generic 0-7,
 * pointcoord at 8, and user varyings from 9 (ntr_fixup_varying_slots, applied
 * to VS outputs by the SW-TCL path).  Selection must record a VAR0 input at
 * generic[9], the index ntr_read_input_output records after the fixup --
 * generic[0] leaves the rasterizer route unassigned and the varying reads
 * garbage on hardware. */
static void
case_varying_semantics_match_fixup(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_semantics");
   nir_variable *in = add_varying(&b, "in_color");
   nir_variable *out = add_color_output(&b);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(b.shader, &r300_screen(ps)->caps);

   struct r300_shader_semantics sem;
   r300_shader_semantics_reset(&sem);
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result r;
   CHECK(r300_classic_select(ctx, b.shader, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, &sem, &r), "selection ran");
   CHECK(r.program != NULL, "varying shader selects");
   if (r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   CHECK(sem.generic[9] == 0, "VAR0 input records at generic[9]");
   CHECK(sem.generic[0] == ATTR_UNUSED, "generic[0] stays unused");
   CHECK(sem.num_generic == 1, "one generic input recorded");
   ralloc_free(ctx);
}

/* A POS input selects through the frag-coord reconstruction: the raw
 * varying feeds an RCP-led perspective divide with the viewport
 * scale/offset state constants, wpos records in the semantics table,
 * and the state table carries exactly the two viewport entries. */
static void
case_wpos_reconstruction(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_wpos");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "gl_FragCoord");
   in->data.location = VARYING_SLOT_POS;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_NOPERSPECTIVE;
   nir_variable *out = add_color_output(&b);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(b.shader, &r300_screen(ps)->caps);

   struct r300_shader_semantics sem;
   r300_shader_semantics_reset(&sem);
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result r;
   CHECK(r300_classic_select(ctx, b.shader, t, NULL, 0, R300_FS_INPUT_INTERPOLATED, &sem, &r), "selection ran");
   CHECK(r.program != NULL, "wpos shader selects");
   if (r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   if (r.program) {
      CHECK(sem.wpos == 0, "POS input records at wpos");
      CHECK(count_op(r.program, R300C_OP_RCP) >= 1,
            "reconstruction leads with RCP");
      CHECK(r.states.count == 2, "viewport scale and offset in the table");
   }
   ralloc_free(ctx);
}

/* A second color attachment selects as its own export and emission gives
 * it a distinct output register (the census's store_output-to-DATA1
 * fallback class). */
static void
case_mrt_data1_export(void)
{
   void *ctx = ralloc_context(NULL);
   nir_builder b = fs_builder("classic_mrt");
   nir_variable *in = add_varying(&b, "in0");
   nir_variable *out0 = add_color_output(&b);
   nir_variable *out1 = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "out1");
   out1->data.location = FRAG_RESULT_DATA1;
   out1->data.driver_location = 1;
   nir_def *v = nir_load_var(&b, in);
   nir_store_var(&b, out0, v, 0xf);
   nir_store_var(&b, out1, nir_fneg(&b, v), 0xf);

   struct r300_classic_select_result r;
   select_shader(ctx, b.shader, &r);
   CHECK(r.program != NULL, "mrt shader selects");
   if (!r.program && r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   if (r.program) {
      unsigned exports = 0, data1 = 0;
      list_for_each_entry (struct r300_classic_instr, i, &r.program->instrs,
                           link) {
         if (i->op == R300C_OP_EXPORT_COLOR) {
            exports++;
            if (i->export_index == 1)
               data1++;
         }
      }
      CHECK(exports == 2, "two color exports selected");
      CHECK(data1 == 1, "DATA1 carries export index 1");
   }
   ralloc_free(ctx);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   case_fmad();
   case_varying_semantics_match_fixup();
   case_flrp_lowers_before_selection();
   case_tex();
   case_tex_targets();
   case_wpos_face_semantics();
   case_wpos_reconstruction();
   case_mrt_data1_export();
   case_passthrough();
   case_control_flow_rejects();
   case_integer_op_rejects();
   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_select_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_select_test: all checks passed\n");
   return 0;
}
