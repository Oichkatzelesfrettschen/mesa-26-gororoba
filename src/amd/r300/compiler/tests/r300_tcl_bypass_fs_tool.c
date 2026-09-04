/*
 * SPDX-License-Identifier: MIT
 *
 * Compiles the native cells' fragment programs through the classic
 * ladder and the production cb_code baker, so each cell's US block is
 * compiler output instead of a hand-built placeholder.  The constant-color
 * program shades the TCL-bypass triangle cell; the varying-passthrough
 * program routes interpolator 0 to the color output, the shape the R2VB
 * producer pass needs to store its embedded record into the carrier.
 * --emit prints a golden header; --check recompiles and byte-compares
 * against the checked-in golden, so the meson test proves the checked-in
 * block regenerates from source.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "util/macros.h"

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_emit.h"
#include "classic/r300_classic_regalloc.h"
#include "r300_context.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_regalloc.h"

#include "amd/r300/common/r300_noperspective_mixed_carrier_fs_block.h"
#include "amd/r300/common/r300_noperspective_q_lane_fs_block.h"
#include "amd/r300/common/r300_noperspective_reciprocal_fs_block.h"
#include "amd/r300/common/r300_r2vb_producer_fs_block.h"
#include "amd/r300/common/r300_tcl_bypass_sampled_fs_block.h"
#include "amd/r300/common/r300_tcl_bypass_triangle_fs_block.h"

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   /* Zeroed caps select the plain R300 US register emission, the register
    * set the RS485M-family cell programs.
    */
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static void
allocate_identity_inputs(struct r300_fragment_program_compiler *c,
                         void (*allocate)(void *data, unsigned input,
                                          unsigned hwreg),
                         void *mydata)
{
   for (unsigned i = 0; i < 8; i++)
      allocate(mydata, i, i);
}

/* The draw color is the byte-order oracle constant: four distinct normalized
 * components produce four distinct ARGB8888 bytes in the color target.
 */
static nir_shader *
build_constant_color_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "tcl_bypass_constant_color");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   nir_store_var(&b, out,
                 nir_imm_vec4(&b, 0.125f, 0.375f, 0.625f, 0.875f), 0xf);
   return b.shader;
}

/* The producer pass stores one interpolated FP32x4 record per rasterized
 * slot pixel, so its program reads texture coordinate set 0 and writes it
 * to the color output unchanged.  The varying reaches the US through
 * interpolator 0, the destination RS_INST_0's TEX_ADDR names.
 */
static nir_shader *
build_varying_passthrough_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "r2vb_varying_passthrough");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "record");
   in->data.location = VARYING_SLOT_TEX0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_NONE;
   b.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_TEX0);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);
   return b.shader;
}

/* The sampling cell's program reads texture coordinate set 0 and samples
 * combined texture/sampler 0 at its xy, the fetch RS routes through
 * interpolator 0 and the TX unit resolves.
 */
static nir_shader *
build_sampled_texture_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "tcl_bypass_sampled_texture");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "texcoord");
   in->data.location = VARYING_SLOT_TEX0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_NONE;
   b.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_TEX0);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *coord = nir_channels(&b, nir_load_var(&b, in), 0x3);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 0;
   tex->sampler_index = 0;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_var(&b, out, &tex->def, 0xf);
   return b.shader;
}

/* Runs the classic ladder plus the backend pass chain and bakes cb_code
 * through the driver's own r300_emit_fs_code_to_buffer.  Returns the baked
 * shader in *shader (cb_code owned by the caller) or nonzero on failure.
 */
