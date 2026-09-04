/*
 * SPDX-License-Identifier: MIT
 *
 * Lower VS system values to synthetic vertex inputs.
 *
 * RS480-family parts keep num_vert_fpus = 0 and has_tcl = false, so r300g
 * ordinary draws use the Gallium Draw SW-TCL path instead of the hardware
 * VAP/PVS vertex-shader route.  gl_VertexIndex / gl_InstanceIndex therefore
 * cannot be produced by PVS microcode, and the NIR-to-RC translator rejects
 * unsupported VS system-value intrinsics outright.  The caller can instead
 * supply the value as an ordinary per-vertex attribute (firstVertex + i, or
 * index-buffer value + vertexOffset) at a reserved driver_location; this pass
 * rewrites the system-value intrinsic into a read of that attribute.  After
 * nir_lower_io lowers the synthetic variable, ntr_emit_load_input reads from
 * RC input slot == driver_location because it keys load_input on
 * nir_intrinsic_base.
 *
 * The pass runs only when the caller reserved a slot for a given system value
 * (slot >= 0).  When no slot is provided the intrinsic is left untouched, so
 * the deterministic NIR-to-RC rejection still applies on paths that cannot
 * supply a synthetic attribute.
 */

#include "r300_nir.h"

#include "compiler/nir/nir_builder.h"

struct lower_vs_sysval_state {
   nir_variable *vertex_id_var;
   nir_variable *instance_id_var;
};

/* Classify an intrinsic as a VS system-value read, tolerant of both forms.
 * Returns the normalized gl_system_value consumed by this pass, or
 * SYSTEM_VALUE_MAX when it is not one of the two synthetic-attribute paths.
 *
 * spirv_to_nir emits gl_VertexIndex / gl_InstanceIndex as a load_deref of a
 * nir_var_system_value variable (vtn_variables.c maps gl_VertexIndex to
 * SYSTEM_VALUE_VERTEX_ID and gl_InstanceIndex to SYSTEM_VALUE_INSTANCE_INDEX --
 * note INSTANCE_INDEX, not INSTANCE_ID).  nir_lower_system_values,
 * when a driver runs it, rewrites that to the load_vertex_id / load_instance_id
 * intrinsic.  R3V runs neither nir_lower_system_values nor a gather of
 * system_values_read before lowering, so both forms must be matched directly
 * here. */
static gl_system_value
vs_sysval_of_intrinsic(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_vertex_id:
      return SYSTEM_VALUE_VERTEX_ID;
   case nir_intrinsic_load_instance_id:
      return SYSTEM_VALUE_INSTANCE_ID;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      if (!deref || !nir_deref_mode_is(deref, nir_var_system_value))
         return SYSTEM_VALUE_MAX;
      nir_variable *sv = nir_deref_instr_get_variable(deref);
      if (!sv)
         return SYSTEM_VALUE_MAX;
      gl_system_value sysval = (gl_system_value)sv->data.location;
      if (sysval == SYSTEM_VALUE_VERTEX_ID)
         return SYSTEM_VALUE_VERTEX_ID;
      /* Vulkan gl_InstanceIndex is SYSTEM_VALUE_INSTANCE_INDEX (the instance
       * number plus firstInstance), not SYSTEM_VALUE_INSTANCE_ID.  The synthetic
       * attribute carries that full value, so normalize it to the instance
       * synthetic.  Matching only INSTANCE_ID misses every Vulkan VS that reads
       * gl_InstanceIndex and leaves a system-value deref outside the direct
       * Draw executor's admitted intrinsic set. */
      if (sysval == SYSTEM_VALUE_INSTANCE_INDEX ||
          sysval == SYSTEM_VALUE_INSTANCE_ID)
         return SYSTEM_VALUE_INSTANCE_ID;
      return SYSTEM_VALUE_MAX;
   }
   default:
      return SYSTEM_VALUE_MAX;
   }
}

/* Map a system value to the reserved synthetic input variable, or NULL. */
static nir_variable *
sysval_to_synthetic(const struct lower_vs_sysval_state *st,
                    gl_system_value sysval)
{
   switch (sysval) {
   case SYSTEM_VALUE_VERTEX_ID:
      return st->vertex_id_var;
   case SYSTEM_VALUE_INSTANCE_ID:
      return st->instance_id_var;
   default:
      return NULL;
   }
}

void
r300_nir_vs_reads_system_values(nir_shader *s, bool *reads_vertex_id,
                                bool *reads_instance_id)
{
   *reads_vertex_id = false;
   *reads_instance_id = false;
   if (s->info.stage != MESA_SHADER_VERTEX)
      return;

   nir_function_impl *entrypoint = nir_shader_get_entrypoint(s);
   nir_foreach_block (block, entrypoint) {
      nir_foreach_instr (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         switch (vs_sysval_of_intrinsic(nir_instr_as_intrinsic(instr))) {
         case SYSTEM_VALUE_VERTEX_ID:
            *reads_vertex_id = true;
            break;
         case SYSTEM_VALUE_INSTANCE_ID:
            *reads_instance_id = true;
            break;
         default:
            break;
         }
      }
   }
}

