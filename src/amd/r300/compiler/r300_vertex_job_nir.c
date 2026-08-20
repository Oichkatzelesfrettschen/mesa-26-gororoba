/*
 * SPDX-License-Identifier: MIT
 *
 * Semantic vertex front end over NIR for the CPU vertex-job IR.
 */

#include "r300_vertex_job_nir.h"

#include "compiler/spirv/spirv_info.h"

#include <string.h>

const nir_shader_compiler_options *r300_vertex_job_nir_options(void)
{
   /* The analyzers admit straight-line vec4 code, so the default
    * (vector-preserving, no lowering flags) option set is the one the
    * ingestion needs. */
   static const nir_shader_compiler_options options = { 0 };
   return &options;
}

const struct spirv_to_nir_options *r300_vertex_job_spirv_options(void)
{
   static const struct spirv_capabilities capabilities = {
      .Matrix = true,
      .Shader = true,
   };
   static const struct spirv_to_nir_options options = {
      .environment = NIR_SPIRV_VULKAN,
      .capabilities = &capabilities,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,
   };
   return &options;
}

struct spirv_translation_state {
   bool warned;
};

static void
record_spirv_diagnostic(void *private_data, enum nir_spirv_debug_level level,
                        size_t spirv_offset, const char *message)
{
   (void)spirv_offset;
   (void)message;
   struct spirv_translation_state *state = private_data;
   if (level >= NIR_SPIRV_DEBUG_LEVEL_WARNING)
      state->warned = true;
}

nir_shader *
r300_vertex_job_spirv_to_nir(const uint32_t *words, size_t word_count,
                             mesa_shader_stage stage,
                             const char *entry_point_name)
{
   struct spirv_translation_state state = {0};
   struct spirv_to_nir_options options =
      *r300_vertex_job_spirv_options();
   options.debug.func = record_spirv_diagnostic;
   options.debug.private_data = &state;
   nir_shader *nir = spirv_to_nir(words, word_count, NULL, stage,
                                  entry_point_name, &options,
                                  r300_vertex_job_nir_options());
   if (state.warned) {
      ralloc_free(nir);
      return NULL;
   }
   return nir;
}

static bool
float_control_modes_supported(const nir_shader *nir, const char **reason)
{
   if (nir->info.float_controls_execution_mode !=
       FLOAT_CONTROLS_DEFAULT_FLOAT_CONTROL_MODE) {
      *reason = "float-control execution mode outside the admitted policy";
      return false;
   }
   return true;
}

static unsigned vec4_slots(const struct glsl_type *type, bool bindless)
{
   (void)bindless;
   return glsl_count_attribute_slots(type, false);
}

