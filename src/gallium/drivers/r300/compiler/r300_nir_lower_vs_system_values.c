/*
 * SPDX-License-Identifier: MIT
 *
 * Lower VS system values to synthetic vertex inputs.
 *
 * The RC vertex path has no system-value input slot.  A caller that reserves
 * an ordinary vertex attribute slot can supply VertexIndex or InstanceIndex as
 * per-vertex data and run this pass before nir_to_rc().  After nir_lower_io
 * lowers the synthetic variable, ntr_emit_load_input reads from RC input
 * slot == driver_location because it keys load_input on nir_intrinsic_base.
 *
 * The pass runs only when the caller reserved a slot for a given system value
 * (slot >= 0).  Without a reserved slot, the intrinsic remains in the shader
 * so nir_to_rc reports a deterministic compiler error instead of aliasing
 * user attribute 0.
 */

#include "r300_nir.h"

#include "compiler/nir/nir_builder.h"

struct lower_vs_sysval_state {
   nir_variable *vertex_id_var;
   nir_variable *instance_id_var;
};

static bool
lower_vs_sysval(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct lower_vs_sysval_state *st = data;
   nir_variable *var;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_vertex_id:
      var = st->vertex_id_var;
      break;
   case nir_intrinsic_load_instance_id:
      var = st->instance_id_var;
      break;
   default:
      return false;
   }
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

   return nir_shader_intrinsics_pass(s, lower_vs_sysval,
                                     nir_metadata_control_flow, &st);
}
