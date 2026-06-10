/*
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_pipeline.h"
#include "r300vk_format.h"
#include "util/u_simple_shaders.h"
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_from_mesa.h"
#include "tgsi/tgsi_ureg.h"
#include "compiler/nir/nir_opcodes.h"
#include "r300vk_device.h"
#include "r300vk_dp4_fs_nir.h"
#include "r300vk_shader_module.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_nir.h"
#include "vk_pipeline_layout.h"
#include "vk_object.h"
#include "vk_util.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/spirv/nir_spirv.h"
#include "r300/r300_compute_admission.h"
#include "r300/r300_screen.h"
#include "r300/compiler/r300_nir.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/macros.h"

#include <string.h>

/* Defined in the r300 driver (compiler/r300_nir_lower_vs_system_values.c).
 * Declared locally because its header r300_nir.h includes r300_screen.h, which
 * is not on the r300vk include path. */
extern bool r300_nir_lower_vs_system_values_to_inputs(nir_shader *s,
                                                      int vertex_id_slot,
                                                      int instance_id_slot);
extern void r300_nir_vs_reads_system_values(nir_shader *s,
                                            bool *reads_vertex_id,
                                            bool *reads_instance_id);

static const VkVertexInputBindingDescription *
r300vk_find_vertex_binding_desc(const VkPipelineVertexInputStateCreateInfo *vi,
                                uint32_t binding)
{
   if (!vi)
      return NULL;

   for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi->pVertexBindingDescriptions[i];
      if (desc->binding == binding)
         return desc;
   }

   return NULL;
}

static uint32_t
r300vk_vertex_fetch_size(enum pipe_format format)
{
   /* r300_create_vertex_elements_state stores r300_vertex_element_state
    * format_size as a dword-aligned byte count, and r300_emit_vertex_arrays
    * emits it through R300_VBPNTR_SIZE*.  Clamp robust vertex counts against
    * that same fetch span so tightly packed 8/16/24-bit attributes cannot
    * expose a final vertex whose r300 hardware fetch crosses the binding end. */
   return align(util_format_get_blocksize(format), 4);
}

static VkResult
r300vk_validate_vertex_input(struct r300vk_device *device,
                              const VkPipelineVertexInputStateCreateInfo *vi,
                              uint32_t *used_binding_mask,
                              uint32_t *next_input_slot)
{
   uint32_t location_mask = 0;
   uint32_t input_slot = 0;

   *used_binding_mask = 0;
   *next_input_slot = 0;

   if (!vi)
      return VK_SUCCESS;

   if (vi->vertexAttributeDescriptionCount > PIPE_MAX_ATTRIBS)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute count %u exceeds %u",
                       vi->vertexAttributeDescriptionCount,
                       PIPE_MAX_ATTRIBS);

   for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi->pVertexBindingDescriptions[i];
      if (desc->binding >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex binding %u exceeds %u",
                          desc->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
   }

   for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &vi->pVertexAttributeDescriptions[i];
      if (attr->location >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute location %u exceeds %u",
                          attr->location, R300VK_MAX_VERTEX_BINDINGS - 1);
      if (location_mask & BITFIELD_BIT(attr->location))
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: duplicate vertex attribute location %u",
                          attr->location);
      if (attr->binding >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute binding %u exceeds %u",
                          attr->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
      if (!r300vk_find_vertex_binding_desc(vi, attr->binding))
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex attribute binding %u has no "
                          "matching binding description", attr->binding);
      location_mask |= BITFIELD_BIT(attr->location);
      input_slot = MAX2(input_slot, attr->location + 1);
      *used_binding_mask |= BITFIELD_BIT(attr->binding);
   }

   if (input_slot > 0 && location_mask != BITFIELD_MASK(input_slot))
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: sparse vertex attribute locations are not "
                       "representable by r300g vertex elements");

   *next_input_slot = input_slot;
   return VK_SUCCESS;
}

static VkResult
r300vk_reserve_vs_system_value_streams(
   struct r300vk_device *device,
   struct r300vk_pipeline *pl,
   const VkPipelineVertexInputStateCreateInfo *vi,
   bool needs_vertex_id,
   bool needs_instance_id,
   int *vertex_id_slot,
   int *instance_id_slot)
{
   *vertex_id_slot = -1;
   *instance_id_slot = -1;

   if (!needs_vertex_id && !needs_instance_id)
      return VK_SUCCESS;

   const uint32_t synth_count =
      (needs_vertex_id ? 1u : 0u) + (needs_instance_id ? 1u : 0u);
   uint32_t next_input_slot = 0;
   uint32_t used_binding_mask = 0;

   VkResult val_res =
      r300vk_validate_vertex_input(device, vi, &used_binding_mask,
                                   &next_input_slot);
   if (val_res != VK_SUCCESS)
      return val_res;

   if (next_input_slot > PIPE_MAX_ATTRIBS - synth_count ||
       next_input_slot > R300VK_MAX_VERTEX_BINDINGS - synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: no vertex input slot available for "
                       "the synthetic VS system-value stream");

   uint8_t synth_bindings[2];
   uint32_t reserved_count = 0;
   for (uint32_t b = 0; b < R300VK_MAX_VERTEX_BINDINGS; b++) {
      if (used_binding_mask & BITFIELD_BIT(b))
         continue;
      synth_bindings[reserved_count++] = (uint8_t)b;
      if (reserved_count == synth_count)
         break;
   }

   if (reserved_count < synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: no vertex buffer binding available for "
                       "the synthetic VS system-value stream");

   uint32_t current_slot = next_input_slot;
   uint32_t synth_index = 0;

   pl->needs_vertex_id_stream = needs_vertex_id;
   if (needs_vertex_id) {
      *vertex_id_slot = (int)current_slot;
      pl->vertex_id_slot = (uint8_t)current_slot;
      pl->vertex_id_vb_binding = synth_bindings[synth_index++];
      current_slot++;
   } else {
      pl->vertex_id_slot = 0;
      pl->vertex_id_vb_binding = 0;
   }

   pl->needs_instance_id_stream = needs_instance_id;
   if (needs_instance_id) {
      *instance_id_slot = (int)current_slot;
      pl->instance_id_slot = (uint8_t)current_slot;
      pl->instance_id_vb_binding = synth_bindings[synth_index++];
   } else {
      pl->instance_id_slot = 0;
      pl->instance_id_vb_binding = 0;
   }

   return VK_SUCCESS;
}

static const struct spirv_to_nir_options r300vk_spirv_opts = {
   .environment            = NIR_SPIRV_VULKAN,
   .ubo_addr_format        = nir_address_format_32bit_index_offset,
   .ssbo_addr_format       = nir_address_format_32bit_index_offset,
   .push_const_addr_format = nir_address_format_32bit_offset,
   .shared_addr_format     = nir_address_format_32bit_offset,
};

/* r300 has one constant file (RC_FILE_CONSTANT), and ntr_emit_load_ubo asserts
 * the UBO block index is 0.  r300vk_nir_lower_vulkan_resource_index_single has
 * already rejected any shader that needs more than the single uniform buffer
 * r300vk_bind_descriptor_ubo binds at CONST[0], and lowered the surviving
 * descriptor chain to a constant block-0 address.  nir_lower_explicit_io then
 * rebuilds that block index as a vec construct + component extract (block =
 * vec2(addr.x, ...).x), i.e. a mov of a vec, not a load_const, and no pass folds
 * it back to a constant before nir_to_rc consumes it.  Force every
 * load_ubo[_vec4] block index to a literal 0 so the index-0 assert holds; the
 * single bound buffer lives at CONST[0]. */
static void
r300vk_nir_remap_single_ubo_to_index0(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_ubo &&
                intr->intrinsic != nir_intrinsic_load_ubo_vec4)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&intr->src[0], nir_imm_int(&b, 0));
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
}

/* r300 exposes a single constant file per stage, and r300vk_bind_descriptor_ubo
 * binds the one bound uniform buffer at CONST[0].  vk_spirv_to_nir emits each
 * UBO access as a vulkan_resource_index -> load_vulkan_descriptor -> deref ->
 * load chain, carrying the resource address in 32bit_index_offset form, where
 * component .x is the constant-file block index and .y the base offset.
 * nir_lower_explicit_io derives load_ubo's block index from that chain, which is
 * a non-constant SSA value, so r300vk_nir_remap_single_ubo_to_index0 cannot fold
 * it to index 0 and the pipeline is rejected.  Lower the chain here, before
 * explicit I/O, so the single supported UBO resolves to a literal block 0:
 *   vulkan_resource_index(set, binding) -> imm ivec2(0, 0)
 *   load_vulkan_descriptor(addr)        -> addr   (identity for index_offset)
 * The byte offset inside the buffer is applied at bind time through
 * pipe_constant_buffer::buffer_offset, so the in-shader base offset is 0.  The
 * post-explicit-I/O remap then sees only block 0 and is a no-op safety net.
 *
 * Reject every descriptor shape r300's single read-only constant file cannot
 * represent, so the pipeline fails create with VK_ERROR_FEATURE_NOT_PRESENT
 * rather than aliasing distinct buffers onto CONST[0] and rendering garbage:
 *   - a storage-buffer descriptor (the constant file is read-only),
 *   - a vulkan_resource_reindex (a dynamically indexed descriptor array),
 *   - a non-constant or non-zero resource array index,
 *   - more than one distinct (set, binding) uniform buffer.
 */
static bool
r300vk_nir_lower_vulkan_resource_index_single(nir_shader *nir,
                                              bool *out_has_ubo,
                                              uint32_t *out_set,
                                              uint32_t *out_binding)
{
   int64_t seen_set = -1;
   uint32_t seen_binding = 0;

   *out_has_ubo = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            /* A dynamically indexed descriptor array reindexes the resource;
             * r300 binds one buffer, so this cannot be satisfied. */
            if (intr->intrinsic == nir_intrinsic_vulkan_resource_reindex)
               return false;

            if (intr->intrinsic != nir_intrinsic_vulkan_resource_index)
               continue;

            /* The constant file is read-only: a storage buffer (or any
             * non-uniform-buffer descriptor) cannot be mapped onto it. */
            if (nir_intrinsic_desc_type(intr) != nir_descriptor_type_uniform_buffer)
               return false;

            /* Only the constant, zeroth array element resolves to the single
             * bound buffer; a dynamic or non-zero index names a buffer r300
             * never binds. */
            if (!nir_src_is_const(intr->src[0]) ||
                nir_src_as_uint(intr->src[0]) != 0)
               return false;

            uint32_t set = nir_intrinsic_desc_set(intr);
            uint32_t binding = nir_intrinsic_binding(intr);
            if (seen_set < 0) {
               seen_set = (int64_t)set;
               seen_binding = binding;
            } else if ((uint32_t)seen_set != set || seen_binding != binding) {
               return false;
            }
         }
      }
   }

   /* No UBO descriptor access in this shader: nothing to lower. */
   if (seen_set < 0)
      return true;

   /* Report the one (set, binding) the shader selects so the replay binds that
    * buffer to CONST[0], not the first UBO the set happens to declare. */
   *out_has_ubo = true;
   *out_set = (uint32_t)seen_set;
   *out_binding = seen_binding;

   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            b.cursor = nir_before_instr(instr);

            if (intr->intrinsic == nir_intrinsic_vulkan_resource_index) {
               nir_def_rewrite_uses(&intr->def, nir_imm_ivec2(&b, 0, 0));
               nir_instr_remove(instr);
               progress = true;
            } else if (intr->intrinsic == nir_intrinsic_load_vulkan_descriptor) {
               nir_def_rewrite_uses(&intr->def, intr->src[0].ssa);
               nir_instr_remove(instr);
               progress = true;
            }
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   return true;
}