bool r300_vertex_job_nir_normalize(nir_shader *nir, const char **reason)
{
   if (nir->info.stage != MESA_SHADER_VERTEX &&
       nir->info.stage != MESA_SHADER_FRAGMENT) {
      *reason = "stage outside the vertex/fragment cell";
      return false;
   }
   if (!float_control_modes_supported(nir, reason))
      return false;

   /* The vtn postlude: one entry point of straight-line code with
    * variables promoted to SSA. */
   NIR_PASS(_, nir, nir_lower_variable_initializers,
            nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~(nir_variable_mode)0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   /* Driver locations: vertex inputs by generic attribute index, the
    * position or color-0 output at zero.  Every other output slot gets
    * a distinct nonzero location the analyzers refuse at its store. */
   uint32_t next_extra_output = 1;
   nir_foreach_shader_in_variable(var, nir) {
      if (nir->info.stage != MESA_SHADER_VERTEX ||
          var->data.location < VERT_ATTRIB_GENERIC0) {
         *reason = "non-attribute shader input";
         return false;
      }
      var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
   }
   nir_foreach_shader_out_variable(var, nir) {
      const bool primary =
         (nir->info.stage == MESA_SHADER_VERTEX &&
          var->data.location == VARYING_SLOT_POS) ||
         (nir->info.stage == MESA_SHADER_FRAGMENT &&
          var->data.location == FRAG_RESULT_DATA0);
      var->data.driver_location = primary ? 0 : next_extra_output++;
   }

   NIR_PASS(_, nir, nir_lower_io,
            nir_var_shader_in | nir_var_shader_out, vec4_slots,
            (nir_lower_io_options)0);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
   return true;
}

/* SSA-to-job binding: each admitted def maps to a temp register;
 * a DP4 result is a broadcast scalar rejoined only by its own
 * replicate. */
struct ssa_binding {
   bool bound;
   bool scalar_broadcast;
   uint8_t temp;
};

struct job_build {
   struct r300_vertex_job *job;
   struct ssa_binding *bindings;
   uint32_t ssa_capacity;
   uint32_t next_temp;
   bool stored;
   const char **reason;
};

static bool refuse(struct job_build *b, const char *why)
{
   *b->reason = why;
   return false;
}

static struct ssa_binding *binding_of(struct job_build *b,
                                      const nir_def *def)
{
   return def->index < b->ssa_capacity ? &b->bindings[def->index] : NULL;
}

static bool bind_new_temp(struct job_build *b, const nir_def *def,
                          bool scalar_broadcast, uint8_t *temp)
{
   struct ssa_binding *binding = binding_of(b, def);
   if (!binding)
      return refuse(b, "SSA index outside the analysis capacity");
   if (b->next_temp >= R300_VERTEX_JOB_MAX_TEMPS)
      return refuse(b, "vertex program exceeds the 16-temp file");
   *temp = (uint8_t)b->next_temp++;
   binding->bound = true;
   binding->scalar_broadcast = scalar_broadcast;
   binding->temp = *temp;
   return true;
}

static bool emit(struct job_build *b, uint8_t opcode, uint8_t dst,
                 uint8_t src0, uint8_t src1, uint8_t src2)
{
   struct r300_vertex_job *job = b->job;
   if (job->instruction_count >= R300_VERTEX_JOB_MAX_INSTRUCTIONS)
      return refuse(b, "vertex program exceeds the instruction budget");
   job->instructions[job->instruction_count++] =
      (struct r300_vertex_job_instruction){ opcode, dst, src0, src1, src2 };
   return true;
}

/* Resolves an ALU source to its temp: a vec4 binding takes the
 * identity swizzle, a broadcast scalar takes the all-x swizzle. */
static bool alu_src_temp(struct job_build *b, const nir_alu_src *src,
                         uint32_t components, uint8_t *temp)
{
   const struct ssa_binding *binding = binding_of(b, src->src.ssa);
   if (!binding || !binding->bound)
      return refuse(b, "operand outside the admitted straight line");
   for (uint32_t c = 0; c < components; c++) {
      const uint8_t lane = binding->scalar_broadcast ? 0 : (uint8_t)c;
      if (src->swizzle[c] != lane)
         return refuse(b, "non-identity swizzle");
   }
   *temp = binding->temp;
   return true;
}

static bool build_alu(struct job_build *b, const nir_alu_instr *alu)
{
   if (alu->fp_math_ctrl & nir_fp_preserve_sz_inf_nan)
      return refuse(b, "float preservation mode outside the admitted policy");

   const nir_def *def = &alu->def;
   if (def->bit_size != 32)
      return refuse(b, "non-32-bit arithmetic");

   uint8_t srcs[3] = { 0, 0, 0 };
   uint8_t opcode;
   uint32_t operand_count;
   uint32_t operand_components = 4;
   bool scalar_result = false;

   switch (alu->op) {
   case nir_op_mov: {
      if (def->num_components != 4)
         return refuse(b, "non-vec4 move");
      if (!alu_src_temp(b, &alu->src[0], 4, &srcs[0]))
         return false;
      /* A move aliases: the def shares the source temp and no
       * instruction is emitted, because the job file has no
       * observable register identity. */
      struct ssa_binding *binding = binding_of(b, def);
      if (!binding)
         return refuse(b, "SSA index outside the analysis capacity");
      binding->bound = true;
      binding->scalar_broadcast = false;
      binding->temp = srcs[0];
      return true;
   }
   case nir_op_fadd:
      opcode = R300_VERTEX_JOB_OP_FADD;
      operand_count = 2;
      break;
   case nir_op_fmul:
      opcode = R300_VERTEX_JOB_OP_FMUL;
      operand_count = 2;
      break;
   case nir_op_fmad:
      opcode = R300_VERTEX_JOB_OP_FMAD;
      operand_count = 3;
      break;
   case nir_op_ffma_weak:
      /* Constant folding evaluates weak FMA with fused semantics.  Precise
       * weak FMA therefore selects FFMA everywhere, while inexact weak FMA
       * retains its permitted two-rounding selection.
       */
      opcode = nir_alu_instr_is_exact(alu) ? R300_VERTEX_JOB_OP_FFMA
                                           : R300_VERTEX_JOB_OP_FMAD;
      operand_count = 3;
      break;
   case nir_op_ffma:
      opcode = R300_VERTEX_JOB_OP_FFMA;
      operand_count = 3;
      break;
   case nir_op_fdot4:
      opcode = R300_VERTEX_JOB_OP_DP4;
      operand_count = 2;
      scalar_result = true;
      break;
   case nir_op_vec4: {
      /* The one admitted vec4 construction is a broadcast scalar's
       * own replicate, which the DP4 temp already carries. */
      const struct ssa_binding *binding =
         binding_of(b, alu->src[0].src.ssa);
      if (!binding || !binding->bound || !binding->scalar_broadcast)
         return refuse(b, "vec4 construction outside a DP4 broadcast");
      for (uint32_t s = 0; s < 4; s++) {
         if (alu->src[s].src.ssa != alu->src[0].src.ssa ||
             alu->src[s].swizzle[0] != 0)
            return refuse(b, "vec4 construction outside a DP4 broadcast");
      }
      struct ssa_binding *dest = binding_of(b, def);
      if (!dest)
         return refuse(b, "SSA index outside the analysis capacity");
      dest->bound = true;
      dest->scalar_broadcast = false;
      dest->temp = binding->temp;
      return true;
   }
   default:
      return refuse(b, "arithmetic outside MOV/FADD/FMUL/FMAD/FFMA/DP4");
   }

   if (scalar_result ? def->num_components != 1
                     : def->num_components != 4)
      return refuse(b, "arithmetic width outside the vec4 model");
   for (uint32_t s = 0; s < operand_count; s++) {
      if (!alu_src_temp(b, &alu->src[s], operand_components, &srcs[s]))
         return false;
   }
   uint8_t dst;
   if (!bind_new_temp(b, def, scalar_result, &dst))
      return false;
   return emit(b, opcode, dst, srcs[0], srcs[1], srcs[2]);
}

static bool build_load_const(struct job_build *b,
                             const nir_load_const_instr *instr)
{
   /* A non-vec4 constant stays unbound: the IO offset immediates are
    * scalar load_consts consumed through nir_src_is_const, and an
    * arithmetic use of an unbound def refuses at the operand. */
   if (instr->def.num_components != 4 || instr->def.bit_size != 32)
      return true;
   uint32_t bits[4];
   for (uint32_t c = 0; c < 4; c++)
      bits[c] = instr->value[c].u32;

   struct r300_vertex_job *job = b->job;
   uint32_t slot = job->constant_count;
   for (uint32_t c = 0; c < job->constant_count; c++) {
      if (memcmp(job->constants[c], bits, sizeof(bits)) == 0) {
         slot = c;
         break;
      }
   }
   if (slot == job->constant_count) {
      if (slot >= R300_VERTEX_JOB_MAX_CONSTANTS)
         return refuse(b, "vertex program exceeds the constant file");
      memcpy(job->constants[slot], bits, sizeof(bits));
      job->constant_count++;
   }
   uint8_t dst;
   if (!bind_new_temp(b, &instr->def, false, &dst))
      return false;
   return emit(b, R300_VERTEX_JOB_OP_LOAD_CONSTANT, dst, (uint8_t)slot, 0,
               0);
}

static bool constant_zero_offset(const nir_src *src)
{
   return nir_src_is_const(*src) && nir_src_as_uint(*src) == 0;
}

static bool build_intrinsic(struct job_build *b,
                            const nir_intrinsic_instr *intr, bool fragment)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input: {
      if (fragment)
         return refuse(b, "fragment shader reads an input");
      if (nir_intrinsic_base(intr) != 0 ||
          nir_intrinsic_component(intr) != 0 ||
          intr->def.num_components != 4 || intr->def.bit_size != 32 ||
          !constant_zero_offset(&intr->src[0]))
         return refuse(b, "vertex input outside attribute 0 as vec4");
      uint8_t dst;
      if (!bind_new_temp(b, &intr->def, false, &dst))
         return false;
      return emit(b, R300_VERTEX_JOB_OP_LOAD_INPUT, dst, 0, 0, 0);
   }
   case nir_intrinsic_store_output: {
      const nir_io_semantics io = nir_intrinsic_io_semantics(intr);
      const unsigned primary =
         fragment ? FRAG_RESULT_DATA0 : VARYING_SLOT_POS;
      if (io.location != primary || nir_intrinsic_base(intr) != 0 ||
          nir_intrinsic_component(intr) != 0 ||
          nir_intrinsic_write_mask(intr) != 0xf ||
          intr->src[0].ssa->num_components != 4 ||
          intr->src[0].ssa->bit_size != 32 ||
          !constant_zero_offset(&intr->src[1]))
         return refuse(b, fragment
                          ? "store outside a full color-0 write"
                          : "store outside a full position write");
      if (b->stored)
         return refuse(b, "second primary-output store");
      const struct ssa_binding *binding = binding_of(b, intr->src[0].ssa);
      if (!binding || !binding->bound || binding->scalar_broadcast)
         return refuse(b, "stored value outside the admitted straight line");
      b->stored = true;
      if (fragment)
         return true;
      return emit(b, R300_VERTEX_JOB_OP_STORE_POSITION, 0, binding->temp, 0,
                  0);
   }
   default:
      return refuse(b, "intrinsic outside the vec4 IO model");
   }
}

