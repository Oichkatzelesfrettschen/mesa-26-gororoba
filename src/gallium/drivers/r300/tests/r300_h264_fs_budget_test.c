/*
 * SPDX-License-Identifier: MIT
 */

/*
 * R300 compile-budget gate for the H.264 back-half fragment programs.
 *
 * vl_h264_idct.c and vl_h264_mc.c build their kernels with the shared vl_nir
 * helpers and verify their arithmetic on a software rasterizer.  Softpipe lowers
 * NIR to TGSI, so that verification does not exercise the r300 nir_to_rc path or
 * the R300 fragment limits (64 ALU, 32 TEX, 32 temp).  This gate builds the same
 * kernels as raw NIR (the *_nir entry points), runs the production
 * r300_optimize_nir plus nir_to_rc against a stack-allocated is_r500=false
 * screen, and checks two things: the translation reports no error (every op the
 * kernels emit -- bcsel, ffract, fmin/fmax, the multiply-adds, the texture
 * fetches -- is one nir_to_rc lowers for R300), and the emitted RC instruction
 * counts sit under the R300 limits.  The raw nir_to_rc count is taken before
 * the backend pairs RGB and alpha ops into instruction slots, so it is an upper
 * bound on the scheduled count: a count under the limit here is a conservative
 * proof the kernel fits.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_regalloc.h"

#include "vl/vl_h264_chroma.h"
#include "vl/vl_h264_deblock.h"
#include "vl/vl_h264_idct.h"
#include "vl/vl_h264_mc.h"
#include "vl/vl_h264_reconstruct.h"

#define R300_FS_MAX_ALU 64  /* proven non-HB R300 fragment ALU envelope */
#define R300_FS_MAX_TEX 32

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

/* The r300_screen.c COMMON_NIR_OPTIONS fields these kernels depend on.
 * lower_ffloor and lower_ftrunc are load-bearing: the FRC-floor shift builds
 * x - ffract(x), which nir_opt_algebraic folds back into ffloor unless ffloor
 * is marked for lowering -- and nir_to_rc has no ffloor, so the fold would make
 * the kernel fail to translate.  has_fmad keeps a multiply-add from splitting;
 * lower_flrp32 matches the production fragment options. */
static const nir_shader_compiler_options fs_options = {
   .float_mul_add32 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .lower_flrp32 = true,
   .lower_ffloor = true,
   .lower_ftrunc = true,
};

static void count_insts(struct radeon_compiler *c, unsigned *alu, unsigned *tex);

/* The full backend copies constants and allocates the packed fragment code
 * into the output object.  This test keeps that object on the stack, so it
 * releases every heap-owned field before the stack lifetime ends. */
static void
destroy_fs_code(struct r300_fragment_shader_code *fs_code)
{
   free(fs_code->code.constants_remap_table);
   rc_constants_destroy(&fs_code->code.constants);
   free(fs_code->cb_code);
   free(fs_code->error);
}

/* An is_r500=false screen so the lowering takes the R300-class fragment path. */
static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   s->caps.is_r500 = false;
   s->caps.is_r400 = false;
   return (struct pipe_screen *)s;
}

/* Assign the fragment inputs nir_to_rc recorded to sequential hardware
 * registers in color, face, generic, fog, and window-position order. */
static void
gate_allocate_inputs(struct r300_fragment_program_compiler *c,
                     void (*allocate)(void *data, unsigned input,
                                      unsigned hwreg),
                     void *mydata)
{
   const struct r300_shader_semantics *inputs = c->UserData;
   int reg = 0;
   for (int i = 0; i < ATTR_COLOR_COUNT; i++)
      if (inputs->color[i] != ATTR_UNUSED)
         allocate(mydata, inputs->color[i], reg++);
   if (inputs->face != ATTR_UNUSED)
      allocate(mydata, inputs->face, reg++);
   for (int i = 0; i < ATTR_GENERIC_COUNT; i++)
      if (inputs->generic[i] != ATTR_UNUSED)
         allocate(mydata, inputs->generic[i], reg++);
   if (inputs->fog != ATTR_UNUSED)
      allocate(mydata, inputs->fog, reg++);
   if (inputs->wpos != ATTR_UNUSED)
      allocate(mydata, inputs->wpos, reg++);
}

/* Run the production NIR optimization, the NIR-to-RC translation, and -- since
 * the R300 budget is enforced by the RC backend's scheduling, not by nir_to_rc
 * -- the full r3xx_compile_fragment_program against max_alu.  c->Base.Error
 * after the full compile is the decisive fit verdict.  raw_alu/raw_tex report
 * the pre-scheduling instruction count (an upper bound) for context.  The
 * kernels use no wpos or face input, so the wpos/face transforms in
 * r300_translate_fragment_shader are skipped. */