/* Lower a fragment subpassLoad into a normalized NEAREST texture() fetch.
 *
 * subpassLoad(input) reads the input attachment's texel at the fragment's own
 * framebuffer pixel and reaches NIR as an image_deref_load on a
 * GLSL_SAMPLER_DIM_SUBPASS image.  The stock nir_lower_input_attachments rewrites
 * it to nir_texop_txf (texelFetch), but r300 is GLSL 1.20 with no texelFetch:
 * nir_to_rc accepts only TEX/TXL/TXB/TXD, so a txf would fail the fragment
 * compile.  Rewrite instead to
 *
 *   texture(input, gl_FragCoord.xy * inv_extent),   inv_extent = (1/W, 1/H)
 *
 * a plain nir_texop_tex r300 can emit.  texelFetch(input,(i,j)) equals a NEAREST
 * texture() at ((i+0.5)/W,(j+0.5)/H); gl_FragCoord.xy is the fragment center
 * (i+0.5, j+0.5), so the product lands on that texel center.  The r300 fragment
 * ALU is FP24 (16-bit mantissa): the product error is at most W*2^-17 (about 0.03
 * texel at W=4096), well inside the half-texel NEAREST margin, so the correct
 * texel is always resolved.  inv_extent is read from the fragment CONST[0]
 * (load_ubo block 0, offset 0); the replay binds (1/W,1/H) there per draw, the
 * same slot the keystone UBO uses -- so an input-attachment shader that also
 * reads an app UBO or push constants is rejected at compile (one CONST[0]).
 * nir_lower_samplers (in nir_to_rc) assigns the Gallium unit from the input
 * variable's data.binding, which the replay binds the input image to.  A
 * multisample subpass input (GLSL_SAMPLER_DIM_SUBPASS_MS) is left unlowered so
 * the pipeline reject path catches it -- r300 is single-sample. */
static bool
r300vk_nir_lower_subpass_input(nir_shader *nir, bool *out_has_input,
                               uint32_t *out_binding)
{
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *load = nir_instr_as_intrinsic(instr);
            if (load->intrinsic != nir_intrinsic_image_deref_load)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(load->src[0]);
            if (!deref || !glsl_type_is_image(deref->type) ||
                glsl_get_sampler_dim(deref->type) != GLSL_SAMPLER_DIM_SUBPASS)
               continue;

            nir_variable *var = nir_deref_instr_get_variable(deref);
            *out_has_input = true;
            *out_binding = var ? var->data.binding : 0;
            const enum glsl_base_type rbt =
               glsl_get_sampler_result_type(deref->type);

            b.cursor = nir_before_instr(instr);
            nir_def *fragcoord_xy = nir_build_frag_coord(&b, 2);
            nir_def *inv_extent = nir_load_ubo(&b, 2, 32,
                                               nir_imm_int(&b, 0),
                                               nir_imm_int(&b, 0),
                                               .align_mul = 8,
                                               .range_base = 0, .range = 8);
            nir_def *coord = nir_fmul(&b, fragcoord_xy, inv_extent);

            /* Texture deref to the same variable, retyped as sampler2D so
             * nir_lower_samplers reads the dim; it keys the unit on
             * var->data.binding regardless of the variable's image mode. */
            nir_deref_instr *tex_deref = nir_build_deref_var(&b, var);
            tex_deref->type =
               glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, rbt);

            nir_tex_instr *tex = nir_tex_instr_create(nir, 3);
            tex->op = nir_texop_tex;
            tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
            tex->coord_components = 2;
            tex->is_array = false;
            tex->is_shadow = false;
            tex->dest_type = nir_get_nir_type_for_glsl_base_type(rbt);
            tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref,
                                              &tex_deref->def);
            tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref,
                                              &tex_deref->def);
            tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
            nir_def_init(&tex->instr, &tex->def,
                         nir_tex_instr_dest_size(tex), load->def.bit_size);
            nir_builder_instr_insert(&b, &tex->instr);
            nir_def_rewrite_uses(&load->def, &tex->def);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   return progress;
}

/* True if the shader reads push constants.  vk_spirv_to_nir (with
 * push_const_addr_format set) leaves the access as a load_deref of a
 * nir_var_mem_push_const variable; r300vk's nir_lower_explicit_io call lowers
 * only UBO/SSBO, so that deref would reach nir_to_rc, which has no push-constant
 * handler and would treat it as an unknown load_deref.  Check the variable mode
 * and the lowered load_push_constant intrinsic so the test holds wherever it
 * runs in the pipeline. */
static bool
r300vk_nir_uses_push_constants(nir_shader *nir)
{
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_push_const)
      return true;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            if (nir_instr_as_intrinsic(instr)->intrinsic ==
                nir_intrinsic_load_push_constant)
               return true;
         }
      }
   }
   return false;
}

/* True if the shader reads a uniform buffer.  Called before
 * r300vk_nir_lower_vulkan_resource_index_single rewrites the chain away, to
 * detect a push-constant + UBO collision: both resolve to CONST[0] and r300's
 * single constant file cannot hold both. */
static bool
r300vk_nir_uses_ubo(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_vulkan_resource_index &&
                nir_intrinsic_desc_type(intr) ==
                   nir_descriptor_type_uniform_buffer)
               return true;
         }
      }
   }
   return false;
}

/* True if the shader samples a texture (any tex instruction).  RS480-family has
 * no hardware vertex texture units; the vertex shader runs in software through
 * the Gallium draw module, and r300vk binds no sampler views/states to that
 * draw module, so a tex executed by the SW-TCL vertex shader dereferences a NULL
 * sampler in tgsi_exec fetch_texel and segfaults at draw.  r300vk_compile_shader
 * uses this to reject a vertex shader that samples rather than crash. */
static bool
r300vk_nir_uses_texture(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex)
               return true;
         }
      }
   }
   return false;
}

/* Lower push-constant loads onto the single constant file.  nir_lower_explicit_io
 * with push_const has turned each access into load_push_constant(offset) with a
 * BASE/RANGE; rewrite it to load_ubo(block 0, BASE + offset) so it flows through
 * the same nir_lower_ubo_vec4 + index-0 path as the descriptor UBO.  Replay binds
 * the running push-constant window at CONST[0] (r300vk_bind_push_constants), and a
 * push-constant + UBO collision is already rejected, so block 0 is unambiguous. */
static void
r300vk_nir_lower_push_constant_to_ubo0(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      bool progress = false;
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_push_constant)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_def *off = intr->src[0].ssa;
            unsigned base = nir_intrinsic_base(intr);
            if (base)
               off = nir_iadd_imm(&b, off, base);
            nir_def *val =
               nir_load_ubo(&b, intr->def.num_components, intr->def.bit_size,
                            nir_imm_int(&b, 0), off,
                            .align_mul = nir_intrinsic_align_mul(intr),
                            .align_offset = nir_intrinsic_align_offset(intr),
                            .range_base = 0, .range = 128);
            nir_def_rewrite_uses(&intr->def, val);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }

   /* nir_to_tgsi sizes the TGSI constant file from nir_var_mem_ubo VARIABLES
    * (glsl_get_explicit_size of the interface type), not from the load_ubo
    * accesses emitted above.  The rewrite leaves load_ubo(block 0, ...) with no
    * backing UBO variable, so nir_to_tgsi declares an empty constant file
    * (file_max[TGSI_FILE_CONSTANT] == -1) and the gallivm draw backend
    * (draw-use-llvm) asserts on the CONST[0] read in lp_build_emit_fetch_src
    * (Register.Index <= file_max); the C draw path skips that bounds check and
    * hides the gap.  Declare a block-0 UBO sized to the 128-byte push-constant
    * window (maxPushConstantsSize, the same .range bound above) so the constant
    * file covers every push slot, mirroring nir_lower_uniforms_to_ubo's default
    * UBO. */
   const struct glsl_type *push_ubo_type =
      glsl_array_type(glsl_vec4_type(), DIV_ROUND_UP(128, 16), 16);
   nir_variable *push_ubo =
      nir_variable_create(nir, nir_var_mem_ubo, push_ubo_type, "push_const_ubo0");
   push_ubo->data.driver_location = 0;
   push_ubo->data.binding = 0;
   push_ubo->data.explicit_binding = 1;
   struct glsl_struct_field push_field = {
      .type = push_ubo_type,
      .name = "data",
      .location = -1,
   };
   push_ubo->interface_type =
      glsl_interface_type(&push_field, 1, GLSL_INTERFACE_PACKING_STD430, false,
                          "__r300vk_push_const_ubo0");
   nir->info.num_ubos = MAX2(nir->info.num_ubos, 1);
   nir->info.first_ubo_is_default_ubo = true;
}

/* R300's constant file (RC_FILE_CONSTANT) is float-typed and the ISA has no native
 * integers (r300_screen.c caps->integers = false).  nir_lower_int_to_float rewrites
 * integer load_const literals to their float value but SKIPS intrinsics, so an
 * integer push-constant value reaches the shader as raw bits while the literals it
 * is compared against become floats -- e.g. switch(kind) compares the bit pattern
 * 0x00000002 (a denormal) against 2.0f and never matches, so the shader takes the
 * wrong path and renders the wrong color.  Reject a push-constant block whose
 * loaded member is not float.  Inspect load_deref SITES (not the block variable
 * type) so a declared-but-never-loaded integer member does not force rejection.
 * Must run before nir_lower_explicit_io, which erases the per-member deref type. */
static bool
r300vk_nir_push_const_all_float(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_deref)
               continue;
            nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
            if (!deref)
               continue;
            nir_variable *var = nir_deref_instr_get_variable(deref);
            if (!var || var->data.mode != nir_var_mem_push_const)
               continue;
            if (glsl_get_base_type(glsl_without_array(deref->type)) !=
                GLSL_TYPE_FLOAT)
               return false;
         }
      }
   }
   return true;
}