static bool
lower_vs_sysval(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct lower_vs_sysval_state *st = data;

   nir_variable *var = sysval_to_synthetic(st, vs_sysval_of_intrinsic(intr));
   if (!var)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *value = nir_load_var(b, var);
   nir_def_rewrite_uses(&intr->def, value);
   nir_instr_remove(&intr->instr);
   return true;
}

static nir_variable *
make_sysval_input(nir_shader *s, int slot, const char *name)
{
   if (slot < 0)
      return NULL;
   /* The r3v vertex-buffer binding for this stream carries
    * PIPE_FORMAT_R32_FLOAT (r3v_build_velems_cso), with the identity value
    * written as a genuine float (r3v_bind_synthetic_identity_stream) so it
    * survives the LLVM draw-JIT's float-typed vertex-fetch register without
    * the pure_integer bitcast in lp_build_fetch_rgba_aos_array reinterpreting
    * a small index as a subnormal.  Declare the input variable float here to
    * match: the SW-TCL vertex shader already runs in nir_lower_int_to_float's
    * float-domain model (r300_vs_draw.c), so a genuinely float-typed input
    * needs no further int-to-float encoding at its use sites. */
   nir_variable *var =
      nir_variable_create(s, nir_var_shader_in, glsl_float_type(), name);
   var->data.location = VERT_ATTRIB_GENERIC0 + slot;
   var->data.driver_location = slot;
   return var;
}

bool
r300_nir_lower_vs_system_values_to_inputs(nir_shader *s, int vertex_id_slot,
                                          int instance_id_slot)
{
   if (s->info.stage != MESA_SHADER_VERTEX)
      return false;
   if (vertex_id_slot < 0 && instance_id_slot < 0)
      return false;

   struct lower_vs_sysval_state st = {
      .vertex_id_var = make_sysval_input(s, vertex_id_slot, "sys_vertex_index"),
      .instance_id_var = make_sysval_input(s, instance_id_slot, "sys_instance_index"),
   };

   bool progress = nir_shader_intrinsics_pass(s, lower_vs_sysval,
                                              nir_metadata_control_flow, &st);
   if (progress) {
      /* The rewrite removed the load_deref reads, but the deref_var feeding each
       * one survives as a dead instruction that still references the
       * nir_var_system_value variable.  nir_remove_dead_variables counts that
       * deref as a use and would keep the variable, so DCE the dead derefs
       * first.  Then drop the now-unreferenced variables and re-gather info so
       * system_values_read no longer reports the lowered values downstream. */
      nir_opt_dce(s);
      nir_remove_dead_variables(s, nir_var_system_value, NULL);
      nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
   }
   return progress;
}

/* Rewrite the spirv_to_nir system-value deref into the native intrinsic the SW
 * Draw executor supplies.  It fills the vertex ID from
 * the vsplit frontend's fetch_elts (an indexed draw's raw index plus eltBias,
 * draw_pt_vsplit.c's vsplit_add_cache_uint32) or i + start_index for a
 * non-indexed draw. Emitting the intrinsic instead of a
 * synthetic vertex input consumes no vertex element, so a shader reading
 * VertexID alongside the full maxVertexInputAttributes set still fits the
 * 16-element PSC budget.
 *
 * load_vertex_id already matches Vulkan's base-inclusive gl_VertexIndex for
 * both indexed and non-indexed draws at any firstVertex/vertexOffset -- GL's
 * gl_VertexID carries the same base-inclusive contract, and draw_vs_exec
 * supplies one value for both.  load_instance_id is the GL-defined
 * gl_InstanceID (TGSI_SEMANTIC_INSTANCEID's documented contract in
 * p_shader_tokens.h: "doesn't include start_instance"), so Vulkan's
 * base-inclusive gl_InstanceIndex needs the firstInstance addend the draw
 * module tracks separately as TGSI_SEMANTIC_BASEINSTANCE
 * (draw_vs_exec.c, shader->draw->start_instance). */
static bool
lower_vs_sysval_to_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                             void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_deref)
      return false;

   nir_def *value;
   switch (vs_sysval_of_intrinsic(intr)) {
   case SYSTEM_VALUE_VERTEX_ID:
      b->cursor = nir_before_instr(&intr->instr);
      value = nir_load_vertex_id(b);
      break;
   case SYSTEM_VALUE_INSTANCE_ID:
      b->cursor = nir_before_instr(&intr->instr);
      value = nir_iadd(b, nir_load_instance_id(b), nir_load_base_instance(b));
      break;
   default:
      return false;
   }

   nir_def_rewrite_uses(&intr->def, value);
   nir_instr_remove(&intr->instr);
   return true;
}

bool
r300_nir_lower_vs_system_values_to_intrinsics(nir_shader *s)
{
   if (s->info.stage != MESA_SHADER_VERTEX)
      return false;

   bool progress = nir_shader_intrinsics_pass(s, lower_vs_sysval_to_intrinsic,
                                              nir_metadata_control_flow, NULL);
   if (progress) {
      /* The rewrite leaves the deref_var feeding each load_deref dead; DCE it
       * before dropping the nir_var_system_value declarations, then re-gather
       * so system_values_read reflects the native intrinsics downstream. */
      nir_opt_dce(s);
      nir_remove_dead_variables(s, nir_var_system_value, NULL);
      nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
   }
   return progress;
}