static int
compile_block(struct r300_fragment_shader_code *shader,
              nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   nir_shader *s = build();

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, &r300_screen(ps)->caps);

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   if (!r300_classic_select(ctx, s, t, NULL, 0, R300_FS_INPUT_INTERPOLATED,
                            NULL, &sel) ||
       sel.program == NULL) {
      fprintf(stderr, "selection failed: %s\n",
              sel.reject_reason != NULL ? sel.reject_reason : "unknown");
      ralloc_free(ctx);
      return 1;
   }

   struct r300_classic_regalloc_result ra;
   if (!r300_classic_regalloc(ctx, sel.program, &ra) ||
       ra.temp_of_ssa == NULL) {
      fprintf(stderr, "register allocation failed\n");
      ralloc_free(ctx);
      return 1;
   }

   struct rc_regalloc_state rs;
   rc_init_regalloc_state(&rs, RC_FRAGMENT_PROGRAM);
   struct r300_fragment_program_compiler fc;
   memset(&fc, 0, sizeof(fc));
   rc_init(&fc.Base, &rs);
   fc.Base.type = RC_FRAGMENT_PROGRAM;
   fc.Base.has_half_swizzles = true;
   fc.Base.has_presub = true;
   fc.Base.has_omod = true;
   fc.Base.max_temp_regs = t->max_temp_regs;
   fc.Base.max_constants = t->max_const_regs;
   fc.Base.max_alu_insts = t->max_alu_insts;
   fc.Base.max_tex_insts = t->max_tex_insts;

   memset(shader, 0, sizeof(*shader));
   fc.code = &shader->code;
   fc.AllocateHwInputs = allocate_identity_inputs;

   int result = 1;
   if (r300_classic_emit(sel.program, &sel.immediates, &sel.states, &fc)) {
      r3xx_compile_fragment_program(&fc);
      if (!fc.Base.Error && shader->code.code.r300.alu.length > 0) {
         for (unsigned i = 0; i < shader->code.constants.Count; i++) {
            if (shader->code.constants.Constants[i].Type ==
                RC_CONSTANT_IMMEDIATE)
               shader->immediates_count++;
            else
               shader->externals_count++;
         }
         /* The constant-color program writes no depth; both depth-output
          * registers keep their zero defaults.
          */
         shader->fg_depth_src = 0;
         shader->us_out_w = 0;

         static struct r300_context fake_context;
         memset(&fake_context, 0, sizeof(fake_context));
         fake_context.screen = &screen;
         r300_emit_fs_code_to_buffer(&fake_context, shader);
         result = shader->cb_code != NULL ? 0 : 1;
      } else if (fc.Base.Error && fc.Base.ErrorMsg != NULL) {
         fprintf(stderr, "backend failed: %s\n", fc.Base.ErrorMsg);
      }
   } else {
      fprintf(stderr, "classic emission failed\n");
   }

   rc_destroy(&fc.Base);
   rc_destroy_regalloc_state(&rs);
   ralloc_free(ctx);
   return result;
}

/* One compiled cell program: the NIR builder that produces it, the header
 * it bakes into, and the golden block --check compares against.
 */
/* The NoPerspective reciprocal-carrier cell: texture coordinate set 0
 * carries the premultiplied payload a * w and set 1's x lane the shared
 * carrier w (r300_noperspective_reciprocal_plan.h), both perspective
 * interpolated, so the program recovers the window-linear value as
 * payload * rcp(carrier.x) through interpolators 0 and 1.
 */
static nir_shader *
build_noperspective_reciprocal_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "noperspective_reciprocal_carrier");
   nir_variable *payload = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "payload");
   payload->data.location = VARYING_SLOT_TEX0;
   payload->data.driver_location = 0;
   payload->data.interpolation = INTERP_MODE_NONE;
   nir_variable *carrier = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "carrier");
   carrier->data.location = VARYING_SLOT_TEX1;
   carrier->data.driver_location = 1;
   carrier->data.interpolation = INTERP_MODE_NONE;
   b.shader->info.inputs_read =
      BITFIELD64_BIT(VARYING_SLOT_TEX0) | BITFIELD64_BIT(VARYING_SLOT_TEX1);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   nir_def *reciprocal =
      nir_frcp(&b, nir_channel(&b, nir_load_var(&b, carrier), 0));
   nir_store_var(&b, out, nir_fmul(&b, nir_load_var(&b, payload),
                                   reciprocal),
                 0xf);
   return b.shader;
}

/* The NoPerspective q-lane cell: texture coordinate set 0 carries the
 * premultiplied payload a * c in xyz and the normalized carrier c in
 * w (r300_noperspective_q_lane_plan.h), perspective interpolated as
 * one vector, so the program recovers the window-linear value as
 * xyz * rcp(w) through interpolator 0 alone and writes alpha 1.0.
 */