/* r300's constant file is addressed by a compile-time vec4 slot plus component,
 * so a runtime (dynamically-indexed) offset cannot be represented.  Two distinct
 * shader shapes trip this: a push-constant offset and a UBO byte offset.  After
 * nir_lower_ubo_vec4 a non-constant offset becomes a runtime vector_extract the
 * SW-TCL nir_to_tgsi float-ARL path (no native integers) cannot map to a static
 * constant fetch -- it floors a non-zero integer index's float bit pattern to
 * slot 0 -- and nir_lower_int_to_float (which nir_to_rc runs next, r300 having no
 * native integers) has no float lowering for the ushr/iand/udiv a dynamic offset
 * decomposes into: it asserts on a debug build and mistranslates on release.
 *
 * Accept only a constant offset at src[off_src]; when straddle is set, also
 * require it to fit within one 16-byte slot (a vec4 access crossing a slot
 * boundary becomes a two-load bcsel with runtime component selection).  A
 * loop-bounded constant access (color[i] in a statically-bounded loop) carries a
 * non-constant offset until the loop unrolls, so a cheap pre-pass first checks
 * whether any offset is even non-constant and returns early otherwise; only then
 * pay for a clone + r300_optimize_nir (the same pass r300_create_*_state runs
 * later) to fold the loop-bounded case before judging it genuinely dynamic
 * (dynamic_index_vert indexes by a gl_Position-derived value that never folds).
 *
 * Shared by the push-constant gate (load_push_constant, src[0], slot-straddle
 * checked) and the UBO-offset gate (load_ubo_vec4, src[1]); together with the
 * dynamic-UBO-index reject in r300vk_nir_lower_vulkan_resource_index_single (the
 * index selects which UBO, the offset selects where within it) they form the
 * complete "r300 constant-file representability" gate. */
static bool
r300vk_nir_offsets_static(struct pipe_screen *pscreen, nir_shader *nir,
                          nir_intrinsic_op op, unsigned off_src, bool straddle)
{
   bool maybe_dynamic = false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == op && !nir_src_is_const(intr->src[off_src]))
               maybe_dynamic = true;
         }
      }
   }
   if (!maybe_dynamic)
      return true;

   nir_shader *check = nir_shader_clone(NULL, nir);
   r300_optimize_nir(check, r300_screen(pscreen));

   bool ok = true;
   nir_foreach_function_impl(impl, check) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != op)
               continue;
            if (!nir_src_is_const(intr->src[off_src])) {
               ok = false;
               continue;
            }
            if (straddle) {
               uint32_t off = (uint32_t)nir_src_as_uint(intr->src[off_src]);
               unsigned byte_width =
                  intr->def.num_components * (intr->def.bit_size / 8u);
               if ((off & 15u) + byte_width > 16u)
                  ok = false;
            }
         }
      }
   }
   ralloc_free(check);
   return ok;
}

/* BASE is 0 for push constants (after nir_lower_explicit_io), so src[0] is the
 * full byte offset; slot-straddle matters. */
static bool
r300vk_nir_push_const_shape_ok(struct pipe_screen *pscreen, nir_shader *nir)
{
   return r300vk_nir_offsets_static(pscreen, nir,
                                    nir_intrinsic_load_push_constant, 0, true);
}

static VkResult
r300vk_compile_shader(struct r300vk_device *device,
                       const VkPipelineShaderStageCreateInfo *stage_info,
                       struct r300vk_pipeline *pl,
                       const VkPipelineVertexInputStateCreateInfo *vi)
{
   /* r300g exposes VS and FS only; geometry, tessellation, and compute are
    * unsupported on R300-class hardware. */
   if (stage_info->stage != VK_SHADER_STAGE_VERTEX_BIT &&
       stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: unsupported shader stage 0x%x",
                       stage_info->stage);

   VK_FROM_HANDLE(r300vk_shader_module, mod, stage_info->module);
   mesa_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);

   const struct nir_shader_compiler_options *nir_opts =
      device->screen->nir_options[stage];

   nir_shader *nir = vk_spirv_to_nir(&device->vk,
                                      mod->code, mod->code_size,
                                      stage, stage_info->pName,
                                      stage_info->pSpecializationInfo,
                                      &r300vk_spirv_opts, nir_opts,
                                      false, NULL);
   if (!nir)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vk_spirv_to_nir failed for %s shader",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");

   /* vk_spirv_to_nir lowers SPIR-V Private-storage-class variables to
    * nir_var_shader_temp and then deletes every non-entrypoint function
    * (nir_remove_non_cmat_call_entrypoints), so each Private variable ends up
    * used only in the one remaining function yet still carries the shader_temp
    * mode.  r300's nir_to_tgsi path promotes locals with nir_lower_vars_to_ssa
    * and nir_lower_locals_to_regs, both of which act on nir_var_function_temp
    * and skip shader_temp; a surviving load_deref of the Private variable would
    * reach ntt_get_alu_src with an unassigned ureg and trip the
    * TGSI_FILE_NULL assert in ureg_swizzle -- an abort under asserts, a wrong
    * constant-file read once asserts are compiled out.  Localize the
    * single-function Private variables to function_temp so the ntt local
    * promotion passes own them. */
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);

   /* Lower a fragment subpassLoad to a normalized texture() (r300 has no
    * texelFetch) before the constant/UBO lowering below.  It injects inv_extent
    * at CONST[0], so the collision check below rejects an input-attachment shader
    * that also reads an app UBO or push constants.  Runs before
    * nir_lower_explicit_io/nir_lower_ubo_vec4 so the emitted load_ubo(0) follows
    * the same vec4-slot path as the keystone UBO. */
   bool stage_has_input = false;
   uint32_t stage_input_binding = 0;
   if (stage_info->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
      r300vk_nir_lower_subpass_input(nir, &stage_has_input, &stage_input_binding);

   /* Push constants and a UBO both resolve to CONST[0]; r300's single constant
    * file cannot host both, so reject the pair before the lowering below rewrites
    * the UBO chain away.  A shader using only one of them is supported. */
   const bool uses_push_const = r300vk_nir_uses_push_constants(nir);
   if (uses_push_const && r300vk_nir_uses_ubo(nir)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: %s shader uses both push constants and a uniform "
                       "buffer; r300's single constant file holds only one at "
                       "CONST[0]",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* A lowered subpassLoad reads inv_extent from CONST[0]; an input-attachment
    * fragment shader that also reads an app UBO or push constants would need two
    * CONST[0] contents.  Reject rather than render wrong pixels. */
   if (stage_has_input && (uses_push_const || r300vk_nir_uses_ubo(nir))) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: fragment shader combines a subpass input with a "
                       "uniform buffer or push constants; r300's single CONST[0] "
                       "holds only the input's inv_extent");
   }
   if (stage_has_input) {
      pl->fs_has_input_attachment = true;
      pl->fs_input_attachment_binding = stage_input_binding;
   }

   /* Resolve the descriptor resource chain (vulkan_resource_index ->
    * load_vulkan_descriptor) to a constant block-0 address, or reject the
    * pipeline when the shader needs a resource r300's single read-only constant
    * file cannot represent.  See r300vk_nir_lower_vulkan_resource_index_single. */
   bool stage_has_ubo = false;
   uint32_t stage_ubo_set = 0, stage_ubo_binding = 0;
   if (!r300vk_nir_lower_vulkan_resource_index_single(nir, &stage_has_ubo,
                                                      &stage_ubo_set,
                                                      &stage_ubo_binding)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: %s shader uses a descriptor resource r300's "
                       "single read-only constant file cannot represent "
                       "(storage buffer, multiple UBOs, or a dynamic UBO index)",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* Record which UBO (set, binding) this stage reads so the replay binds that
    * exact descriptor to the stage's CONST[0].  r300 has separate vertex and
    * fragment constant files, so the two stages may select different bindings
    * (and dEQP-VK.ubo.link_by_binding reads two bindings of one buffer across
    * the stages); each is bound independently rather than forcing one buffer
    * onto both. */
   if (stage_has_ubo) {
      if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
         pl->vs_has_ubo = true;
         pl->vs_ubo_set = stage_ubo_set;
         pl->vs_ubo_binding = stage_ubo_binding;
      } else {
         pl->fs_has_ubo = true;
         pl->fs_ubo_set = stage_ubo_set;
         pl->fs_ubo_binding = stage_ubo_binding;
      }
   }

   /* A push-constants-only shader maps onto the same single constant file: lower
    * the push_const derefs to load_push_constant, then onto load_ubo(block 0) so
    * the UBO path below (nir_lower_ubo_vec4 + the index-0 pin) carries them.
    * Replay binds the running push-constant window at CONST[0]
    * (r300vk_bind_push_constants).  A push-constant + UBO collision was rejected
    * above, so block 0 is unambiguous.  r300's float-only constant file and static
    * vec4-slot addressing cannot represent an integer push constant or a
    * dynamic/slot-straddling offset, so reject those shapes rather than render
    * wrong pixels (correct-or-clean-reject). */
   if (uses_push_const) {
      if (!r300vk_nir_push_const_all_float(nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: %s shader reads a non-float push constant; "
                          "r300's constant file (RC_FILE_CONSTANT) is float-typed",
                          stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                          ? "vertex" : "fragment");
      }
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
               nir_address_format_32bit_offset);
      if (!r300vk_nir_push_const_shape_ok(device->screen, nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: %s shader uses a dynamic or slot-straddling "
                          "push-constant offset; r300 constant-file addressing is "
                          "static and 16-byte-slot granular",
                          stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                          ? "vertex" : "fragment");
      }
      r300vk_nir_lower_push_constant_to_ubo0(nir);
      pl->uses_push_constants = true;
   }

   /* With the descriptor chain resolved to constant block 0, lower the UBO
    * deref/load to load_ubo, which ntr_emit_load_ubo (the FS nir_to_rc path)
    * and the SW-TCL VS nir_to_tgsi path consume.  An unlowered
    * vulkan_resource_index chain would otherwise dummy-shader the FS or abort
    * the VS.  nir_var_mem_ssbo stays for completeness; a storage-buffer
    * descriptor is already rejected above, so none remain. */
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   /* nir_lower_explicit_io leaves load_ubo with a byte offset, but nir_to_rc's
    * ntr_emit_load_ubo addresses the constant file by vec4 index (it handles
    * load_ubo_vec4 and applies the component shift).  Convert to load_ubo_vec4,
    * the same form r300g's GL uniforms-to-UBO path feeds nir_to_rc. */
   NIR_PASS(_, nir, nir_lower_ubo_vec4);

   /* The single supported UBO is bound at CONST[0]; pin every load_ubo[_vec4]
    * block index to a literal 0 for ntr_emit_load_ubo's index-0 assert. */
   r300vk_nir_remap_single_ubo_to_index0(nir);

   /* Reject a dynamic UBO offset (ubo.arr[i], a runtime-indexed member) the same
    * way push constants are gated: r300's static vec4-slot constant file cannot
    * address load_ubo_vec4's src[1] offset at runtime.  Complements the
    * dynamic-UBO-index reject above -- the index selects which UBO, the offset
    * selects where within it; r300 can represent neither dynamically. */
   if (!r300vk_nir_offsets_static(device->screen, nir,
                                  nir_intrinsic_load_ubo_vec4, 1, false)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: %s shader uses a dynamic uniform-buffer offset; "
                       "r300's constant file is addressed by a static vec4 slot",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* vk_spirv_to_nir sets data.location for VS inputs (VERT_ATTRIB_GENERIC0+n)
    * but leaves data.driver_location at zero for all variables.  nir_lower_io
    * inside r300g's nir_to_rc uses driver_location as the RC input base, so
    * all inputs would collapse to IN[0] without this assignment. */
   if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
      /* RS480-family has no hardware vertex texture units, and the SW-TCL draw
       * module that runs the vertex shader has no sampler views bound by r300vk,
       * so a vertex texture fetch reaches tgsi_exec fetch_texel with a NULL
       * sampler (vs_exec_run_linear -> r300_swtcl_draw_vbo) and segfaults at
       * draw.  Reject a vertex shader that samples rather than crash.  A
       * conformant vertex-texturing path must bind the draw module's
       * PIPE_SHADER_VERTEX sampler views and sampler state before this gate can
       * be removed. */
      if (r300vk_nir_uses_texture(nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r300vk: vertex shader samples a texture; the RS480 "
                          "SW-TCL vertex path binds no sampler and cannot fetch "
                          "from a vertex stage");
      }

      /* vk_spirv_to_nir leaves gl_VertexIndex / gl_InstanceIndex as a load_deref
       * of a nir_var_system_value variable and never gathers system_values_read,
       * so nir->info cannot be trusted to flag the read here.  Scan the NIR for
       * both the deref and the lowered-intrinsic form instead. */
      bool needs_vid = false, needs_iid = false;
      r300_nir_vs_reads_system_values(nir, &needs_vid, &needs_iid);
      if (needs_vid || needs_iid) {
         int vid_slot = -1;
         int iid_slot = -1;
         VkResult r = r300vk_reserve_vs_system_value_streams(
            device, pl, vi, needs_vid, needs_iid, &vid_slot, &iid_slot);
         if (r != VK_SUCCESS) {
            ralloc_free(nir);
            return r;
         }

         if (!r300_nir_lower_vs_system_values_to_inputs(nir, vid_slot,
                                                        iid_slot)) {
            ralloc_free(nir);
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r300vk: failed to lower VS system values");
         }

         bool still_needs_vid = false, still_needs_iid = false;
         r300_nir_vs_reads_system_values(nir, &still_needs_vid,
                                         &still_needs_iid);
         if (still_needs_vid || still_needs_iid) {
            ralloc_free(nir);
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r300vk: VS system-value lowering left an "
                             "unsupported read");
         }
      }

      nir_foreach_shader_in_variable(var, nir) {
         assert(var->data.location >= VERT_ATTRIB_GENERIC0);
         var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
      }
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   } else {
      nir_assign_io_var_locations(nir, nir_var_shader_in);
   }

   struct pipe_shader_state ss = {
      .type   = PIPE_SHADER_IR_NIR,
      .ir.nir = nir,
   };

   if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
      pl->vs_cso = device->pipe->create_vs_state(device->pipe, &ss);
      if (!pl->vs_cso) {
         ralloc_free(nir);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      pl->vs_hw_valid = r300_vs_get_hw_code(pl->vs_cso, &pl->vs_hw);
   } else {
      pl->fs_cso = device->pipe->create_fs_state(device->pipe, &ss);
      if (!pl->fs_cso) {
         ralloc_free(nir);
         return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      }
      pl->fs_hw_valid = r300_fs_get_hw_code(pl->fs_cso, &pl->fs_hw);
   }
   return VK_SUCCESS;
}