static void
run_fs(struct r300_fragment_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir, unsigned max_alu, unsigned *raw_alu, unsigned *raw_tex,
       unsigned *sched_alu)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   struct r300_fragment_shader_code fs_code = {0};
   union r300_shader_code code = {
      .f = &fs_code,
   };
   /* nir_to_rc records the used fragment inputs into fs_code.inputs; the full
    * compile's AllocateHwInputs callback reads them back, so reset them to
    * ATTR_UNUSED (the zero-init is the valid attr index 0, not "unused"). */
   r300_shader_semantics_reset(&fs_code.inputs);

   rc_init_regalloc_state(rs, RC_FRAGMENT_PROGRAM);
   memset(c, 0, sizeof(*c));
   rc_init(&c->Base, rs);
   c->Base.type = RC_FRAGMENT_PROGRAM;
   c->Base.is_r400 = false;
   c->Base.is_r500 = false;
   c->Base.has_half_swizzles = true;
   c->Base.has_presub = true;
   c->Base.has_omod = true;
   c->Base.max_temp_regs = 32;
   c->Base.max_constants = 32;
   c->Base.max_alu_insts = max_alu;
   c->Base.max_tex_insts = R300_FS_MAX_TEX;
   c->code = &fs_code.code;
   c->AllocateHwInputs = gate_allocate_inputs;
   c->UserData = &fs_code.inputs;

   *raw_alu = 0;
   *raw_tex = 0;
   *sched_alu = 0;

   r300_optimize_nir(nir, &r300_screen(ps)->caps);
   nir_to_rc(nir, &r300_screen(ps)->caps, ext, code, &c->Base);
   if (!c->Base.Error) {
      count_insts(&c->Base, raw_alu, raw_tex);
      c->Base.remove_unused_constants = true;
      r3xx_compile_fragment_program(c);
      if (!c->Base.Error)
         *sched_alu = fs_code.code.code.r300.alu.length;
   }
   destroy_fs_code(&fs_code);
}

static void
teardown_fs(struct r300_fragment_program_compiler *c,
            struct rc_regalloc_state *rs)
{
   rc_destroy(&c->Base);
   rc_destroy_regalloc_state(rs);
}

/* Texture instructions (HasTexture) and ALU instructions (everything else that
 * writes a register and is not flow control). */
static void
count_insts(struct radeon_compiler *c, unsigned *alu, unsigned *tex)
{
   *alu = 0;
   *tex = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      if (info->HasTexture)
         (*tex)++;
      else if (info->HasDstReg && !info->IsFlowControl)
         (*alu)++;
   }
}

static void
gate_one(const char *name,
         nir_shader *(*build)(const nir_shader_compiler_options *))
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   unsigned raw_alu = 0, raw_tex = 0, sched_alu = 0;

   run_fs(&c, &rs, build(&fs_options), R300_FS_MAX_ALU, &raw_alu, &raw_tex,
          &sched_alu);

   char label[160];
   printf("    %s: %u scheduled ALU / %u (%u raw), %u TEX / %u -> %s\n", name,
          sched_alu, R300_FS_MAX_ALU, raw_alu, raw_tex, R300_FS_MAX_TEX,
          c.Base.Error ? "DOES NOT FIT" : "fits");
   if (c.Base.Error && c.Base.ErrorMsg)
      printf("      %s\n", c.Base.ErrorMsg);
   snprintf(label, sizeof(label),
            "%s: translates and schedules within %u ALU / %u TEX", name,
            R300_FS_MAX_ALU, R300_FS_MAX_TEX);
   CHECK(!c.Base.Error, label);
   snprintf(label, sizeof(label), "%s: TEX %u <= %u", name, raw_tex,
            R300_FS_MAX_TEX);
   CHECK(raw_tex <= R300_FS_MAX_TEX, label);

   teardown_fs(&c, &rs);
}

int
main(void)
{
   printf("r300-h264-fs-budget\n");
   /* Each kernel translates through nir_to_rc and schedules within this
    * gate's R300 limits (64 ALU / 32 TEX).  RC backend scheduling supplies
    * the ALU-fit verdict; raw nir_to_rc counts provide context. */
   gate_one("h264_idct_row", vl_h264_idct_row_nir);
   gate_one("h264_idct_col", vl_h264_idct_col_nir);
   gate_one("h264_idct_plane_row", vl_h264_idct_plane_row_nir);
   gate_one("h264_idct_plane_col", vl_h264_idct_plane_col_nir);
   gate_one("h264_mc_halfpel_h", vl_h264_mc_halfpel_h_nir);
   gate_one("h264_mc_halfpel_v", vl_h264_mc_halfpel_v_nir);
   gate_one("h264_mc_qpel_axis", vl_h264_mc_qpel_axis_nir);
   gate_one("h264_mc_qpel_diag", vl_h264_mc_qpel_diag_nir);
   gate_one("h264_chroma_bilinear", vl_h264_chroma_bilinear_nir);
   gate_one("h264_deblock_luma", vl_h264_deblock_luma_nir);
   gate_one("h264_deblock_apply_v", vl_h264_deblock_apply_v_nir);
   gate_one("h264_deblock_apply_h", vl_h264_deblock_apply_h_nir);
   gate_one("h264_deblock_strong_vp", vl_h264_deblock_strong_vp_nir);
   gate_one("h264_deblock_strong_vq", vl_h264_deblock_strong_vq_nir);
   gate_one("h264_deblock_strong_hp", vl_h264_deblock_strong_hp_nir);
   gate_one("h264_deblock_strong_hq", vl_h264_deblock_strong_hq_nir);
   gate_one("h264_deblock_chroma_v", vl_h264_deblock_chroma_v_nir);
   gate_one("h264_deblock_chroma_h", vl_h264_deblock_chroma_h_nir);
   gate_one("h264_deblock_chroma_strong_v", vl_h264_deblock_chroma_strong_v_nir);
   gate_one("h264_deblock_chroma_strong_h", vl_h264_deblock_chroma_strong_h_nir);
   gate_one("h264_reconstruct", vl_h264_reconstruct_nir);

   printf("r300-h264-fs-budget: %s\n", g_failures ? "FAIL" : "PASS");
   return g_failures ? 1 : 0;
}