static nir_shader *
build_noperspective_q_lane_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "noperspective_q_lane_carrier");
   nir_variable *payload = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "payload");
   payload->data.location = VARYING_SLOT_TEX0;
   payload->data.driver_location = 0;
   payload->data.interpolation = INTERP_MODE_NONE;
   b.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_TEX0);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   nir_def *loaded = nir_load_var(&b, payload);
   nir_def *reciprocal = nir_frcp(&b, nir_channel(&b, loaded, 3));
   nir_def *xyz = nir_fmul(&b, nir_trim_vector(&b, loaded, 3), reciprocal);
   nir_store_var(&b, out,
                 nir_vec4(&b, nir_channel(&b, xyz, 0),
                          nir_channel(&b, xyz, 1), nir_channel(&b, xyz, 2),
                          nir_imm_float(&b, 1.0f)),
                 0xf);
   return b.shader;
}

/* The mixed carrier cell: texture coordinate set 0 carries the Smooth
 * vec4, set 1 the NoPerspective vec4 premultiplied by the normalized
 * carrier c, and set 2's x lane c itself
 * (r300_noperspective_mixed_carrier_plan.h), all perspective
 * interpolated, so the program stores (set0.x, set0.y, r.x, r.y) with
 * r = set1 * rcp(set2.x): two lanes that stay perspective beside two
 * recovered window-linear lanes of one draw.
 */
static nir_shader *
build_noperspective_mixed_carrier_shader(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "noperspective_mixed_carrier");
   nir_variable *smooth = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "smooth");
   smooth->data.location = VARYING_SLOT_TEX0;
   smooth->data.driver_location = 0;
   smooth->data.interpolation = INTERP_MODE_NONE;
   nir_variable *payload = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "payload");
   payload->data.location = VARYING_SLOT_TEX1;
   payload->data.driver_location = 1;
   payload->data.interpolation = INTERP_MODE_NONE;
   nir_variable *carrier = nir_variable_create(
      b.shader, nir_var_shader_in, glsl_vec4_type(), "carrier");
   carrier->data.location = VARYING_SLOT_TEX2;
   carrier->data.driver_location = 2;
   carrier->data.interpolation = INTERP_MODE_NONE;
   b.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_TEX0) |
                                BITFIELD64_BIT(VARYING_SLOT_TEX1) |
                                BITFIELD64_BIT(VARYING_SLOT_TEX2);

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   nir_def *s = nir_load_var(&b, smooth);
   nir_def *reciprocal =
      nir_frcp(&b, nir_channel(&b, nir_load_var(&b, carrier), 0));
   nir_def *recovered =
      nir_fmul(&b, nir_trim_vector(&b, nir_load_var(&b, payload), 2),
               reciprocal);
   nir_store_var(&b, out,
                 nir_vec4(&b, nir_channel(&b, s, 0), nir_channel(&b, s, 1),
                          nir_channel(&b, recovered, 0),
                          nir_channel(&b, recovered, 1)),
                 0xf);
   return b.shader;
}

struct fs_program {
   const char *option;
   const char *description;
   const char *guard;
   const char *macro_prefix;
   const char *symbol;
   nir_shader *(*build)(void);
   const uint32_t *golden;
   unsigned golden_size;
   uint32_t golden_fg_depth_src;
   uint32_t golden_us_out_w;
   /* When set the header also carries the program's ALU instruction
    * and temporary counts, the values a plan judges against its US
    * budget. */
   bool emit_us_budget;
   uint32_t golden_us_alu_instructions;
   uint32_t golden_us_temporaries;
};