/* Build and create the vertex elements CSO.  Extracted to keep
 * r300vk_create_one_pipeline within the CCN budget. */
static VkResult
r300vk_vertex_element_count(struct r300vk_device *device,
                            const VkPipelineVertexInputStateCreateInfo *vi,
                            uint32_t *element_count_out)
{
   uint32_t element_count = 0;
   uint32_t location_mask = 0;

   if (!vi) {
      *element_count_out = 0;
      return VK_SUCCESS;
   }

   for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &vi->pVertexAttributeDescriptions[i];
      if (attr->location >= R300VK_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r300vk: vertex attribute location %u exceeds %u",
                          attr->location, R300VK_MAX_VERTEX_BINDINGS - 1);
      if (location_mask & BITFIELD_BIT(attr->location))
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r300vk: duplicate vertex attribute location %u",
                          attr->location);

      location_mask |= BITFIELD_BIT(attr->location);
      element_count = MAX2(element_count, attr->location + 1);
   }

   if (element_count > 0 && location_mask != BITFIELD_MASK(element_count))
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: sparse vertex attribute locations are not "
                       "representable by r300g vertex elements");

   *element_count_out = element_count;
   return VK_SUCCESS;
}

static VkResult
r300vk_populate_vertex_element(struct r300vk_device *device,
                                struct r300vk_pipeline *pl,
                                const VkPipelineVertexInputStateCreateInfo *vi,
                                uint32_t attr_index,
                                struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS])
{
   const VkVertexInputAttributeDescription *attr =
      &vi->pVertexAttributeDescriptions[attr_index];
   if (attr->binding >= R300VK_MAX_VERTEX_BINDINGS)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vertex attribute binding %u exceeds %u",
                       attr->binding, R300VK_MAX_VERTEX_BINDINGS - 1);

   enum pipe_format elem_fmt = r300vk_vk_format_to_pipe_format(attr->format);
   if (elem_fmt == PIPE_FORMAT_NONE)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: unsupported vertex attribute format %d "
                       "at location %u", attr->format, attr->location);
   const uint32_t attr_size = r300vk_vertex_fetch_size(elem_fmt);
   if (attr->offset > UINT32_MAX - attr_size)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: vertex attribute offset %u exceeds "
                       "representable binding extent", attr->offset);

   struct pipe_vertex_element *elem = &ve[attr->location];
   elem->src_offset          = (uint16_t)attr->offset;
   elem->vertex_buffer_index = (uint8_t)attr->binding;
   elem->src_format          = (uint8_t)elem_fmt;
   const VkVertexInputBindingDescription *binding_desc =
      r300vk_find_vertex_binding_desc(vi, attr->binding);
   if (!binding_desc)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: vertex attribute binding %u has no "
                       "matching binding description", attr->binding);

   elem->src_stride = binding_desc->stride;
   pl->vertex_stride[attr->binding] = binding_desc->stride;
   pl->vertex_binding_extent[attr->binding] =
      MAX2(pl->vertex_binding_extent[attr->binding],
           attr->offset + attr_size);
   pl->vertex_binding_mask |= BITFIELD_BIT(attr->binding);

   return VK_SUCCESS;
}

static VkResult
r300vk_build_velems_cso(struct r300vk_device *device,
                         struct r300vk_pipeline *pl,
                         const VkPipelineVertexInputStateCreateInfo *vi)
{
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
   uint32_t n;
   VkResult result = r300vk_vertex_element_count(device, vi, &n);
   if (result != VK_SUCCESS)
      return result;

   if (vi) {
      for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++) {
         const VkVertexInputBindingDescription *desc =
            &vi->pVertexBindingDescriptions[b];
         if (desc->binding >= R300VK_MAX_VERTEX_BINDINGS)
            return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                             "r300vk: vertex binding %u exceeds %u",
                             desc->binding, R300VK_MAX_VERTEX_BINDINGS - 1);
      }
   }

   memset(ve, 0, sizeof(ve));
   for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
      VkResult res = r300vk_populate_vertex_element(device, pl, vi, i, ve);
      if (res != VK_SUCCESS)
         return res;
   }

   /* Append synthetic VS-system-value elements (R32_SINT, one int per element)
    * the VS reads via the lowering in r300vk_compile_shader.  instance_divisor
    * 0 steps the vertex-id element per vertex; 1 steps the instance-id element
    * per instance. */
   uint32_t velem_count = n;
   if (pl->needs_vertex_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->vertex_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_SINT;
      ve[velem_count].src_stride          = sizeof(int32_t);
      ve[velem_count].instance_divisor    = 0;
      velem_count++;
   }
   if (pl->needs_instance_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->instance_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_SINT;
      ve[velem_count].src_stride          = sizeof(int32_t);
      ve[velem_count].instance_divisor    = 1;
      velem_count++;
   }

   pl->velems_cso =
      device->pipe->create_vertex_elements_state(device->pipe, velem_count, ve);
   if (!pl->velems_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   return VK_SUCCESS;
}

static VkResult
r300vk_init_graphics_pipeline_cso_state(struct r300vk_device *device,
                                        struct r300vk_pipeline *pl)
{
   struct pipe_blend_state bs = {0};
   bs.rt[0].rgb_func        = PIPE_BLEND_ADD;
   bs.rt[0].rgb_src_factor  = PIPE_BLENDFACTOR_ONE;
   bs.rt[0].rgb_dst_factor  = PIPE_BLENDFACTOR_ZERO;
   bs.rt[0].alpha_func      = PIPE_BLEND_ADD;
   bs.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   bs.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
   bs.rt[0].colormask       = PIPE_MASK_RGBA;
   pl->blend_cso = device->pipe->create_blend_state(device->pipe, &bs);
   if (!pl->blend_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   struct pipe_rasterizer_state rs = {0};
   rs.fill_front  = PIPE_POLYGON_MODE_FILL;
   rs.fill_back   = PIPE_POLYGON_MODE_FILL;
   rs.cull_face   = PIPE_FACE_NONE;
   rs.front_ccw   = true;
   rs.depth_clip_near = true;
   rs.depth_clip_far  = true;
   pl->rasterizer_cso = device->pipe->create_rasterizer_state(device->pipe, &rs);
   if (!pl->rasterizer_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   struct pipe_depth_stencil_alpha_state dsa = {0};
   pl->dsa_cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   if (!pl->dsa_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   return VK_SUCCESS;
}

static void
r300vk_capture_dynamic_state(struct r300vk_pipeline *pl,
                             const VkGraphicsPipelineCreateInfo *info)
{
   bool dynamic_viewport = false;
   bool dynamic_scissor = false;
   bool dynamic_raster_discard = false;
   if (info->pDynamicState) {
      for (uint32_t d = 0; d < info->pDynamicState->dynamicStateCount; d++) {
         switch (info->pDynamicState->pDynamicStates[d]) {
         case VK_DYNAMIC_STATE_VIEWPORT:
         case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
            dynamic_viewport = true;
            break;
         case VK_DYNAMIC_STATE_SCISSOR:
         case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
            dynamic_scissor = true;
            break;
         case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE:
            dynamic_raster_discard = true;
            break;
         default:
            break;
         }
      }
   }

   /* pViewportState is ignored when rasterization is statically discarded, and
    * the app may then leave it NULL or pass a garbage pointer
    * (dEQP-VK.api.pipeline.pipeline_invalid_pointers_unused_structs.graphics).
    * Read it only when rasterization can run: discard disabled, or made dynamic
    * so a later draw may enable it (in which case the app must supply a valid
    * pViewportState). */
   const VkPipelineRasterizationStateCreateInfo *rs = info->pRasterizationState;
   const bool raster_discarded =
      rs && rs->rasterizerDiscardEnable && !dynamic_raster_discard;
   const VkPipelineViewportStateCreateInfo *vp_state =
      raster_discarded ? NULL : info->pViewportState;
   if (vp_state && !dynamic_viewport &&
       vp_state->pViewports && vp_state->viewportCount > 0) {
      pl->static_viewport = vp_state->pViewports[0];
      pl->has_static_viewport = true;
   }
   if (vp_state && !dynamic_scissor &&
       vp_state->pScissors && vp_state->scissorCount > 0) {
      pl->static_scissor = vp_state->pScissors[0];
      pl->has_static_scissor = true;
   }
}

static VkResult
r300vk_create_one_pipeline(struct r300vk_device *device,
                             const VkGraphicsPipelineCreateInfo *info,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   struct r300vk_pipeline *pl;

   pl = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*pl), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pl)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);

#define FAIL_PIPELINE(r) \
   do { \
      r300vk_DestroyPipeline(r300vk_device_to_handle(device), \
                             r300vk_pipeline_to_handle(pl), pAllocator); \
      return (r); \
   } while (0)