/* One straight-line pass over the entry point; on success the job (or
 * for a fragment shader the color constant) is complete. */
static bool build_from_block(nir_shader *nir, struct job_build *b,
                             bool fragment)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   if (!impl)
      return refuse(b, "shader without an entry point");
   if (nir_start_block(impl) != nir_impl_last_block(impl))
      return refuse(b, "control flow outside one straight-line block");

   const uint32_t capacity = impl->ssa_alloc;
   struct ssa_binding bindings[512];
   if (capacity > 512)
      return refuse(b, "shader outside the analysis capacity");
   memset(bindings, 0, sizeof(bindings[0]) * capacity);
   b->bindings = bindings;
   b->ssa_capacity = capacity;

   nir_foreach_instr(instr, nir_start_block(impl)) {
      switch (instr->type) {
      case nir_instr_type_load_const:
         if (!build_load_const(b, nir_instr_as_load_const(instr)))
            return false;
         break;
      case nir_instr_type_alu:
         if (!build_alu(b, nir_instr_as_alu(instr)))
            return false;
         break;
      case nir_instr_type_intrinsic:
         if (!build_intrinsic(b, nir_instr_as_intrinsic(instr), fragment))
            return false;
         break;
      case nir_instr_type_undef:
         return refuse(b, "undefined value in the admitted straight line");
      default:
         return refuse(b, "instruction outside the vec4 straight line");
      }
   }
   if (!b->stored)
      return refuse(b, fragment ? "missing color-0 store"
                                : "missing position store");
   return true;
}