static const struct fs_program fs_programs[] = {
   {
      .option = "triangle",
      .description = "Constant-color US block for the TCL-bypass triangle "
                     "cell",
      .guard = "R300_TCL_BYPASS_TRIANGLE_FS_BLOCK_H",
      .macro_prefix = "R300_TCL_BYPASS_TRIANGLE_FS",
      .symbol = "r300_tcl_bypass_triangle_fs_block",
      .build = build_constant_color_shader,
      .golden = r300_tcl_bypass_triangle_fs_block,
      .golden_size = ARRAY_SIZE(r300_tcl_bypass_triangle_fs_block),
      .golden_fg_depth_src = R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W,
   },
   {
      .option = "r2vb-producer",
      .description = "Varying-passthrough US block for the R2VB producer "
                     "pass",
      .guard = "R300_R2VB_PRODUCER_FS_BLOCK_H",
      .macro_prefix = "R300_R2VB_PRODUCER_FS",
      .symbol = "r300_r2vb_producer_fs_block",
      .build = build_varying_passthrough_shader,
      .golden = r300_r2vb_producer_fs_block,
      .golden_size = ARRAY_SIZE(r300_r2vb_producer_fs_block),
      .golden_fg_depth_src = R300_R2VB_PRODUCER_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_R2VB_PRODUCER_FS_US_OUT_W,
   },
   {
      .option = "sampled",
      .description = "Sampled-texture US block for the TCL-bypass sampling "
                     "cell",
      .guard = "R300_TCL_BYPASS_SAMPLED_FS_BLOCK_H",
      .macro_prefix = "R300_TCL_BYPASS_SAMPLED_FS",
      .symbol = "r300_tcl_bypass_sampled_fs_block",
      .build = build_sampled_texture_shader,
      .golden = r300_tcl_bypass_sampled_fs_block,
      .golden_size = ARRAY_SIZE(r300_tcl_bypass_sampled_fs_block),
      .golden_fg_depth_src = R300_TCL_BYPASS_SAMPLED_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_TCL_BYPASS_SAMPLED_FS_US_OUT_W,
   },
   {
      .option = "noperspective-reciprocal",
      .description = "Reciprocal-carrier US block for the NoPerspective "
                     "carrier cell",
      .guard = "R300_NOPERSPECTIVE_RECIPROCAL_FS_BLOCK_H",
      .macro_prefix = "R300_NOPERSPECTIVE_RECIPROCAL_FS",
      .symbol = "r300_noperspective_reciprocal_fs_block",
      .build = build_noperspective_reciprocal_shader,
      .golden = r300_noperspective_reciprocal_fs_block,
      .golden_size = ARRAY_SIZE(r300_noperspective_reciprocal_fs_block),
      .golden_fg_depth_src = R300_NOPERSPECTIVE_RECIPROCAL_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_NOPERSPECTIVE_RECIPROCAL_FS_US_OUT_W,
   },
   {
      .option = "noperspective-q-lane",
      .description = "Q-lane carrier US block for the NoPerspective q-lane "
                     "cell",
      .guard = "R300_NOPERSPECTIVE_Q_LANE_FS_BLOCK_H",
      .macro_prefix = "R300_NOPERSPECTIVE_Q_LANE_FS",
      .symbol = "r300_noperspective_q_lane_fs_block",
      .build = build_noperspective_q_lane_shader,
      .golden = r300_noperspective_q_lane_fs_block,
      .golden_size = ARRAY_SIZE(r300_noperspective_q_lane_fs_block),
      .golden_fg_depth_src = R300_NOPERSPECTIVE_Q_LANE_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_NOPERSPECTIVE_Q_LANE_FS_US_OUT_W,
   },
   {
      .option = "noperspective-mixed-carrier",
      .description = "Mixed Smooth/NoPerspective carrier US block for the "
                     "mixed reciprocal carrier cell",
      .guard = "R300_NOPERSPECTIVE_MIXED_CARRIER_FS_BLOCK_H",
      .macro_prefix = "R300_NOPERSPECTIVE_MIXED_CARRIER_FS",
      .symbol = "r300_noperspective_mixed_carrier_fs_block",
      .build = build_noperspective_mixed_carrier_shader,
      .golden = r300_noperspective_mixed_carrier_fs_block,
      .golden_size = ARRAY_SIZE(r300_noperspective_mixed_carrier_fs_block),
      .golden_fg_depth_src =
         R300_NOPERSPECTIVE_MIXED_CARRIER_FS_FG_DEPTH_SRC,
      .golden_us_out_w = R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_OUT_W,
      .emit_us_budget = true,
      .golden_us_alu_instructions =
         R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_ALU_INSTRUCTIONS,
      .golden_us_temporaries =
         R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_TEMPORARIES,
   },
};