   /* r300 exposes a single constant-buffer slot (max_const_buffers = 1) and binds
    * one flat push-constant window at CONST[0]; it cannot represent more than one
    * push-constant range (per-stage divergent or overlapping ranges share the slot
    * and alias).  Reject a multi-range layout at create time rather than render
    * wrong pixels. */
   VK_FROM_HANDLE(vk_pipeline_layout, pc_layout, info->layout);
   if (pc_layout && pc_layout->push_range_count > 1)
      FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                              "r300vk: %u push-constant ranges; r300's single "
                              "constant slot supports at most one",
                              pc_layout->push_range_count));

   for (uint32_t i = 0; i < info->stageCount; i++) {
      VkResult r = r300vk_compile_shader(device, &info->pStages[i], pl,
                                         info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

   /* r300 has separate vertex and fragment constant files, so a single shader
    * using both push constants and a UBO is the only true CONST[0] collision,
    * and r300vk_compile_shader already rejects that per stage.  A pipeline that
    * splits them across stages (push constants in one, a UBO in the other) is
    * representable in hardware, but the replay is not split: it binds the
    * push-constant window to BOTH stages' CONST[0] (r300vk_bind_push_constants)
    * whenever any stage uses push constants, which then cannot also bind the
    * other stage's UBO.  Reject the cross-stage mix rather than silently
    * overwrite the UBO stage's CONST[0] with the push-constant window. */
   if (pl->uses_push_constants && (pl->vs_has_ubo || pl->fs_has_ubo))
      FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                              "r300vk: pipeline uses push constants in one stage "
                              "and a uniform buffer in another; the replay binds "
                              "the push-constant window to both stages' CONST[0] "
                              "and cannot also bind a per-stage UBO"));

   VkResult cso_res = r300vk_init_graphics_pipeline_cso_state(device, pl);
   if (cso_res != VK_SUCCESS)
      FAIL_PIPELINE(cso_res);

   if (info->pVertexInputState || pl->needs_vertex_id_stream ||
       pl->needs_instance_id_stream) {
      VkResult r = r300vk_build_velems_cso(device, pl, info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

#undef FAIL_PIPELINE

   pl->topology = info->pInputAssemblyState
                  ? info->pInputAssemblyState->topology
                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   r300vk_capture_dynamic_state(pl, info);

   *pPipeline = r300vk_pipeline_to_handle(pl);
   return VK_SUCCESS;
}

VkResult
r300vk_CreateGraphicsPipelines(VkDevice _device,
                                 VkPipelineCache pipelineCache,
                                 uint32_t createInfoCount,
                                 const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult r = r300vk_create_one_pipeline(device, &pCreateInfos[i],
                                               pAllocator, &pPipelines[i]);
      if (r != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;
         if (result == VK_SUCCESS)
            result = r;
      }
   }
   return result;
}

/* Classify one compute kernel against the RS482 compute-as-raster substrate
 * without lowering or executing it.  r300g sets nir_options for VERTEX and
 * FRAGMENT only, so there is no compute entry to translate with; the kernel is
 * translated with the fragment-stage options purely to obtain a well-formed
 * nir_shader to walk.  That is sound for classification because the substrate
 * verbs (FP24 ALU compute, texture-load, RB3D export, blend/stencil/ZPASS
 * reductions) are the fragment pipeline's, and the kernel is never handed to
 * the RC backend.  r300_nir_classify_compute reads the shader and mutates
 * nothing.  Returns false only when SPIR-V translation itself failed. */
static bool
r300vk_classify_compute_kernel(struct r300vk_device *device,
                               const VkPipelineShaderStageCreateInfo *stage_info,
                               struct r300_compute_admission *adm,
                               struct r300_compute_identity_pattern *ident,
                               struct r300_compute_binary_map_pattern *binmap,
                               struct r300_compute_blend_acc_reduction_pattern *blendacc,
                               struct r300_compute_zpass_reduction_pattern *zpass,
                               struct r300_compute_multipass_scan_pattern *multiscan,
                               struct r300_compute_predicated_store_pattern *predstore,
                               struct r300_compute_multitap_gather_pattern *gather,
                               struct r300_compute_dp4_pattern *dp4,
                               struct r300_compute_qmul_pattern *qmul,
                               struct r300_compute_qrotate_pattern *qrotate,
                               struct r300_compute_qconj_pattern *qconj,
                               struct r300_compute_qnorm_pattern *qnorm,
                               uint32_t local_size[3])
{
   VK_FROM_HANDLE(r300vk_shader_module, mod, stage_info->module);
   if (!mod)
      return false;

   nir_shader *nir = vk_spirv_to_nir(&device->vk, mod->code, mod->code_size,
                                     MESA_SHADER_COMPUTE, stage_info->pName,
                                     stage_info->pSpecializationInfo,
                                     &r300vk_spirv_opts,
                                     device->screen->nir_options[MESA_SHADER_FRAGMENT],
                                     false, NULL);
   if (!nir)
      return false;

   local_size[0] = nir->info.workgroup_size[0];
   local_size[1] = nir->info.workgroup_size[1];
   local_size[2] = nir->info.workgroup_size[2];

   /* SSA-lower the kernel's local temporaries before pattern detection.  A
    * GLSL/SPIR-V kernel that stages its inputs in locals -- vec4 a = q1[gid];
    * b = q2[gid]; out[gid] = f(a, b) -- reaches here with function_temp
    * load_deref/store_deref, so the arithmetic reads the deref loads, not the
    * load_ssbo defs the detectors key on.  Without this the substrate only ever
    * recognized kernels that inline their buffer reads.  vars_to_ssa needs the
    * copies split and lowered first. */
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
   } while (progress);

   r300_nir_classify_compute(nir, adm);
   r300_nir_detect_identity_map(nir, ident);
   r300_nir_detect_binary_map(nir, binmap);
   r300_nir_detect_blend_acc_reduction(nir, blendacc);
   r300_nir_detect_zpass_reduction(nir, zpass);
   r300_nir_detect_multipass_scan_pattern(nir, multiscan);
   r300_nir_detect_predicated_store_pattern(nir, predstore);
   r300_nir_detect_multitap_gather_pattern(nir, gather);
   r300_nir_detect_dp4_pattern(nir, dp4);
   r300_nir_detect_qmul_pattern(nir, qmul);
   r300_nir_detect_qrotate_pattern(nir, qrotate);
   r300_nir_detect_qconj_pattern(nir, qconj);
   r300_nir_detect_qnorm_pattern(nir, qnorm);

   ralloc_free(nir);
   return true;
}

/* Lazily create the gallium state CSOs every identity-map dispatch reuses:
 * blend = passthrough (write color unmodified, all four channels), rasterizer
 * = no cull / fill solid / no scissor / depth clip both planes, dsa = depth
 * test off + stencil off + alpha test off, sampler = NEAREST + CLAMP_TO_EDGE
 * (only NEAREST returns the stored texel unmodified, which the identity-map
 * bit-exact readback requires).
 *
 * Idempotent: subsequent identity-map pipelines find the CSOs populated and
 * skip recreation.  The matching delete_*_state runs in r300vk_DestroyDevice
 * before the pipe_context itself is destroyed. */
static bool
r300vk_device_init_identity_map_state(struct r300vk_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   if (!device->identity_map_blend_cso) {
      struct pipe_blend_state blend = {0};
      blend.rt[0].colormask = PIPE_MASK_RGBA;
      device->identity_map_blend_cso =
         pipe->create_blend_state(pipe, &blend);
      if (!device->identity_map_blend_cso)
         return false;
   }

   if (!device->identity_map_rasterizer_cso) {
      struct pipe_rasterizer_state raster = {0};
      raster.cull_face       = PIPE_FACE_NONE;
      raster.fill_front      = PIPE_POLYGON_MODE_FILL;
      raster.fill_back       = PIPE_POLYGON_MODE_FILL;
      raster.point_size      = 1.0f;
      raster.line_width      = 1.0f;
      raster.depth_clip_near = 1;
      raster.depth_clip_far  = 1;
      raster.half_pixel_center = 1;
      raster.bottom_edge_rule  = 1;
      device->identity_map_rasterizer_cso =
         pipe->create_rasterizer_state(pipe, &raster);
      if (!device->identity_map_rasterizer_cso)
         return false;
   }

   if (!device->identity_map_dsa_cso) {
      struct pipe_depth_stencil_alpha_state dsa = {0};
      /* All zero: depth test off, stencil off, alpha test off. */
      device->identity_map_dsa_cso =
         pipe->create_depth_stencil_alpha_state(pipe, &dsa);
      if (!device->identity_map_dsa_cso)
         return false;
   }

   if (!device->identity_map_sampler_cso) {
      struct pipe_sampler_state samp = {0};
      samp.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
      samp.min_img_filter = PIPE_TEX_FILTER_NEAREST;
      samp.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
      samp.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
      samp.max_lod  = 0.0f;
      /* unnormalized_coords stays 0 (default = normalized [0,1] coords);
       * the fullscreen-quad texcoords land in that range exactly. */
      device->identity_map_sampler_cso =
         pipe->create_sampler_state(pipe, &samp);
      if (!device->identity_map_sampler_cso)
         return false;
   }

   return true;
}

/* Emit the binary ALU op into a ureg fragment program.  Maps the detected
 * NIR opcode to its TGSI counterpart via the tgsi_ureg helpers.  The
 * admitted op set mirrors r300_compute_admission.c
 * binary_map_op_admitted().  TGSI ADD / SUB / MUL / MIN / MAX are float;
 * integer NIR opcodes fold into the same float ALU because the texture
 * sampling normalises UNORM8 bytes to [0,1] floats anyway -- the byte
 * round-trip stays bit-exact when the operator obeys the FP24 integer-exact
 * envelope. */
