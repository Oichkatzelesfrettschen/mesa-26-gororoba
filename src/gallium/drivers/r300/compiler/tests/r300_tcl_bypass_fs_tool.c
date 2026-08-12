/*
 * SPDX-License-Identifier: MIT
 *
 * Compiles the TCL-bypass triangle cell's constant-color fragment
 * program through the classic ladder and the production cb_code baker,
 * so the cell's US block is compiler output instead of a hand-built
 * placeholder.  --emit prints the golden header; --check recompiles and
 * byte-compares against the checked-in golden, so the meson test proves
 * the checked-in block regenerates from source.
 */

#include <stdio.h>
#include <string.h>

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

#include "amd/r300/common/r300_tcl_bypass_triangle_fs_block.h"

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   /* Zeroed caps select the plain R300 US register emission, the register
    * set the RS480-family cell programs.
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

/* Runs the classic ladder plus the backend pass chain and bakes cb_code
 * through the driver's own r300_emit_fs_code_to_buffer.  Returns the baked
 * shader in *shader (cb_code owned by the caller) or nonzero on failure.
 */
static int
compile_block(struct r300_fragment_shader_code *shader)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *s = build_constant_color_shader();

   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

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

static void
emit_header(const struct r300_fragment_shader_code *shader)
{
   printf("/*\n"
          " * SPDX-License-Identifier: MIT\n"
          " *\n"
          " * Constant-color US block for the TCL-bypass triangle cell,\n"
          " * baked by r300_tcl_bypass_fs_tool --emit; the paired --check\n"
          " * meson test proves this file regenerates from source.\n"
          " */\n\n");
   printf("#ifndef R300_TCL_BYPASS_TRIANGLE_FS_BLOCK_H\n");
   printf("#define R300_TCL_BYPASS_TRIANGLE_FS_BLOCK_H\n\n");
   printf("#include <stdint.h>\n\n");
   printf("#define R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC 0x%08xu\n",
          shader->fg_depth_src);
   printf("#define R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W 0x%08xu\n\n",
          shader->us_out_w);
   printf("static const uint32_t r300_tcl_bypass_triangle_fs_block[] = {\n");
   for (unsigned i = 0; i < shader->cb_code_size; i++) {
      printf("%s0x%08x,%s", i % 4 == 0 ? "   " : " ",
             shader->cb_code[i], i % 4 == 3 ? "\n" : "");
   }
   if (shader->cb_code_size % 4 != 0)
      printf("\n");
   printf("};\n\n#endif /* R300_TCL_BYPASS_TRIANGLE_FS_BLOCK_H */\n");
}

int
main(int argc, char **argv)
{
   const bool check = argc == 2 && strcmp(argv[1], "--check") == 0;
   const bool emit = argc == 2 && strcmp(argv[1], "--emit") == 0;
   if (!check && !emit) {
      fprintf(stderr, "usage: %s --emit|--check\n", argv[0]);
      return 2;
   }

   struct r300_fragment_shader_code shader;
   if (compile_block(&shader) != 0)
      return 1;

   if (emit) {
      emit_header(&shader);
      return 0;
   }

   const unsigned golden_size =
      sizeof(r300_tcl_bypass_triangle_fs_block) /
      sizeof(r300_tcl_bypass_triangle_fs_block[0]);
   if (shader.cb_code_size != golden_size ||
       memcmp(shader.cb_code, r300_tcl_bypass_triangle_fs_block,
              golden_size * sizeof(uint32_t)) != 0) {
      fprintf(stderr,
              "FAIL: regenerated block (%u dwords) differs from the "
              "checked-in golden (%u dwords); rerun --emit and review\n",
              shader.cb_code_size, golden_size);
      return 1;
   }
   if (shader.fg_depth_src != R300_TCL_BYPASS_TRIANGLE_FS_FG_DEPTH_SRC ||
       shader.us_out_w != R300_TCL_BYPASS_TRIANGLE_FS_US_OUT_W) {
      fprintf(stderr, "FAIL: depth-output metadata differs\n");
      return 1;
   }
   printf("r300_tcl_bypass_fs_tool --check: golden block regenerates\n");
   return 0;
}
