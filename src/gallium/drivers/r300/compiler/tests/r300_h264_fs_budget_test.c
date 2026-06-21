/*
 * Copyright (c) 2026 Terascale Functionalists
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
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_regalloc.h"

#include "vl/vl_h264_chroma.h"
#include "vl/vl_h264_idct.h"
#include "vl/vl_h264_mc.h"

#define R300_FS_MAX_ALU 64
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

/* Run the production NIR optimization and the NIR-to-RC translation for a
 * fragment program against the R300-class limits. */
static void
run_fs(struct r300_fragment_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   struct r300_fragment_shader_code fs_code = {0};
   union r300_shader_code code = {
      .f = &fs_code,
   };

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
   c->Base.max_alu_insts = R300_FS_MAX_ALU;
   c->Base.max_tex_insts = R300_FS_MAX_TEX;

   r300_optimize_nir(nir, r300_screen(ps));
   nir_to_rc(nir, ps, ext, code, &c->Base);
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

   run_fs(&c, &rs, build(&fs_options));

   char label[128];
   snprintf(label, sizeof(label), "%s: nir_to_rc reports no error", name);
   CHECK(!c.Base.Error, label);
   if (c.Base.Error)
      printf("    error: %s\n", c.Base.ErrorMsg ? c.Base.ErrorMsg : "(none)");

   if (!c.Base.Error) {
      unsigned alu = 0, tex = 0;
      count_insts(&c.Base, &alu, &tex);
      printf("    %s: %u ALU / %u (<= %u) , %u TEX / %u (<= %u)\n",
             name, alu, R300_FS_MAX_ALU, R300_FS_MAX_ALU,
             tex, R300_FS_MAX_TEX, R300_FS_MAX_TEX);
      snprintf(label, sizeof(label), "%s: ALU %u <= %u", name, alu,
               R300_FS_MAX_ALU);
      CHECK(alu <= R300_FS_MAX_ALU, label);
      snprintf(label, sizeof(label), "%s: TEX %u <= %u", name, tex,
               R300_FS_MAX_TEX);
      CHECK(tex <= R300_FS_MAX_TEX, label);
   }

   teardown_fs(&c, &rs);
}

int
main(void)
{
   printf("r300-h264-fs-budget\n");
   gate_one("h264_idct_row", vl_h264_idct_row_nir);
   gate_one("h264_idct_col", vl_h264_idct_col_nir);
   gate_one("h264_mc_halfpel_h", vl_h264_mc_halfpel_h_nir);
   gate_one("h264_mc_halfpel_v", vl_h264_mc_halfpel_v_nir);
   gate_one("h264_chroma_bilinear", vl_h264_chroma_bilinear_nir);

   printf("r300-h264-fs-budget: %s\n", g_failures ? "FAIL" : "PASS");
   return g_failures ? 1 : 0;
}