static bool
emit_binary_op(struct ureg_program *ureg, uint16_t nir_op,
               struct ureg_dst dst,
               struct ureg_src a, struct ureg_src b)
{
   switch (nir_op) {
   case nir_op_fadd: case nir_op_iadd:
      ureg_ADD(ureg, dst, a, b); return true;
   case nir_op_fsub: case nir_op_isub:
      /* TGSI has no SUB opcode; ureg_ADD with the second operand negated
       * is the canonical lowering (and what gallium drivers expect). */
      ureg_ADD(ureg, dst, a, ureg_negate(b)); return true;
   case nir_op_fmul: case nir_op_imul:
      ureg_MUL(ureg, dst, a, b); return true;
   case nir_op_fmin: case nir_op_imin: case nir_op_umin:
      ureg_MIN(ureg, dst, a, b); return true;
   case nir_op_fmax: case nir_op_imax: case nir_op_umax:
      ureg_MAX(ureg, dst, a, b); return true;
   default:
      return false;
   }
}

/* Synthesise the 2-sampler fragment program for the binary-map lowering:
 *   TEX  tmp0, IN[0], SAMP[0]   (sample in_a)
 *   TEX  tmp1, IN[0], SAMP[1]   (sample in_b)
 *   <op> tmp0, tmp0, tmp1       (the binary ALU op)
 *   MOV  OUT[0], tmp0
 *   END
 *
 * Costs: 2 TEX + 2 ALU = 4/96 of the R300 PFS budget
 * (R300_PFS_MAX_ALU_INST=64 / R300_PFS_MAX_TEX_INST=32). */