static void
emit_header(const struct fs_program *program,
            const struct r300_fragment_shader_code *shader)
{
   printf("/*\n"
          " * SPDX-License-Identifier: MIT\n"
          " *\n"
          " * %s,\n"
          " * baked by r300_tcl_bypass_fs_tool --emit=%s; the paired\n"
          " * --check=%s meson test proves this file regenerates from\n"
          " * source.\n"
          " */\n\n",
          program->description, program->option, program->option);
   printf("#ifndef %s\n", program->guard);
   printf("#define %s\n\n", program->guard);
   printf("#include <stdint.h>\n\n");
   printf("#define %s_FG_DEPTH_SRC 0x%08xu\n", program->macro_prefix,
          shader->fg_depth_src);
   printf("#define %s_US_OUT_W 0x%08xu\n", program->macro_prefix,
          shader->us_out_w);
   if (program->emit_us_budget) {
      /* alu.length is the emitted ALU instruction count and pixsize the
       * highest temporary index (r300_fragprog_emit.c). */
      printf("#define %s_US_ALU_INSTRUCTIONS %uu\n", program->macro_prefix,
             shader->code.code.r300.alu.length);
      printf("#define %s_US_TEMPORARIES %uu\n", program->macro_prefix,
             shader->code.code.r300.pixsize + 1);
   }
   printf("\n");
   printf("static const uint32_t %s[] = {\n", program->symbol);
   for (unsigned i = 0; i < shader->cb_code_size; i++) {
      printf("%s0x%08x,%s", i % 4 == 0 ? "   " : " ",
             shader->cb_code[i], i % 4 == 3 ? "\n" : "");
   }
   if (shader->cb_code_size % 4 != 0)
      printf("\n");
   printf("};\n\n#endif /* %s */\n", program->guard);
}

/* Accepts the bare form for the triangle cell and the suffixed form for
 * either program, so the checked-in test invocations name their block.
 */
static const struct fs_program *
select_program(const char *argument, const char *verb, bool *selected)
{
   const size_t verb_length = strlen(verb);
   if (strncmp(argument, verb, verb_length) != 0)
      return NULL;
   if (argument[verb_length] == '\0') {
      *selected = true;
      return &fs_programs[0];
   }
   if (argument[verb_length] != '=')
      return NULL;
   const char *name = &argument[verb_length + 1];
   for (unsigned i = 0; i < ARRAY_SIZE(fs_programs); i++) {
      if (strcmp(fs_programs[i].option, name) == 0) {
         *selected = true;
         return &fs_programs[i];
      }
   }
   return NULL;
}

int
main(int argc, char **argv)
{
   bool emit = false;
   bool check = false;
   const struct fs_program *program = NULL;
   if (argc == 2) {
      program = select_program(argv[1], "--emit", &emit);
      if (program == NULL)
         program = select_program(argv[1], "--check", &check);
   }
   if (program == NULL) {
      fprintf(stderr, "usage: %s --emit[=PROGRAM]|--check[=PROGRAM]\n",
              argv[0]);
      for (unsigned i = 0; i < ARRAY_SIZE(fs_programs); i++)
         fprintf(stderr, "  PROGRAM %s\n", fs_programs[i].option);
      return 2;
   }

   struct r300_fragment_shader_code shader;
   if (compile_block(&shader, program->build) != 0)
      return 1;

   if (emit) {
      emit_header(program, &shader);
      return 0;
   }

   if (shader.cb_code_size != program->golden_size ||
       memcmp(shader.cb_code, program->golden,
              program->golden_size * sizeof(uint32_t)) != 0) {
      fprintf(stderr,
              "FAIL: regenerated %s block (%u dwords) differs from the "
              "checked-in golden (%u dwords); rerun --emit=%s and review\n",
              program->option, shader.cb_code_size, program->golden_size,
              program->option);
      return 1;
   }
   if (shader.fg_depth_src != program->golden_fg_depth_src ||
       shader.us_out_w != program->golden_us_out_w ||
       (program->emit_us_budget &&
        (shader.code.code.r300.alu.length !=
            program->golden_us_alu_instructions ||
         shader.code.code.r300.pixsize + 1 !=
            program->golden_us_temporaries))) {
      fprintf(stderr, "FAIL: %s depth-output metadata differs\n",
              program->option);
      return 1;
   }
   printf("r300_tcl_bypass_fs_tool --check=%s: golden block regenerates\n",
          program->option);
   return 0;
}
