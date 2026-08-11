/*
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * ICD direct-emit extraction API.
 * Implements r300_vs_get_hw_code() and r300_fs_get_hw_code() declared in
 * r300_public.h.  These are the only functions that legitimately access r300g
 * private types (r300_vertex_shader, r300_fragment_shader) from code that
 * will be called from outside the r300g translation unit (r3v ICD).
 * Keeping the casts here (inside r300g) ensures that any layout change to the
 * private structs produces a compile error at the definition site, not a
 * silent runtime misread at the call site in r3v.
 */

#include "r300_public.h"
#include "r300_vs.h"
#include "r300_fs.h"

bool
r300_vs_get_hw_code(void *vs_cso, struct r300_vs_hw_code *out)
{
   struct r300_vertex_shader *vs = (struct r300_vertex_shader *)vs_cso;
   if (!vs || !vs->shader || vs->shader->dummy)
      return false;

   /* SW-TCL path: draw_vs is non-NULL and shader->code.length is 0.
    * r300_parse_chipset in r300_chipset.c leaves num_vert_fpus at zero for
    * CHIP_RS480, and the same function derives caps.has_tcl from that count.
    * r300_context.c routes !has_tcl through Gallium Draw, so this guard keeps
    * the ICD from emitting a zero-length hardware VS program.  Symbol
    * discovery uses (rg --fixed-strings r300_parse_chipset
    * src/gallium/drivers/r300/; rg --fixed-strings has_tcl
    * src/gallium/drivers/r300/r300_context.c). */
   struct r300_vertex_program_code *code = &vs->shader->code;
   if (code->length == 0)
      return false;

   out->dwords           = code->body.d;
   out->length           = (unsigned)code->length;
   out->inputs_read      = code->InputsRead;
   out->outputs_written  = code->OutputsWritten;
   out->last_pos_write   = (int)code->last_pos_write;
   out->last_input_read  = (int)code->last_input_read;
   out->num_temporaries  = code->num_temporaries;
   out->pos_end          = code->pos_end;
   out->num_fc_ops       = code->num_fc_ops;
   out->fc_ops           = code->fc_ops;
   out->fc_op_addrs_r300 = code->fc_op_addrs.r300;
   out->fc_loop_index    = code->fc_loop_index;
   return true;
}

bool
r300_fs_get_hw_code(void *fs_cso, struct r300_fs_hw_code *out)
{
   struct r300_fragment_shader *fs = (struct r300_fragment_shader *)fs_cso;
   if (!fs || !fs->shader || fs->shader->dummy || !fs->shader->cb_code)
      return false;

   out->cb_code      = fs->shader->cb_code;
   out->cb_code_size = fs->shader->cb_code_size;
   out->fg_depth_src = fs->shader->fg_depth_src;
   out->us_out_w     = fs->shader->us_out_w;
   return true;
}