static void *
r300vk_synthesize_binary_map_fs(struct pipe_context *pipe, uint16_t alu_op)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp_a = ureg_DECL_sampler(ureg, 0);
   struct ureg_src samp_b = ureg_DECL_sampler(ureg, 1);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   ureg_DECL_sampler_view(ureg, 1, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp_a = ureg_DECL_temporary(ureg);
   struct ureg_dst tmp_b = ureg_DECL_temporary(ureg);

   /* ureg_load_tex (u_simple_shaders.c) is file-static and not exported;
    * call ureg_TEX directly with TGSI_TEXTURE_2D + the (coord, sampler)
    * pair, which is the same opcode the helper emits for use_txf=false. */
   ureg_TEX(ureg, ureg_writemask(tmp_a, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_a);
   ureg_TEX(ureg, ureg_writemask(tmp_b, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_b);

   if (!emit_binary_op(ureg, alu_op,
                       ureg_writemask(tmp_a, TGSI_WRITEMASK_XYZW),
                       ureg_src(tmp_a), ureg_src(tmp_b))) {
      ureg_destroy(ureg);
      return NULL;
   }

   ureg_MOV(ureg, out, ureg_src(tmp_a));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the binary-map VS + FS pair on the pipeline.  Reuses the
 * device-cached state CSOs (blend / raster / dsa / sampler) the identity-map
 * synthesis populates -- the binary-map and identity-map paths share every
 * per-draw state object; only the FS differs. */
/* Fullscreen-quad vertex shader synthesis: 2 attributes (POSITION + GENERIC).
 * Identity-map coordinate interpolation and per-vertex reduction values use
 * this passthrough shape.  Cached on the pipeline object; the existing
 * destroy path frees it. */
static void *
r300vk_synthesize_passthrough_vs(struct pipe_context *pipe)
{
   const enum tgsi_semantic names[]   = { TGSI_SEMANTIC_POSITION,
                                          TGSI_SEMANTIC_GENERIC };
   const unsigned          indices[] = { 0, 0 };
   return util_make_vertex_passthrough_shader(pipe, 2, names, indices, false);
}

static bool
r300vk_binary_map_synthesize_shaders(struct r300vk_device *device,
                                      struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_binary_map_fs(pipe, pl->binary_map.alu_op);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesize the DP4 fragment-program CSO.  The NIR itself is built by
 * r300vk_build_dp4_fs_nir (r300vk_dp4_fs_nir.c) so a build-time test can
 * validate the shader shape -- notably the 2-component 2D-sampler coordinate --
 * without a pipe_context; here we only finalize for the screen and create the
 * gallium state. */
static void *
r300vk_synthesize_dp4_fs(struct pipe_context *pipe, uint8_t components)
{
   nir_shader *s = r300vk_build_dp4_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT], components);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* DP4 VS+FS synthesis: the passthrough VS shared with binary-map plus the
 * pure-NIR DP4 FS.  Reuses the device-cached identity-map state CSOs. */
static bool
r300vk_dp4_synthesize_shaders(struct r300vk_device *device,
                              struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_dp4_fs(pipe, pl->dp4.components);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QMUL FS: the Hamilton-product fragment program built by r300vk_build_qmul_fs_nir
 * (r300vk_dp4_fs_nir.c) -- four sign-permuted DP4s writing the four-lane product
 * to the FP16 color export.  Finalize for the screen and create the gallium
 * state, as the DP4 FS does. */
static void *
r300vk_synthesize_qmul_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qmul_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QMUL VS+FS synthesis: the passthrough VS shared with DP4 plus the Hamilton FS.
 * Reuses the device-cached identity-map state CSOs. */
static bool
r300vk_qmul_synthesize_shaders(struct r300vk_device *device,
                               struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_qmul_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QROTATE FS: the sandwich q*embed(v)*conj(q) built by r300vk_build_qrotate_fs_nir
 * -- two Hamilton products, eight DP4s, to the FP16 color export. */
static void *
r300vk_synthesize_qrotate_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qrotate_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QROTATE VS+FS synthesis: the passthrough VS plus the sandwich FS. */
static bool
r300vk_qrotate_synthesize_shaders(struct r300vk_device *device,
                                  struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_qrotate_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QCONJ FS: the sign-flip conjugate built by r300vk_build_qconj_fs_nir -- one
 * sampled quaternion written as (a.x,-a.y,-a.z,-a.w) to the FP16 color export. */
static void *
r300vk_synthesize_qconj_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qconj_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QCONJ VS+FS synthesis: the passthrough VS plus the conjugate FS. */
static bool
r300vk_qconj_synthesize_shaders(struct r300vk_device *device,
                                struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_qconj_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QNORM FS: the squared-norm splat built by r300vk_build_qnorm_fs_nir -- one
 * sampled quaternion written as vec4(dot(a,a)) to the FP16 color export. */
static void *
r300vk_synthesize_qnorm_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qnorm_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QNORM VS+FS synthesis: the passthrough VS plus the self-dot FS. */
static bool
r300vk_qnorm_synthesize_shaders(struct r300vk_device *device,
                                struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_qnorm_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesize the fullscreen-quad VS + texture-sampling FS pair that lowers an
 * identity-map compute kernel onto the compute-as-raster substrate.  The VS
 * passes through a POSITION attribute and one GENERIC varying (texture
 * coordinates); the FS samples PIPE_TEXTURE_2D (NEAREST configured at the
 * sampler-state binding point at dispatch replay time) and writes the texel
 * to the bound color RT.  Both CSOs are cached on the pipeline; the existing
 * destroy path frees vs_cso / fs_cso conditionally.
 *
 * util_make_fragment_tex_shader is the Mesa-canonical helper in
 * src/gallium/auxiliary/util/u_simple_shaders.c (TGSI-based).  Returning false
 * signals a synthesis failure that demotes the pipeline back to a no-op
 * compute object so vkCreateComputePipelines still succeeds with the kernel
 * admitted, just without the identity-map lowering. */
static bool
r300vk_identity_map_synthesize_shaders(struct r300vk_device *device,
                                        struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   /* Cached gallium state CSOs live on the device so every identity-map
    * pipeline reuses them.  Initialize on demand from the first identity-map
    * synthesis; subsequent calls find them populated. */
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = util_make_fragment_tex_shader(
                    pipe, TGSI_TEXTURE_2D,
                    TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                    false /* use_txf: NEAREST sample, not integer fetch */,
                    true  /* use_persp: perspective-correct interpolation */);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the VS + FS pair for the blend-add reduction lowering.  The
 * shaders are structurally identical to r300vk_identity_map_synthesize_shaders
 * -- a vertex_passthrough VS feeding a single-sampler FS that samples the
 * bound 2D texture and writes the texel to OUT[0] COLOR.  The semantic
 * difference between identity-map and blend-acc reduction lives ENTIRELY in
 * the orchestrator at dispatch time:
 *
 *   - identity-map orchestrator binds the output buffer as a W x H RT
 *     matching the input texture extent and draws a fullscreen quad with
 *     blending DISABLED.  Each fragment writes one texel exactly.
 *
 *   - blend-acc orchestrator binds the output buffer as a 1 x M RT (M =
 *     histogram bin count, derived from the output buffer's element count
 *     at orchestrator time) and draws N point primitives at positions
 *     (gid & MASK, 0) with blending ENABLED in COMB_FCN_ADD /
 *     blend_func = (ONE, ONE).  The blend hardware accumulates N writes
 *     into M bins.
 *
 * Sharing the synthesis lets the blend-acc orchestrator reuse the
 * device-cached sampler / raster CSOs the identity-map lowering already
 * populates and adds only the blend-state difference, the same reuse pattern
 * the binary-map synthesis uses for its own state CSOs. */
/* Initialise the device-cached blend-acc-reduction blend state CSO on
 * demand (the only state difference from the identity-map CSO set).
 * Configures the RB3D blend hardware path the compute-as-raster substrate
 * confirmed: COMB_FCN_ADD with blend_func = (ONE, ONE) accumulates dest+src
 * into the RT cell.  The other CSOs (rasterizer / dsa / sampler) come from
 * r300vk_device_init_identity_map_state -- the blend-acc orchestrator binds
 * those unchanged from the identity-map set. */
bool
r300vk_device_init_blend_acc_reduction_state(struct r300vk_device *device);

bool
r300vk_device_init_blend_acc_reduction_state(struct r300vk_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;
   if (device->blend_acc_reduction_blend_cso)
      return true;

   struct pipe_blend_state blend = {0};
   blend.rt[0].blend_enable     = 1;
   blend.rt[0].rgb_func         = PIPE_BLEND_ADD;
   blend.rt[0].alpha_func       = PIPE_BLEND_ADD;
   blend.rt[0].rgb_src_factor   = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].rgb_dst_factor   = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ONE;
   blend.rt[0].colormask        = PIPE_MASK_RGBA;
   device->blend_acc_reduction_blend_cso =
      pipe->create_blend_state(pipe, &blend);
   return device->blend_acc_reduction_blend_cso != NULL;
}

/* Synthesise the blend-acc-reduction fragment program: a single-MOV
 * passthrough from the GENERIC varying (carrying the per-fragment value
 * the orchestrator baked into the per-point VBO entry) to OUT[0] COLOR.
 * The RB3D blend hardware sums the per-fragment color into the bin cell
 * of the 1xM output RT.  Cost: 1 MOV ALU / 64-slot R300 PFS budget.
 *
 * This is structurally distinct from the identity-map FS, which does
 * TEX + MOV (samples the input texture); the blend-acc FS doesn't sample
 * because the value rides on the per-vertex attribute through the
 * rasterizer interpolator. */
static void *
r300vk_synthesize_blend_acc_reduction_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src in_color = ureg_DECL_fs_input(
      ureg, TGSI_SEMANTIC_GENERIC, 0, TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out_color = ureg_DECL_output(
      ureg, TGSI_SEMANTIC_COLOR, 0);
   ureg_MOV(ureg, out_color, in_color);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_blend_acc_reduction_synthesize_shaders(struct r300vk_device *device,
                                              struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;
   if (!r300vk_device_init_blend_acc_reduction_state(device))
      return false;

   /* The VS is the same vertex-passthrough shape as the identity-map and
    * binary-map paths: 2 attributes (POSITION + GENERIC) feed the
    * rasterizer.  The GENERIC attribute carries the per-vertex color the
    * orchestrator stages into the VBO (a packed RGBA8 of the kernel's per-gid
    * input value). */
    pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
    if (!pl->vs_cso)
      return false;

    pl->fs_cso = r300vk_synthesize_blend_acc_reduction_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the ZPASS-reduction fragment program: one MOV from the
 * GENERIC predicate varying (carrying a per-vertex predicate value the
 * orchestrator baked into the VBO: 1.0 = survive, 0.0 = discard) to a
 * temp, then KILL_IF on (predicate <= 0).  Surviving fragments write a
 * constant white color to OUT[0] (the color is irrelevant -- only the
 * ZPASS counter matters).  Cost: 1 MOV + 1 KILL_IF + 1 MOV out = 3
 * ALU / 64-slot R300 PFS budget.
 *
 * Mesa's tgsi_ureg has no ureg_KILL_IF helper, so we emit through the
 * TGSI macro path: ureg_insn with TGSI_OPCODE_KILL_IF takes one source
 * and discards the fragment when src.x < 0.  We negate the predicate
 * before emitting so KILL_IF(-predicate) discards when predicate < 0 --
 * we want discard when predicate == 0, but the canonical r300 predicate
 * convention here is "1.0 = pass, 0.0 = kill"; KILL_IF(-1.0) does NOT
 * trigger discard (negative-of-positive is negative, KILL_IF discards
 * on negative, so KILL_IF(-1.0) discards), so KILL_IF(predicate-0.5)
 * gives the right shape: discard when predicate < 0.5 (i.e. 0.0 baked
 * value), pass when predicate >= 0.5 (i.e. 1.0). */
static void *
r300vk_synthesize_zpass_reduction_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src in_pred = ureg_DECL_fs_input(
      ureg, TGSI_SEMANTIC_GENERIC, 0, TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst tmp = ureg_DECL_temporary(ureg);
   struct ureg_dst out_color = ureg_DECL_output(
      ureg, TGSI_SEMANTIC_COLOR, 0);
   /* TGSI KILL_IF discards when ANY of src.x/y/z/w is negative.  The
    * baked predicate only lives in the GENERIC varying's x channel;
    * the y/z/w channels follow the GL/D3D convention (0, 0, 1) for an
    * unwritten varying.  A naive `tmp = in_pred - 0.5; KILL_IF tmp`
    * would compute tmp.y = -0.5 and kill EVERY fragment regardless of
    * predicate, returning ZPASS counter = 0.  Broadcasting the predicate
    * to all four channels before the subtract gives KILL_IF
    * (predicate-0.5, predicate-0.5, predicate-0.5, predicate-0.5):
    * discard when predicate < 0.5 (the 0.0-baked discard case), pass when
    * predicate >= 0.5. */
   struct ureg_src half = ureg_imm1f(ureg, 0.5f);
   struct ureg_src pred_xxxx =
      ureg_scalar(in_pred, TGSI_SWIZZLE_X);
   ureg_ADD(ureg, tmp, pred_xxxx, ureg_negate(half));
   ureg_KILL_IF(ureg, ureg_src(tmp));
   /* Surviving fragments write white -- color content doesn't matter for
    * the ZPASS count, just that A fragment lands and the depth/stencil
    * unit increments the counter.  Reusing the predicate varying (which
    * is 1.0 for survivors anyway) keeps the program minimal. */
   ureg_MOV(ureg, out_color, pred_xxxx);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_zpass_reduction_synthesize_shaders(struct r300vk_device *device,
                                          struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   /* Same vertex-passthrough as the other compute-as-raster lowerings:
    * 2 attributes (POSITION + GENERIC predicate-value). */
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_zpass_reduction_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the multipass ping-pong scan fragment program: one per-pass FS
 * the orchestrator binds for every dependent FBO pass.  Each pass samples the
 * prior pass's render target (NEAREST) at the fragment's GENERIC texcoord and
 * writes the texel doubled -- a 1-TEX + 1-MUL shape.  The orchestrator runs
 * this FS pass_count times, swapping the sampler/target RT pair each pass, so
 * the texel doubles once per pass and lands at in * 2^pass_count.
 *
 * Supported shape: the FS hard-codes the doubling step, matching the probe
 * kernel `x = x * 2u`.  Other per-iteration scales need a distinct lowering
 * shape keyed by step_op; the doubling-only synthesis mirrors the iadd-first
 * scoping of the blend-acc reduction. */
static void *
r300vk_synthesize_multipass_scan_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp = ureg_DECL_temporary(ureg);

   /* Sample the prior pass's RT, then double every channel.  The per-byte
    * UNORM8 doubling matches the kernel's uint *2 only while each byte stays
    * below 256 / 2^pass_count (the probe seeds inputs within that bound); a
    * channel that would exceed 1.0 clamps, which the readback oracle catches
    * as a mismatch rather than silently passing. */
   ureg_TEX(ureg, ureg_writemask(tmp, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp);
   struct ureg_src two = ureg_imm4f(ureg, 2.0f, 2.0f, 2.0f, 2.0f);
   ureg_MUL(ureg, out, ureg_src(tmp), two);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the multipass-scan VS + per-pass FS.  Same vertex-passthrough as
 * the other compute-as-raster lowerings (POSITION + GENERIC texcoord); the FS
 * is the doubling sampler program the orchestrator rebinds for each ping-pong
 * pass. */
static bool
r300vk_multipass_scan_synthesize_shaders(struct r300vk_device *device,
                                         struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_multipass_scan_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the predicated masked-store fragment program.  Two samplers:
 * sampler 0 is the predicate texture, sampler 1 is the value texture; both
 * carry the per-element SSBO contents wrapped as PIPE_TEXTURE_2D (NEAREST).
 * The FS samples the predicate, discards the fragment when the predicate is
 * false, samples the value, and writes it.  A discarded fragment performs no
 * ROP write, so the render target keeps whatever the orchestrator seeded into
 * it from out_data -- that is how a masked cell stays at its baseline.
 *
 * The predicate arrives as a UNORM8 texel, so a true predicate (kernel encodes
 * any non-zero low byte) samples to >= 1/255, and a false one to 0.  TGSI
 * KILL_IF discards when ANY of src.x/y/z/w is negative, so the threshold is
 * pred_x - 1/512: 0 -> -1/512 < 0 -> discard (masked); 1/255 -> positive ->
 * pass.  1/512 sits below the smallest non-zero UNORM8 step (1/255), so any
 * non-zero predicate byte passes -- matching the kernel's `!= 0u`.  The
 * predicate is broadcast to all four channels before the subtract (the
 * unwritten y/z/w of a GENERIC varying default to (0,0,1), which would make a
 * naive per-vector subtract discard every fragment).
 *
 * Cost: 2 TEX + 1 ADD + 1 KILL_IF + 1 MOV. */
static void *
r300vk_synthesize_predicated_store_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp_pred = ureg_DECL_sampler(ureg, 0);
   struct ureg_src samp_val  = ureg_DECL_sampler(ureg, 1);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   ureg_DECL_sampler_view(ureg, 1, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out      = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst tmp_pred = ureg_DECL_temporary(ureg);
   struct ureg_dst tmp_val  = ureg_DECL_temporary(ureg);
   struct ureg_dst kill     = ureg_DECL_temporary(ureg);

   ureg_TEX(ureg, ureg_writemask(tmp_pred, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_pred);
   struct ureg_src thresh = ureg_imm1f(ureg, 1.0f / 512.0f);
   struct ureg_src pred_xxxx =
      ureg_scalar(ureg_src(tmp_pred), TGSI_SWIZZLE_X);
   ureg_ADD(ureg, kill, pred_xxxx, ureg_negate(thresh));
   ureg_KILL_IF(ureg, ureg_src(kill));

   ureg_TEX(ureg, ureg_writemask(tmp_val, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp_val);
   ureg_MOV(ureg, out, ureg_src(tmp_val));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the predicated masked-store VS + FS pair.  Same fullscreen-quad
 * vertex passthrough and device-cached state CSOs as the identity / binary
 * lowerings; only the KILL_IF FS differs. */
static bool
r300vk_predicated_store_synthesize_shaders(struct r300vk_device *device,
                                           struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_predicated_store_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Synthesise the box-3 multi-tap gather fragment program:
 *   ADD  coord_l, IN[0], -CONST[0]   (texcoord one texel left)
 *   ADD  coord_r, IN[0],  CONST[0]   (texcoord one texel right)
 *   TEX  t_c, IN[0],     SAMP[0]     (center tap, in[gid])
 *   TEX  t_l, coord_l,   SAMP[0]     (left   tap, in[gid-1])
 *   TEX  t_r, coord_r,   SAMP[0]     (right  tap, in[gid+1])
 *   ADD  acc, t_c, t_l
 *   ADD  OUT[0], acc, t_r
 *   END
 *
 * One sampler sampled at three neighborhood offsets, summed in the FP24 ALU.
 * CONST[0] carries the neighbor texel displacement (1/width, 0, 0, 0): the
 * element count <= 2048 lays out as a single texture row (height == 1, per
 * derive_raster_extent in r300vk_identity_map.c), so the displacement is
 * purely in normalized texcoord X and CONST[0].y stays 0 to keep the offset
 * taps in row 0.  width is a dispatch-time quantity (the grid size arrives at
 * CmdDispatch, not pipeline-create), so the orchestrator uploads CONST[0] per
 * dispatch via set_constant_buffer; the FS adds it to the interpolated
 * texcoord.  CLAMP_TO_EDGE on the device sampler defines the boundary taps:
 * gid 0's left tap and gid (N-1)'s right tap clamp to the edge texel, matching
 * a 1D edge-clamped analytic convolution.
 *
 * The FS emits a FIXED box-3 regardless of the detected tap_count -- the
 * canonical-kernel contract (the detector recognizes the N-tap shape; the
 * orchestrator and the probe agree on box-3).  The three TEX read distinct
 * offset coordinates, so CSE does not collapse them to one fetch.
 *
 * Bit-exactness: explicitly performs the 32-bit integer addition carry chain
 * in FP24.  The 0.0-1.0 UNORM8 input samples are scaled to 0-255, summed
 * per-channel with carry extraction via TRUNC and remainders via FRC, then
 * output as exact UNORM8 values.  FP24 mantissa (16-bit) exactly represents
 * the 0..767 intermediate channel sums.
 *
 * Cost: 3 TEX + ~26 ALU = ~30/96 of the R300 PFS budget. */
static void *
r300vk_synthesize_multitap_gather_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);

   struct ureg_src delta = ureg_DECL_constant(ureg, 0);
   struct ureg_src tex = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                            TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out     = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst coord_l = ureg_DECL_temporary(ureg);
   struct ureg_dst coord_r = ureg_DECL_temporary(ureg);
   struct ureg_dst t_c     = ureg_DECL_temporary(ureg);
   struct ureg_dst t_l     = ureg_DECL_temporary(ureg);
   struct ureg_dst t_r     = ureg_DECL_temporary(ureg);
   struct ureg_dst s0      = ureg_DECL_temporary(ureg);
   struct ureg_dst s1      = ureg_DECL_temporary(ureg);
   struct ureg_dst s2      = ureg_DECL_temporary(ureg);
   struct ureg_dst carry   = ureg_DECL_temporary(ureg);

   ureg_ADD(ureg, coord_l, tex, ureg_negate(delta));
   ureg_ADD(ureg, coord_r, tex, delta);

   ureg_TEX(ureg, ureg_writemask(t_c, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp);
   ureg_TEX(ureg, ureg_writemask(t_l, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, ureg_src(coord_l), samp);
   ureg_TEX(ureg, ureg_writemask(t_r, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, ureg_src(coord_r), samp);

   /* Scale 0.0-1.0 UNORM8 to 0-255. */
   struct ureg_src scale255 = ureg_imm1f(ureg, 255.0f);
   ureg_MUL(ureg, t_c, ureg_src(t_c), scale255);
   ureg_MUL(ureg, t_l, ureg_src(t_l), scale255);
   ureg_MUL(ureg, t_r, ureg_src(t_r), scale255);

   /* s0 = t_c + t_l + t_r */
   ureg_ADD(ureg, s0, ureg_src(t_c), ureg_src(t_l));
   ureg_ADD(ureg, s0, ureg_src(s0), ureg_src(t_r));

   struct ureg_src inv256 = ureg_imm1f(ureg, 1.0f / 256.0f);
   struct ureg_src scale_out = ureg_imm1f(ureg, 256.0f / 255.0f);

   /* Carry chain: X -> Y -> Z -> W. */
   /* X channel: remainder s0.x % 256, carry s0.x / 256. */
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_X), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_X), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_X), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_X), ureg_src(s2), scale_out);

   /* Y channel: s0.y + carry.x */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Y),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_X));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Y), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_Y), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_Y), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_Y), ureg_src(s2), scale_out);

   /* Z channel: s0.z + carry.y */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Z),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_Y));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_Z), inv256);
   ureg_TRUNC(ureg, ureg_writemask(carry, TGSI_WRITEMASK_Z), ureg_src(s1));
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_Z), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_Z), ureg_src(s2), scale_out);

   /* W channel: s0.w + carry.z (no carry-out needed). */
   ureg_ADD(ureg, ureg_writemask(s0, TGSI_WRITEMASK_W),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_W),
            ureg_scalar(ureg_src(carry), TGSI_SWIZZLE_Z));
   ureg_MUL(ureg, ureg_writemask(s1, TGSI_WRITEMASK_W),
            ureg_scalar(ureg_src(s0), TGSI_SWIZZLE_W), inv256);
   ureg_FRC(ureg, ureg_writemask(s2, TGSI_WRITEMASK_W), ureg_src(s1));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_W), ureg_src(s2), scale_out);

   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesise the multi-tap gather VS + FS pair.  Same fullscreen-quad vertex
 * passthrough (POSITION + one GENERIC texcoord) and device-cached state CSOs
 * as the identity / binary lowerings; only the box-3 FS differs.  The FS reads
 * CONST[0] (the neighbor texel delta) which the orchestrator uploads per
 * dispatch. */