bool r300_vertex_job_from_nir(nir_shader *nir, struct r300_vertex_job *job,
                              const char **reason)
{
   if (nir->info.stage != MESA_SHADER_VERTEX) {
      *reason = "vertex analysis over a non-vertex shader";
      return false;
   }
   if (!float_control_modes_supported(nir, reason))
      return false;
   memset(job, 0, sizeof(*job));
   struct job_build build = { .job = job, .reason = reason };
   if (!build_from_block(nir, &build, false))
      return false;
   /* The store emits last by construction; the caller assigns
    * input_format_id and validates the finished job. */
   return true;
}

bool r300_fragment_nir_constant_color(nir_shader *nir,
                                      uint32_t color_bits[4],
                                      const char **reason)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      *reason = "fragment analysis over a non-fragment shader";
      return false;
   }
   if (!float_control_modes_supported(nir, reason))
      return false;
   /* The fragment shape reuses the job builder as its straight-line
    * admission: the one admitted program is LOAD_CONSTANT feeding the
    * color store, so the finished job is that constant. */
   struct r300_vertex_job job;
   memset(&job, 0, sizeof(job));
   struct job_build build = { .job = &job, .reason = reason };
   if (!build_from_block(nir, &build, true))
      return false;
   if (job.instruction_count != 1 ||
       job.instructions[0].opcode != R300_VERTEX_JOB_OP_LOAD_CONSTANT) {
      *reason = "fragment program outside one constant color";
      return false;
   }
   memcpy(color_bits, job.constants[job.instructions[0].src0],
          4 * sizeof(uint32_t));
   return true;
}
