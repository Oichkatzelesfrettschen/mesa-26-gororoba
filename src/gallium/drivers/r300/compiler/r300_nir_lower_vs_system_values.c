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