static bool
r300vk_multitap_gather_synthesize_shaders(struct r300vk_device *device,
                                          struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r300vk_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r300vk_synthesize_multitap_gather_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}



static bool
r300vk_synthesize_compute_shaders(struct r300vk_device *device,
                                  struct r300vk_pipeline *pl)
{
   if (pl->identity_map.is_identity_map) {
      if (!r300vk_identity_map_synthesize_shaders(device, pl))
         pl->identity_map.is_identity_map = false;
      return true;
   }
   if (pl->binary_map.is_binary_map) {
      if (!r300vk_binary_map_synthesize_shaders(device, pl))
         pl->binary_map.is_binary_map = false;
      return true;
   }
   if (pl->dp4.is_dp4) {
      if (!r300vk_dp4_synthesize_shaders(device, pl))
         pl->dp4.is_dp4 = false;
      return true;
   }
   if (pl->qmul.is_qmul) {
      if (!r300vk_qmul_synthesize_shaders(device, pl))
         pl->qmul.is_qmul = false;
      return true;
   }
   if (pl->qrotate.is_qrotate) {
      if (!r300vk_qrotate_synthesize_shaders(device, pl))
         pl->qrotate.is_qrotate = false;
      return true;
   }
   if (pl->qconj.is_qconj) {
      if (!r300vk_qconj_synthesize_shaders(device, pl))
         pl->qconj.is_qconj = false;
      return true;
   }
   if (pl->qnorm.is_qnorm) {
      if (!r300vk_qnorm_synthesize_shaders(device, pl))
         pl->qnorm.is_qnorm = false;
      return true;
   }
   if (pl->blend_acc_reduction.is_blend_acc_reduction) {
      if (!r300vk_blend_acc_reduction_synthesize_shaders(device, pl))
         pl->blend_acc_reduction.is_blend_acc_reduction = false;
      return true;
   }
   if (pl->zpass_reduction.is_zpass_reduction) {
      if (!r300vk_zpass_reduction_synthesize_shaders(device, pl))
         pl->zpass_reduction.is_zpass_reduction = false;
      return true;
   }
   if (pl->multipass_scan.is_multipass_scan) {
      if (!r300vk_multipass_scan_synthesize_shaders(device, pl))
         pl->multipass_scan.is_multipass_scan = false;
      return true;
   }
   if (pl->predicated_store.is_predicated_store) {
      if (!r300vk_predicated_store_synthesize_shaders(device, pl))
         pl->predicated_store.is_predicated_store = false;
      return true;
   }
   if (pl->multitap_gather.is_multitap_gather) {
      if (!r300vk_multitap_gather_synthesize_shaders(device, pl))
         pl->multitap_gather.is_multitap_gather = false;
      return true;
   }

   return true;
}

static VkResult
r300vk_create_one_compute_pipeline(struct r300vk_device *device,
                                    const VkComputePipelineCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct r300vk_pipeline **out_pipeline,
                                    uint32_t i)
{
   struct r300_compute_admission adm;
   struct r300_compute_identity_pattern ident = {0};
   struct r300_compute_binary_map_pattern binmap = {0};
   struct r300_compute_blend_acc_reduction_pattern blendacc = {0};
   struct r300_compute_zpass_reduction_pattern zpass = {0};
   struct r300_compute_multipass_scan_pattern multiscan = {0};
   struct r300_compute_predicated_store_pattern predstore = {0};
   struct r300_compute_multitap_gather_pattern gather = {0};
   struct r300_compute_dp4_pattern dp4_pat = {0};
   struct r300_compute_qmul_pattern qmul_pat = {0};
   struct r300_compute_qrotate_pattern qrotate_pat = {0};
   struct r300_compute_qconj_pattern qconj_pat = {0};
   struct r300_compute_qnorm_pattern qnorm_pat = {0};
   uint32_t local_size[3];

   if (!r300vk_classify_compute_kernel(device, &pCreateInfo->stage,
                                       &adm, &ident, &binmap, &blendacc, &zpass,
                                       &multiscan, &predstore, &gather, &dp4_pat,
                                       &qmul_pat, &qrotate_pat,
                                       &qconj_pat, &qnorm_pat, local_size))
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: SPIR-V to NIR failed for compute kernel %u",
                       i);

   /* Kernels the RS482 substrate classifier cannot map to a raster pattern are
    * still valid VkPipeline objects.  vkCreateComputePipelines does not permit
    * VK_ERROR_FEATURE_NOT_PRESENT; the dispatch path returns
    * VK_ERROR_OUT_OF_DEVICE_MEMORY for inadmissible pipelines dispatched at
    * replay time, keeping the object lifecycle correct. */
   struct r300vk_pipeline *pl =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pl), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pl)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);
   pl->is_compute = true;
   pl->admission = adm;
   pl->identity_map = ident;
   pl->binary_map = binmap;
   pl->dp4 = dp4_pat;
   pl->qmul = qmul_pat;
   pl->qrotate = qrotate_pat;
   pl->qconj = qconj_pat;
   pl->qnorm = qnorm_pat;
   pl->blend_acc_reduction = blendacc;
   pl->zpass_reduction = zpass;
   pl->multipass_scan = multiscan;
   pl->predicated_store = predstore;
   pl->multitap_gather = gather;
   pl->local_size_x = local_size[0];
   pl->local_size_y = local_size[1];
   pl->local_size_z = local_size[2];

   r300vk_synthesize_compute_shaders(device, pl);

   *out_pipeline = pl;
   return VK_SUCCESS;
}


VkResult
r300vk_CreateComputePipelines(VkDevice _device,
                              VkPipelineCache pipelineCache,
                              uint32_t createInfoCount,
                              const VkComputePipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   (void)pipelineCache;

   if (createInfoCount == 0)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   if (!device->hybrid_compute_enabled)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r300vk: compute is not exposed (set "
                       R300VK_HYBRID_COMPUTE_ENV "=1 for the experimental "
                       "hybrid-compute path)");

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct r300vk_pipeline *pl = NULL;
      VkResult result = r300vk_create_one_compute_pipeline(device, &pCreateInfos[i],
                                                            pAllocator, &pl, i);
      if (result != VK_SUCCESS)
         return result;
      pPipelines[i] = r300vk_pipeline_to_handle(pl);
   }

   return VK_SUCCESS;
}

void
r300vk_DestroyPipeline(VkDevice _device,
                        VkPipeline _pipeline,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_pipeline, pl, _pipeline);
   if (!pl)
      return;

   if (pl->vs_cso)
      device->pipe->delete_vs_state(device->pipe, pl->vs_cso);
   if (pl->fs_cso)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso);
   if (pl->blend_cso)
      device->pipe->delete_blend_state(device->pipe, pl->blend_cso);
   if (pl->rasterizer_cso)
      device->pipe->delete_rasterizer_state(device->pipe, pl->rasterizer_cso);
   if (pl->dsa_cso)
      device->pipe->delete_depth_stencil_alpha_state(device->pipe, pl->dsa_cso);
   if (pl->velems_cso)
      device->pipe->delete_vertex_elements_state(device->pipe, pl->velems_cso);

   vk_object_base_finish(&pl->base);
   vk_free2(&device->vk.alloc, pAllocator, pl);
}
