/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Lower VS system values to synthetic vertex inputs for the SW-TCL route.
 *
 * RS480-family parts have no hardware vertex shader (num_vert_fpus = 0,
 * r300_chipset.c:131), so gl_VertexIndex / gl_InstanceIndex cannot be produced
 * by a PVS, and r300_nir_to_rc_direct rejects the intrinsics outright.  The
 * driver can instead supply the value as an ordinary per-vertex attribute
 * (firstVertex + i, or index-buffer value + vertexOffset) at a driver_location
 * it reserves; this pass rewrites the system-value intrinsic into a read of
 * that attribute.  After nir_lower_io lowers the synthetic variable, the value
 * arrives in RC input slot == driver_location, because r300_nir_to_rc_direct
 * keys load_input on nir_intrinsic_base.
 *
 * The pass runs only when the caller reserved a slot for a given system value
 * (slot >= 0).  When no slot is provided the intrinsic is left untouched, so
 * the deterministic r300_nir_to_rc_direct rejection still applies on paths that
 * cannot supply a synthetic attribute.
 */

#include "r300_nir.h"

#include "compiler/nir/nir_builder.h"

struct lower_vs_sysval_state {
   nir_variable *vertex_id_var;
   nir_variable *instance_id_var;
};

/* Classify an intrinsic as a VS system-value read, tolerant of both forms.
 * Returns the gl_system_value it reads, or SYSTEM_VALUE_MAX when it is not one
 * of the two the SW-TCL synthetic-attribute path handles.
 *
 * spirv_to_nir emits gl_VertexIndex / gl_InstanceIndex as a load_deref of a
 * nir_var_system_value variable (vtn_variables.c maps both to
 * SYSTEM_VALUE_VERTEX_ID / SYSTEM_VALUE_INSTANCE_ID).  nir_lower_system_values,
 * when a driver runs it, rewrites that to the load_vertex_id / load_instance_id
 * intrinsic.  The SW-TCL route runs neither nir_lower_system_values nor a
 * gather of system_values_read before lowering, so both forms must be matched
 * directly here. */
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
      if (sysval == SYSTEM_VALUE_VERTEX_ID ||
          sysval == SYSTEM_VALUE_INSTANCE_ID)
         return sysval;
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

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
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
   nir_variable *var =
      nir_variable_create(s, nir_var_shader_in, glsl_int_type(), name);
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
