/*
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_pipeline.h"
#include "r300vk_cmd_buffer.h"
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
 * pay for a clone + r300_optimize_nir (the same optimization pass used by
 * r300_create_*_state) to fold the loop-bounded case before judging it dynamic
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

   /* r300 has no indexable register file, so a dynamic index into a local
    * array or matrix (mat3 m; m[i], or float a[4]; a[i]) cannot be emitted:
    * the function_temp indirect deref reaches nir_lower_locals_to_regs inside
    * nir_to_rc, which cannot represent indirect register addressing, and the
    * compile aborts.  The native GL path never hits this -- st/mesa lowers
    * function_temp indirects up front because r300 advertises no indirect-temp
    * addressing -- but the SPIR-V path bypasses st, so the deref survives.
    * Lower it here to the same if/else selection trees nir_to_rc already
    * applies to shader_in, for both local temps and (dynamically-indexed)
    * shader outputs.  Runs before the constant/UBO lowering so a lowered
    * select chain sees the original loads. */
   NIR_PASS(_, nir, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_function_temp | nir_var_shader_out, UINT32_MAX);

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
   uint32_t n = 0;
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

   memcpy(pl->velems_template, ve, sizeof(ve));
   pl->velems_count = velem_count;
   pl->velems_cso =
      device->pipe->create_vertex_elements_state(device->pipe, velem_count, ve);
   if (!pl->velems_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
   return VK_SUCCESS;
}

/* VkBlendFactor and PIPE_BLENDFACTOR_* enumerate differently (gallium splits
 * the inverted factors into a high band), so the map is explicit.  Dual-source
 * SRC1 factors are unreachable: the dualSrcBlend feature stays false. */
static unsigned
r300vk_blend_factor_to_pipe(VkBlendFactor f)
{
   switch (f) {
   case VK_BLEND_FACTOR_ONE:                 return PIPE_BLENDFACTOR_ONE;
   case VK_BLEND_FACTOR_SRC_COLOR:           return PIPE_BLENDFACTOR_SRC_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA:           return PIPE_BLENDFACTOR_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA:           return PIPE_BLENDFACTOR_DST_ALPHA;
   case VK_BLEND_FACTOR_DST_COLOR:           return PIPE_BLENDFACTOR_DST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:  return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
   case VK_BLEND_FACTOR_CONSTANT_COLOR:      return PIPE_BLENDFACTOR_CONST_COLOR;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:      return PIPE_BLENDFACTOR_CONST_ALPHA;
   case VK_BLEND_FACTOR_ZERO:                return PIPE_BLENDFACTOR_ZERO;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      return PIPE_BLENDFACTOR_INV_CONST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
   default:                                  return PIPE_BLENDFACTOR_ONE;
   }
}

/* VkBlendOp and PIPE_BLEND_* share value order (ADD=0 .. MAX=4); proven
 * statically like the compare-op map. */
static unsigned
r300vk_blend_op_to_pipe(VkBlendOp op)
{
   STATIC_ASSERT((unsigned)VK_BLEND_OP_ADD == PIPE_BLEND_ADD &&
                 (unsigned)VK_BLEND_OP_SUBTRACT == PIPE_BLEND_SUBTRACT &&
                 (unsigned)VK_BLEND_OP_REVERSE_SUBTRACT == PIPE_BLEND_REVERSE_SUBTRACT &&
                 (unsigned)VK_BLEND_OP_MIN == PIPE_BLEND_MIN &&
                 (unsigned)VK_BLEND_OP_MAX == PIPE_BLEND_MAX);
   return (unsigned)op <= PIPE_BLEND_MAX ? (unsigned)op : PIPE_BLEND_ADD;
}

unsigned
r300vk_cull_mode_to_pipe(VkCullModeFlags cull)
{
   switch (cull) {
   case VK_CULL_MODE_FRONT_BIT:          return PIPE_FACE_FRONT;
   case VK_CULL_MODE_BACK_BIT:           return PIPE_FACE_BACK;
   case VK_CULL_MODE_FRONT_AND_BACK:     return PIPE_FACE_FRONT_AND_BACK;
   default:                              return PIPE_FACE_NONE;
   }
}

/* VkCompareOp and PIPE_FUNC_* share value order (NEVER=0 .. ALWAYS=7); keep
 * the static proof rather than an identity cast so an enum change breaks the
 * build, not rendering. */
unsigned
r300vk_compare_op_to_pipe(VkCompareOp op)
{
   STATIC_ASSERT((unsigned)VK_COMPARE_OP_NEVER == PIPE_FUNC_NEVER &&
                 (unsigned)VK_COMPARE_OP_LESS == PIPE_FUNC_LESS &&
                 (unsigned)VK_COMPARE_OP_EQUAL == PIPE_FUNC_EQUAL &&
                 (unsigned)VK_COMPARE_OP_LESS_OR_EQUAL == PIPE_FUNC_LEQUAL &&
                 (unsigned)VK_COMPARE_OP_GREATER == PIPE_FUNC_GREATER &&
                 (unsigned)VK_COMPARE_OP_NOT_EQUAL == PIPE_FUNC_NOTEQUAL &&
                 (unsigned)VK_COMPARE_OP_GREATER_OR_EQUAL == PIPE_FUNC_GEQUAL &&
                 (unsigned)VK_COMPARE_OP_ALWAYS == PIPE_FUNC_ALWAYS);
   return (unsigned)op & 0x7;
}

/* VkStencilOp and PIPE_STENCIL_OP_* do NOT share order (Vulkan places the
 * wrap variants after INVERT, gallium before), so this map is explicit. */
unsigned
r300vk_stencil_op_to_pipe(VkStencilOp op)
{
   switch (op) {
   case VK_STENCIL_OP_ZERO:                return PIPE_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE:             return PIPE_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return PIPE_STENCIL_OP_INCR;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return PIPE_STENCIL_OP_DECR;
   case VK_STENCIL_OP_INVERT:              return PIPE_STENCIL_OP_INVERT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP:  return PIPE_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP:  return PIPE_STENCIL_OP_DECR_WRAP;
   case VK_STENCIL_OP_KEEP:
   default:                                return PIPE_STENCIL_OP_KEEP;
   }
}

static void
r300vk_stencil_face_to_pipe(const VkStencilOpState *vk_face,
                            bool enabled,
                            struct pipe_stencil_state *out)
{
   out->enabled   = enabled;
   out->func      = r300vk_compare_op_to_pipe(vk_face->compareOp);
   out->fail_op   = r300vk_stencil_op_to_pipe(vk_face->failOp);
   out->zpass_op  = r300vk_stencil_op_to_pipe(vk_face->passOp);
   out->zfail_op  = r300vk_stencil_op_to_pipe(vk_face->depthFailOp);
   out->valuemask = (uint8_t)vk_face->compareMask;
   out->writemask = (uint8_t)vk_face->writeMask;
}

static VkResult
r300vk_init_graphics_pipeline_cso_state(struct r300vk_device *device,
                                        struct r300vk_pipeline *pl,
                                        const VkGraphicsPipelineCreateInfo *info)
{
   /* Translate the pipeline-static colour-blend state for the single render
    * target (maxColorAttachments == 1).  VkColorComponentFlags shares the
    * R/G/B/A bit order with PIPE_MASK_*; absent state (rasterizer discard or
    * no colour attachment) leaves blending off with a full writemask. */
   const VkPipelineColorBlendStateCreateInfo *vk_cb_state =
      info ? info->pColorBlendState : NULL;
   struct pipe_blend_state bs = {0};
   bs.rt[0].rgb_func        = PIPE_BLEND_ADD;
   bs.rt[0].rgb_src_factor  = PIPE_BLENDFACTOR_ONE;
   bs.rt[0].rgb_dst_factor  = PIPE_BLENDFACTOR_ZERO;
   bs.rt[0].alpha_func      = PIPE_BLEND_ADD;
   bs.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
   bs.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_ZERO;
   bs.rt[0].colormask       = PIPE_MASK_RGBA;
   STATIC_ASSERT(VK_COLOR_COMPONENT_R_BIT == PIPE_MASK_R &&
                 VK_COLOR_COMPONENT_G_BIT == PIPE_MASK_G &&
                 VK_COLOR_COMPONENT_B_BIT == PIPE_MASK_B &&
                 VK_COLOR_COMPONENT_A_BIT == PIPE_MASK_A);
   if (vk_cb_state && vk_cb_state->attachmentCount > 0 &&
       vk_cb_state->pAttachments) {
      const VkPipelineColorBlendAttachmentState *att =
         &vk_cb_state->pAttachments[0];
      bs.rt[0].blend_enable     = att->blendEnable;
      bs.rt[0].rgb_func         = r300vk_blend_op_to_pipe(att->colorBlendOp);
      bs.rt[0].rgb_src_factor   = r300vk_blend_factor_to_pipe(att->srcColorBlendFactor);
      bs.rt[0].rgb_dst_factor   = r300vk_blend_factor_to_pipe(att->dstColorBlendFactor);
      bs.rt[0].alpha_func       = r300vk_blend_op_to_pipe(att->alphaBlendOp);
      bs.rt[0].alpha_src_factor = r300vk_blend_factor_to_pipe(att->srcAlphaBlendFactor);
      bs.rt[0].alpha_dst_factor = r300vk_blend_factor_to_pipe(att->dstAlphaBlendFactor);
      bs.rt[0].colormask        = att->colorWriteMask & PIPE_MASK_RGBA;
   }
   /* VkLogicOp and PIPE_LOGICOP_* do NOT share value order (gallium follows
    * the GL truth-table encoding: COPY is 12 there, 3 in Vulkan), so the map
    * is explicit.  r300 implements all sixteen in the ROP unit
    * (RB3D_ROPCNTL). */
   if (vk_cb_state && vk_cb_state->logicOpEnable) {
      bs.logicop_enable = true;
      switch (vk_cb_state->logicOp) {
      case VK_LOGIC_OP_CLEAR:         bs.logicop_func = PIPE_LOGICOP_CLEAR; break;
      case VK_LOGIC_OP_AND:           bs.logicop_func = PIPE_LOGICOP_AND; break;
      case VK_LOGIC_OP_AND_REVERSE:   bs.logicop_func = PIPE_LOGICOP_AND_REVERSE; break;
      case VK_LOGIC_OP_COPY:          bs.logicop_func = PIPE_LOGICOP_COPY; break;
      case VK_LOGIC_OP_AND_INVERTED:  bs.logicop_func = PIPE_LOGICOP_AND_INVERTED; break;
      case VK_LOGIC_OP_NO_OP:         bs.logicop_func = PIPE_LOGICOP_NOOP; break;
      case VK_LOGIC_OP_XOR:           bs.logicop_func = PIPE_LOGICOP_XOR; break;
      case VK_LOGIC_OP_OR:            bs.logicop_func = PIPE_LOGICOP_OR; break;
      case VK_LOGIC_OP_NOR:           bs.logicop_func = PIPE_LOGICOP_NOR; break;
      case VK_LOGIC_OP_EQUIVALENT:    bs.logicop_func = PIPE_LOGICOP_EQUIV; break;
      case VK_LOGIC_OP_INVERT:        bs.logicop_func = PIPE_LOGICOP_INVERT; break;
      case VK_LOGIC_OP_OR_REVERSE:    bs.logicop_func = PIPE_LOGICOP_OR_REVERSE; break;
      case VK_LOGIC_OP_COPY_INVERTED: bs.logicop_func = PIPE_LOGICOP_COPY_INVERTED; break;
      case VK_LOGIC_OP_OR_INVERTED:   bs.logicop_func = PIPE_LOGICOP_OR_INVERTED; break;
      case VK_LOGIC_OP_NAND:          bs.logicop_func = PIPE_LOGICOP_NAND; break;
      case VK_LOGIC_OP_SET:           bs.logicop_func = PIPE_LOGICOP_SET; break;
      default:                        bs.logicop_func = PIPE_LOGICOP_COPY; break;
      }
   }
   pl->blend_cso = device->pipe->create_blend_state(device->pipe, &bs);
   if (!pl->blend_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   /* Translate the pipeline-static rasterization state.  Fields covered by
    * dynamic state still get their static value here: the replay
    * overlays only the R300VK_DYN_* bits the pipeline declared dynamic AND
    * the command buffer actually set. */
   const VkPipelineRasterizationStateCreateInfo *vk_rs =
      info ? info->pRasterizationState : NULL;
   struct pipe_rasterizer_state rs = {0};
   rs.fill_front  = PIPE_POLYGON_MODE_FILL;
   rs.fill_back   = PIPE_POLYGON_MODE_FILL;
   rs.cull_face   = PIPE_FACE_NONE;
   rs.front_ccw   = true;
   rs.depth_clip_near = true;
   rs.depth_clip_far  = true;
   /* Vulkan's scissor test always applies; the replay supplies the rectangle
    * (pipeline-static or CmdSetScissor) translated to live tile space. */
   rs.scissor     = true;
   rs.line_width  = 1.0f;
   rs.point_size  = 1.0f;
   if (vk_rs) {
      /* VK_EXT_depth_clip_enable: explicit clip control overrides the
       * default-on near/far clip (r300vk does not expose depthClampEnable,
       * so the implicit inverse-of-clamp rule never fires). */
      const VkPipelineRasterizationDepthClipStateCreateInfoEXT *clip_info =
         vk_find_struct_const(vk_rs->pNext,
                              PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
      if (clip_info) {
         rs.depth_clip_near = clip_info->depthClipEnable;
         rs.depth_clip_far  = clip_info->depthClipEnable;
      }
      /* VK_EXT_line_rasterization: r300 draws Bresenham lines natively and
       * the GL 2.1 line-stipple hardware backs stippled Bresenham; the
       * rectangular and smooth modes stay unadvertised. */
      const VkPipelineRasterizationLineStateCreateInfo *line_info =
         vk_find_struct_const(vk_rs->pNext,
                              PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO);
      if (line_info && line_info->stippledLineEnable) {
         rs.line_stipple_enable  = true;
         rs.line_stipple_factor  = line_info->lineStippleFactor
                                   ? line_info->lineStippleFactor - 1 : 0;
         rs.line_stipple_pattern = line_info->lineStipplePattern;
      }
      rs.cull_face = r300vk_cull_mode_to_pipe(vk_rs->cullMode);
      rs.front_ccw = vk_rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.line_width = vk_rs->lineWidth != 0.0f ? vk_rs->lineWidth : 1.0f;
      if (vk_rs->depthBiasEnable) {
         rs.offset_tri   = true;
         rs.offset_line  = true;
         rs.offset_point = true;
         rs.offset_units = vk_rs->depthBiasConstantFactor;
         rs.offset_scale = vk_rs->depthBiasSlopeFactor;
         rs.offset_clamp = vk_rs->depthBiasClamp;
      }
   }
   pl->rs_template = rs;
   pl->rasterizer_cso = device->pipe->create_rasterizer_state(device->pipe, &rs);
   if (!pl->rasterizer_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   /* Translate the pipeline-static depth/stencil state.  pDepthStencilState
    * is valid only when the pipeline rasterizes and a depth/stencil
    * attachment may be present; absent state leaves both tests disabled,
    * which is also Vulkan's no-attachment behavior. */
   const VkPipelineDepthStencilStateCreateInfo *vk_ds =
      info ? info->pDepthStencilState : NULL;
   struct pipe_depth_stencil_alpha_state dsa = {0};
   if (vk_ds) {
      dsa.depth_enabled   = vk_ds->depthTestEnable;
      dsa.depth_writemask = vk_ds->depthWriteEnable;
      dsa.depth_func      = r300vk_compare_op_to_pipe(vk_ds->depthCompareOp);
      r300vk_stencil_face_to_pipe(&vk_ds->front, vk_ds->stencilTestEnable,
                                  &dsa.stencil[0]);
      r300vk_stencil_face_to_pipe(&vk_ds->back, vk_ds->stencilTestEnable,
                                  &dsa.stencil[1]);
      pl->static_stencil_ref_front = vk_ds->front.reference;
      pl->static_stencil_ref_back  = vk_ds->back.reference;
   }
   pl->dsa_template = dsa;
   pl->dsa_cso = device->pipe->create_depth_stencil_alpha_state(device->pipe, &dsa);
   if (!pl->dsa_cso)
      return vk_error(device, VK_ERROR_INITIALIZATION_FAILED);

   const VkPipelineColorBlendStateCreateInfo *vk_cb =
      info ? info->pColorBlendState : NULL;
   if (vk_cb)
      memcpy(pl->static_blend_const, vk_cb->blendConstants,
             sizeof(pl->static_blend_const));

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
         /* The R300VK_DYN_* family: a vkCmdSet* value applies to a draw only
          * when the bound pipeline declared that state dynamic, so the replay
          * masks its merged Set* shadow with dyn_mask before overlaying the
          * pipeline's rs/dsa templates. */
         case VK_DYNAMIC_STATE_CULL_MODE:
            pl->dyn_mask |= R300VK_DYN_CULL;
            break;
         case VK_DYNAMIC_STATE_FRONT_FACE:
            pl->dyn_mask |= R300VK_DYN_FRONT_FACE;
            break;
         case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
            pl->dyn_mask |= R300VK_DYN_TOPOLOGY;
            break;
         case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
            pl->dyn_mask |= R300VK_DYN_DEPTH_TEST;
            break;
         case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
            pl->dyn_mask |= R300VK_DYN_DEPTH_WRITE;
            break;
         case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
            pl->dyn_mask |= R300VK_DYN_DEPTH_OP;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
            pl->dyn_mask |= R300VK_DYN_DEPTH_BOUNDS;
            break;
         case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
            pl->dyn_mask |= R300VK_DYN_STENCIL_TEST;
            break;
         case VK_DYNAMIC_STATE_STENCIL_OP:
            pl->dyn_mask |= R300VK_DYN_STENCIL_OP;
            break;
         case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
            pl->dyn_mask |= R300VK_DYN_STENCIL_CMP_MASK;
            break;
         case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
            pl->dyn_mask |= R300VK_DYN_STENCIL_WR_MASK;
            break;
         case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
            pl->dyn_mask |= R300VK_DYN_STENCIL_REF;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BIAS:
            pl->dyn_mask |= R300VK_DYN_DEPTH_BIAS;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
            pl->dyn_mask |= R300VK_DYN_DEPTH_BIAS_EN;
            break;
         case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
            pl->dyn_mask |= R300VK_DYN_BLEND_CONST;
            break;
         case VK_DYNAMIC_STATE_LINE_WIDTH:
            pl->dyn_mask |= R300VK_DYN_LINE_WIDTH;
            break;
         case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT:
            pl->dyn_mask |= R300VK_DYN_LINE_STIPPLE;
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
    * so command recording may enable it (in which case the app must supply a valid
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

   VkResult cso_res = r300vk_init_graphics_pipeline_cso_state(device, pl, info);
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
                               struct r300_compute_unary_map_pattern *unary,
                               struct r300_compute_blend_acc_reduction_pattern *blendacc,
                               struct r300_compute_zpass_reduction_pattern *zpass,
                               struct r300_compute_multipass_scan_pattern *multiscan,
                               struct r300_compute_predicated_store_pattern *predstore,
                               struct r300_compute_multitap_gather_pattern *gather,
                               struct r300_compute_dp4_pattern *dp4,
                               struct r300_compute_qmul_pattern *qmul,
                               struct r300_compute_qdiv_pattern *qdiv,
                               struct r300_compute_mat4vec_pattern *mat4vec,
                               struct r300_compute_qfmul_pattern *qfmul,
                               struct r300_compute_qrotate_pattern *qrotate,
                               struct r300_compute_qconj_pattern *qconj,
                               struct r300_compute_qnorm_pattern *qnorm,
                               struct r300_compute_qnormalize_pattern *qnormalize,
                               struct r300_compute_omul_pattern *omul,
                               struct r300_compute_oaddsub_pattern *oaddsub,
                               struct r300_compute_oconj_pattern *oconj,
                               struct r300_compute_onorm_pattern *onorm,
                               struct r300_compute_odiv_pattern *odiv,
                               struct r300_compute_otrans_pattern *otrans,
                               struct r300_compute_qfmadd_pattern *qfmadd,
                               struct r300_compute_qfmmul_pattern *qfmmul,
                               struct r300_compute_ieee16_classify_pattern *ieee16_classify,
                               struct r300_compute_ieee16_mul_pattern *ieee16_mul,
                               struct r300_compute_const_fill_pattern *constfill,
                               struct r300_compute_index_pattern *index_consumption,
                               struct r300_compute_affine_iota_pattern *affine_iota,
                               struct r300_compute_multilimb_mul_pattern *multilimb_mul,
                               struct r300_compute_cas_pattern *cas,
                               struct r300_compute_log4_pool_pattern *log4_pool,
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

   /* SPIR-V delivers gl_GlobalInvocationID as a system-value VARIABLE read
    * through load_deref; the index-consumption classifier and the affine-iota
    * detector walk the load_global_invocation_id intrinsic.  Lower the
    * variable form to the intrinsic before detection -- without this every
    * real kernel classifies INDEX_NONE and the index-exactness gate is
    * inert.  Plain system-value lowering only: the compute variant that
    * decomposes the global id into workgroup_id * size + local_id would
    * destroy the affine chain the classifier proves. */
   NIR_PASS(_, nir, nir_lower_system_values);

   /* Push-constant reads must reach the detectors as load_push_constant with
    * a foldable offset, not as opaque push_const derefs -- the unary-map
    * detector keys c0/c1 capture on that intrinsic. */
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      /* Fold the explicit_io offset arithmetic to inline constants: the
       * detectors capture bindings and push offsets only from
       * nir_src_is_const sources, and a SPIR-V access chain reaches here as
       * iadd/imul trees over immediates that copy-prop alone never folds. */
      NIR_PASS(progress, nir, nir_opt_constant_folding);
   } while (progress);

   /* Detector-eye view of the kernel: R300VK_DEBUG=classify_nir dumps the
    * exact NIR the classify + pattern detectors walk, the first thing to
    * read when a kernel that should match a raster verb dispatches as an
    * unknown-shape no-op. */
   {
      const char *dbg = getenv("R300VK_DEBUG");
      if (dbg && strstr(dbg, "classify_nir"))
         nir_print_shader(nir, stderr);
   }

   r300_nir_classify_compute(nir, adm);
   r300_nir_detect_identity_map(nir, ident);
   r300_nir_detect_binary_map(nir, binmap);
   r300_nir_detect_unary_map(nir, unary);
   r300_nir_detect_blend_acc_reduction(nir, blendacc);
   r300_nir_detect_zpass_reduction(nir, zpass);
   r300_nir_detect_multipass_scan_pattern(nir, multiscan);
   r300_nir_detect_predicated_store_pattern(nir, predstore);
   r300_nir_detect_multitap_gather_pattern(nir, gather);
   r300_nir_detect_dp4_pattern(nir, dp4);
   r300_nir_detect_qmul_pattern(nir, qmul);
   r300_nir_detect_qdiv_pattern(nir, qdiv);
   r300_nir_detect_mat4vec_pattern(nir, mat4vec);
   r300_nir_detect_qfmul_pattern(nir, qfmul);
   r300_nir_detect_qrotate_pattern(nir, qrotate);
   r300_nir_detect_qconj_pattern(nir, qconj);
   r300_nir_detect_qnorm_pattern(nir, qnorm);
   r300_nir_detect_qnormalize_pattern(nir, qnormalize);
   r300_nir_detect_omul_pattern(nir, omul);
   r300_nir_detect_oaddsub_pattern(nir, oaddsub);
   r300_nir_detect_oconj_pattern(nir, oconj);
   r300_nir_detect_onorm_pattern(nir, onorm);
   r300_nir_detect_odiv_pattern(nir, odiv);
   r300_nir_detect_otrans_pattern(nir, otrans);
   r300_nir_detect_qfmadd_pattern(nir, qfmadd);
   r300_nir_detect_qfmmul_pattern(nir, qfmmul);
   r300_nir_detect_ieee16_classify(nir, ieee16_classify);
   r300_nir_detect_ieee16_mul(nir, ieee16_mul);
   r300_nir_detect_const_fill_pattern(nir, constfill);
   r300_nir_classify_index_consumption(nir, index_consumption);
   r300_nir_detect_affine_iota_pattern(nir, affine_iota);
   r300_nir_detect_multilimb_mul_pattern(nir, multilimb_mul);
   r300_nir_detect_cas_pattern(nir, cas);
   r300_nir_detect_log4_pool_pattern(nir, log4_pool);

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

/* Synthesise the single-sampler affine fragment program for the unary-map
 * lowering: out = sample(in) * c0 + c1.
 *   TEX tmp, IN[0], SAMP[0]   (sample in, NEAREST)
 *   MUL tmp, tmp, c0.broadcast
 *   ADD OUT[0], tmp, c1.broadcast
 *   END
 * A literal constant bakes as an immediate from the detector's captured
 * value.  A push-derived constant has no value at pipeline-create time, so it
 * reads the constant file instead: the dispatch replay binds the 128-byte
 * push window at FS CONST[0], mapping push byte offset N to CONST[N/16]
 * component (N%16)/4.  MUL + ADD (not a single MAD) stays within the opcode
 * set the binary-map FS already uses.  Cost: 1 TEX + 2 ALU. */
static struct ureg_src
r300vk_unary_map_const_src(struct ureg_program *ureg, bool from_push,
                           uint16_t push_offset, float literal)
{
   if (!from_push)
      return ureg_imm4f(ureg, literal, literal, literal, literal);
   return ureg_scalar(ureg_DECL_constant(ureg, push_offset / 16),
                      (push_offset % 16) / 4);
}

static void *
r300vk_synthesize_unary_map_fs(struct pipe_context *pipe,
                               const struct r300_compute_unary_map_pattern *um)
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
   struct ureg_src c0s = r300vk_unary_map_const_src(
      ureg, um->mul_const_from_push, um->mul_const_push_offset, um->mul_const);
   struct ureg_src c1s = r300vk_unary_map_const_src(
      ureg, um->add_const_from_push, um->add_const_push_offset, um->add_const);

   ureg_TEX(ureg, ureg_writemask(tmp, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tex, samp);
   ureg_MUL(ureg, ureg_writemask(tmp, TGSI_WRITEMASK_XYZW),
            ureg_src(tmp), c0s);
   ureg_ADD(ureg, out, ureg_src(tmp), c1s);
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* Synthesize the unary-map VS + FS.  The dispatch replay uses the unary-map
 * metadata directly and binds the affine fragment program, so the same
 * fullscreen draw computes out = tex*c0 + c1 instead of a copy. */
static bool
r300vk_unary_map_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_unary_map_fs(pipe, &pl->unary_map);
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
   nir_shader *fs_nir = r300vk_build_dp4_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT], components);
   if (!fs_nir)
      return NULL;
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, fs_nir, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = fs_nir };
   void *fs_cso = pipe->create_fs_state(pipe, &state);
   if (!fs_cso)
      ralloc_free(fs_nir);
   return fs_cso;
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

/* QDIV FS: the quaternion-division fragment program built by r300vk_build_qdiv_fs_nir
 * -- one Hamilton product over the scaled conjugate of the divisor, four DP4s plus
 * the US RCP, to the FP16 color export.  Same finalize+CSO as QMUL. */
static void *
r300vk_synthesize_qdiv_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qdiv_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QDIV VS+FS synthesis: the passthrough VS shared with DP4 plus the division FS.
 * Two inputs, one output -- the same dispatch shape as QMUL. */
static bool
r300vk_qdiv_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_qdiv_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* MULTILIMB column FS: one convolution column of the 7-bit-limb u32
 * multiply.  Samples factor a (stage 0) and factor b (stage 1) as RGBA8
 * texels, recovers the four bytes with the SNAPPED form
 * floor(v * 255 + 0.5) -- the raw v * 255 product double-rounds through the
 * UNORM divide and can land a hair below the integer, which shatters every
 * downstream floor (the on-target probe measured exactly that; the
 * snap-before-arithmetic rule from the AFFINE_IOTA lane applies to sampled
 * bytes too) -- and extracts the five 7-bit limbs of each factor with
 * byte-local floor arithmetic (every intermediate <= 2^11, exact in FP24):
 *
 *    f0 = floor(B0/128)  g1 = floor(B1/64)  g2 = floor(B2/32)  g3 = floor(B3/16)
 *    l0 = B0 - 128 f0
 *    l1 = f0 + 2 B1 - 128 g1
 *    l2 = g1 + 4 B2 - 128 g2
 *    l3 = g2 + 8 B3 - 128 g3
 *    l4 = g3
 *
 * then evaluates c_k = sum_{i+j=k} la_i * lb_j (at most five terms, each
 * <= 127^2, so c_k <= 80645 < 2^17 stays exact) and byte-decomposes the
 * 17-bit column little-endian into the RGBA8 export.  The dispatch reads
 * the nine columns back and assembles the carries on the host. */
static void
multilimb_extract_limbs(struct ureg_program *ureg, struct ureg_src bytes,
                        struct ureg_dst floors, struct ureg_dst limbs01,
                        struct ureg_dst limbs234)
{
   /* floors = floor(bytes * (1/128, 1/64, 1/32, 1/16)) per lane. */
   struct ureg_src inv = ureg_imm4f(ureg, 1.0f / 128.0f, 1.0f / 64.0f,
                                    1.0f / 32.0f, 1.0f / 16.0f);
   struct ureg_dst t = ureg_DECL_temporary(ureg);
   ureg_MUL(ureg, t, bytes, inv);
   ureg_FLR(ureg, floors, ureg_src(t));

   struct ureg_src f = ureg_src(floors);
   struct ureg_src m128 = ureg_imm1f(ureg, -128.0f);
   /* limbs01.x = B0 - 128 f0 */
   ureg_MAD(ureg, ureg_writemask(limbs01, TGSI_WRITEMASK_X),
            ureg_scalar(f, TGSI_SWIZZLE_X), m128,
            ureg_scalar(bytes, TGSI_SWIZZLE_X));
   /* limbs01.y = (f0 + 2 B1) - 128 g1 */
   ureg_MAD(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(bytes, TGSI_SWIZZLE_Y), ureg_imm1f(ureg, 2.0f),
            ureg_scalar(f, TGSI_SWIZZLE_X));
   ureg_MAD(ureg, ureg_writemask(limbs01, TGSI_WRITEMASK_Y),
            ureg_scalar(f, TGSI_SWIZZLE_Y), m128,
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X));
   /* limbs234.x = (g1 + 4 B2) - 128 g2 */
   ureg_MAD(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(bytes, TGSI_SWIZZLE_Z), ureg_imm1f(ureg, 4.0f),
            ureg_scalar(f, TGSI_SWIZZLE_Y));
   ureg_MAD(ureg, ureg_writemask(limbs234, TGSI_WRITEMASK_X),
            ureg_scalar(f, TGSI_SWIZZLE_Z), m128,
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X));
   /* limbs234.y = (g2 + 8 B3) - 128 g3 */
   ureg_MAD(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(bytes, TGSI_SWIZZLE_W), ureg_imm1f(ureg, 8.0f),
            ureg_scalar(f, TGSI_SWIZZLE_Z));
   ureg_MAD(ureg, ureg_writemask(limbs234, TGSI_WRITEMASK_Y),
            ureg_scalar(f, TGSI_SWIZZLE_W), m128,
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X));
   /* limbs234.z = g3 */
   ureg_MOV(ureg, ureg_writemask(limbs234, TGSI_WRITEMASK_Z),
            ureg_scalar(f, TGSI_SWIZZLE_W));
   ureg_release_temporary(ureg, t);
}

static struct ureg_src
multilimb_limb(struct ureg_src limbs01, struct ureg_src limbs234, unsigned i)
{
   switch (i) {
   case 0: return ureg_scalar(limbs01, TGSI_SWIZZLE_X);
   case 1: return ureg_scalar(limbs01, TGSI_SWIZZLE_Y);
   case 2: return ureg_scalar(limbs234, TGSI_SWIZZLE_X);
   case 3: return ureg_scalar(limbs234, TGSI_SWIZZLE_Y);
   default: return ureg_scalar(limbs234, TGSI_SWIZZLE_Z);
   }
}

static void *
r300vk_synthesize_multilimb_fs(struct pipe_context *pipe, unsigned column)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp[2];
   for (unsigned s = 0; s < 2; s++) {
      samp[s] = ureg_DECL_sampler(ureg, s);
      ureg_DECL_sampler_view(ureg, s, TGSI_TEXTURE_2D,
                             TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                             TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   }
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);

   struct ureg_dst bytes_a = ureg_DECL_temporary(ureg);
   struct ureg_dst bytes_b = ureg_DECL_temporary(ureg);
   ureg_TEX(ureg, bytes_a, TGSI_TEXTURE_2D, tc, samp[0]);
   ureg_TEX(ureg, bytes_b, TGSI_TEXTURE_2D, tc, samp[1]);
   ureg_MAD(ureg, bytes_a, ureg_src(bytes_a), ureg_imm1f(ureg, 255.0f),
            ureg_imm1f(ureg, 0.5f));
   ureg_FLR(ureg, bytes_a, ureg_src(bytes_a));
   ureg_MAD(ureg, bytes_b, ureg_src(bytes_b), ureg_imm1f(ureg, 255.0f),
            ureg_imm1f(ureg, 0.5f));
   ureg_FLR(ureg, bytes_b, ureg_src(bytes_b));

   struct ureg_dst fl_a = ureg_DECL_temporary(ureg);
   struct ureg_dst la01 = ureg_DECL_temporary(ureg);
   struct ureg_dst la234 = ureg_DECL_temporary(ureg);
   struct ureg_dst fl_b = ureg_DECL_temporary(ureg);
   struct ureg_dst lb01 = ureg_DECL_temporary(ureg);
   struct ureg_dst lb234 = ureg_DECL_temporary(ureg);
   multilimb_extract_limbs(ureg, ureg_src(bytes_a), fl_a, la01, la234);
   multilimb_extract_limbs(ureg, ureg_src(bytes_b), fl_b, lb01, lb234);

   /* c = sum over the column's limb pairs. */
   struct ureg_dst c = ureg_DECL_temporary(ureg);
   bool first = true;
   for (unsigned i = 0; i < 5; i++) {
      if (column < i || column - i > 4)
         continue;
      const unsigned j = column - i;
      struct ureg_src ai = multilimb_limb(ureg_src(la01), ureg_src(la234), i);
      struct ureg_src bj = multilimb_limb(ureg_src(lb01), ureg_src(lb234), j);
      if (first) {
         ureg_MUL(ureg, ureg_writemask(c, TGSI_WRITEMASK_X), ai, bj);
         first = false;
      } else {
         ureg_MAD(ureg, ureg_writemask(c, TGSI_WRITEMASK_X), ai, bj,
                  ureg_scalar(ureg_src(c), TGSI_SWIZZLE_X));
      }
   }

   /* Byte-decompose c <= 80645 < 2^17 little-endian into the RGBA8 export. */
   struct ureg_dst e = ureg_DECL_temporary(ureg);
   struct ureg_dst v = ureg_DECL_temporary(ureg);
   struct ureg_src csrc = ureg_scalar(ureg_src(c), TGSI_SWIZZLE_X);
   ureg_MUL(ureg, ureg_writemask(v, TGSI_WRITEMASK_X), csrc,
            ureg_imm1f(ureg, 1.0f / 256.0f));
   ureg_FLR(ureg, ureg_writemask(v, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_X));
   ureg_MUL(ureg, ureg_writemask(v, TGSI_WRITEMASK_X), csrc,
            ureg_imm1f(ureg, 1.0f / 65536.0f));
   ureg_FLR(ureg, ureg_writemask(v, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_X));
   ureg_MAD(ureg, ureg_writemask(e, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Y),
            ureg_imm1f(ureg, -256.0f), csrc);
   ureg_MAD(ureg, ureg_writemask(e, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Z),
            ureg_imm1f(ureg, -256.0f),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Y));
   ureg_MOV(ureg, ureg_writemask(e, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Z));
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_XYZ), ureg_src(e),
            ureg_imm1f(ureg, 1.0f / 255.0f));
   ureg_MOV(ureg, ureg_writemask(out, TGSI_WRITEMASK_W),
            ureg_imm1f(ureg, 0.0f));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* MULTILIMB VS+FS synthesis: shared passthrough VS + one specialized column
 * program per convolution column.  Any column failing to compile clears the
 * pattern so the kernel falls to the binary-map or no-op lifecycle. */
static bool
r300vk_multilimb_synthesize_shaders(struct r300vk_device *device,
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

   for (unsigned k = 0; k < 9; k++) {
      pl->multilimb_fs[k] = r300vk_synthesize_multilimb_fs(pipe, k);
      if (!pl->multilimb_fs[k]) {
         for (unsigned j = 0; j < k; j++) {
            pipe->delete_fs_state(pipe, pl->multilimb_fs[j]);
            pl->multilimb_fs[j] = NULL;
         }
         pipe->delete_vs_state(pipe, pl->vs_cso);
         pl->vs_cso = NULL;
         return false;
      }
   }
   /* The dispatch validation prologue requires a non-NULL fs_cso; the
    * column draws bind multilimb_fs[k] explicitly. */
   pl->fs_cso = pl->multilimb_fs[0];
   return true;
}

/* log4 FS: one TEX through the LINEAR sampler IS the op, but the corner
 * coordinate must be CONSTRUCTED, not interpolated: the raw varying
 * carries sub-texel plane-equation error that nudges the 6-bit weights off
 * 16/64 (the first silicon run measured +-1 across most elements).  Snap
 * the integer output column from the varying -- x = floor(tc.x * W2) --
 * then build u = (2x + 1) / W from exact power-of-two constants
 * (CONST[0] = (W2, H2, 1/W, 1/H), uploaded per dispatch).  Only the R
 * channel carries payload; G/B/A are forced to zero so the raw RGBA8 row
 * copy yields the bare u32 result. */
static void *
r300vk_synthesize_log4_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;
   struct ureg_src cst = ureg_DECL_constant(ureg, 0);
   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst t = ureg_DECL_temporary(ureg);
   struct ureg_dst c = ureg_DECL_temporary(ureg);
   /* t.xy = floor(tc.xy * (W2, H2)): the snapped integer output cell. */
   ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_XY), tc, cst);
   ureg_FLR(ureg, ureg_writemask(t, TGSI_WRITEMASK_XY), ureg_src(t));
   /* c.xy = (2 * cell + 1) * (1/W, 1/H): the exact 2x2 corner. */
   ureg_MAD(ureg, ureg_writemask(c, TGSI_WRITEMASK_XY), ureg_src(t),
            ureg_imm1f(ureg, 2.0f), ureg_imm1f(ureg, 1.0f));
   ureg_MUL(ureg, ureg_writemask(c, TGSI_WRITEMASK_XY), ureg_src(c),
            ureg_swizzle(cst, TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W,
                         TGSI_SWIZZLE_Z, TGSI_SWIZZLE_W));
   ureg_TEX(ureg, t, TGSI_TEXTURE_2D, ureg_src(c), samp);
   ureg_MOV(ureg, ureg_writemask(out, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X));
   ureg_MOV(ureg, ureg_writemask(out, TGSI_WRITEMASK_YZW),
            ureg_imm1f(ureg, 0.0f));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_log4_synthesize_shaders(struct r300vk_device *device,
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
   pl->fs_cso = r300vk_synthesize_log4_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* CAS FS: guard' = (guard == C_expect) ? C_new : guard, decided BYTEWISE.
 * Sample the guard texel, snap the bytes (floor(v * 255 + 0.5) -- the
 * sampled-byte snap rule), compare all four channels against the expect
 * bytes with one vec4 SEQ (silicon-confirmed exact for byte operands),
 * combine the four lanes into one predicate with three multiplies, and
 * select per channel with out = guard + t * (new - guard).  Every operand
 * is a byte (<= 255) or a 0/1 predicate; all arithmetic exact in FP24. */
static void *
r300vk_synthesize_cas_fs(struct pipe_context *pipe, uint32_t expect,
                         uint32_t value_new)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);

   struct ureg_src expb = ureg_imm4f(ureg,
      (float)(expect & 0xFF), (float)((expect >> 8) & 0xFF),
      (float)((expect >> 16) & 0xFF), (float)((expect >> 24) & 0xFF));
   struct ureg_src newb = ureg_imm4f(ureg,
      (float)(value_new & 0xFF), (float)((value_new >> 8) & 0xFF),
      (float)((value_new >> 16) & 0xFF), (float)((value_new >> 24) & 0xFF));

   struct ureg_dst g = ureg_DECL_temporary(ureg);
   struct ureg_dst eq = ureg_DECL_temporary(ureg);
   struct ureg_dst t = ureg_DECL_temporary(ureg);
   struct ureg_dst d = ureg_DECL_temporary(ureg);

   ureg_TEX(ureg, g, TGSI_TEXTURE_2D, tc, samp);
   ureg_MAD(ureg, g, ureg_src(g), ureg_imm1f(ureg, 255.0f),
            ureg_imm1f(ureg, 0.5f));
   ureg_FLR(ureg, g, ureg_src(g));

   ureg_SEQ(ureg, eq, ureg_src(g), expb);
   ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(eq), TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(eq), TGSI_SWIZZLE_Y));
   ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(eq), TGSI_SWIZZLE_Z));
   ureg_MUL(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(eq), TGSI_SWIZZLE_W));

   ureg_ADD(ureg, d, newb, ureg_negate(ureg_src(g)));
   ureg_MAD(ureg, d, ureg_src(d), ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
            ureg_src(g));
   ureg_MUL(ureg, out, ureg_src(d), ureg_imm1f(ureg, 1.0f / 255.0f));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

static bool
r300vk_cas_synthesize_shaders(struct r300vk_device *device,
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
   pl->fs_cso = r300vk_synthesize_cas_fs(pipe, pl->cas.expect,
                                         pl->cas.value_new);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* AFFINE_IOTA FS: out[gid] = stride * gid + offset, the index materialized
 * in the FP24 fragment ALU.  The texel-unit varying interpolates to
 * (x + 0.5, y + 0.5) at fragment centres; the dispatch-known scalars ride
 * the fragment constant file as CONST[0] = (width, stride, offset, unused).
 * gid = floor(tc.y) * width + floor(tc.x); v = gid * stride + offset; the
 * integer result is byte-decomposed little-endian into the RGBA8 export:
 * r = v mod 256, g = floor(v/256) mod 256, b = floor(v/65536).  Every
 * intermediate is an exact FP24 integer while v <= 2^17 (the dispatch gate
 * bounds stride * (total - 1) + offset by exactly that), and the high
 * byte b = floor(v/65536) <= 2 needs no mod.  Divisions are by powers of
 * two -- exponent shifts, exact -- and floor is the FLR opcode. */
static void *
r300vk_synthesize_affine_iota_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src cst = ureg_DECL_constant(ureg, 0);
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst t = ureg_DECL_temporary(ureg);
   struct ureg_dst v = ureg_DECL_temporary(ureg);
   struct ureg_dst e = ureg_DECL_temporary(ureg);

   /* imm = (unused, 1/256, 1/65536, 1/255); imm2 = (-256, 0, -, -) */
   struct ureg_src imm = ureg_imm4f(ureg, 0.0f, 1.0f / 256.0f,
                                    1.0f / 65536.0f, 1.0f / 255.0f);
   struct ureg_src imm2 = ureg_imm4f(ureg, -256.0f, 0.0f, 0.0f, 0.0f);

   /* t.xy = floor(tc.xy): SNAP the interpolated texel-centre varying back to
    * the integer coordinate.  The rasterizer interpolant carries x + 0.5
    * plus a sub-texel plane-equation error (the RS482 probe measured cliff
    * flips at byte boundaries when the raw value fed the decompose), and
    * floor absorbs any error below half a texel, so every downstream
    * operand is an exact FP24 integer.  Snapping per axis matters: the
    * combined linear index exceeds 2^16 where a +0.5 round-bias would no
    * longer be representable, but per-axis coordinates stay <= 2048. */
   ureg_FLR(ureg, ureg_writemask(t, TGSI_WRITEMASK_XY), tc);
   /* t.x = t.y * width + t.x: the linear gid. */
   ureg_MAD(ureg, ureg_writemask(t, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_Y),
            ureg_scalar(cst, TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X));
   /* v.x = gid * stride + offset. */
   ureg_MAD(ureg, ureg_writemask(v, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(t), TGSI_SWIZZLE_X),
            ureg_scalar(cst, TGSI_SWIZZLE_Y),
            ureg_scalar(cst, TGSI_SWIZZLE_Z));
   /* v.y = floor(v.x / 256); v.z = floor(v.x / 65536). */
   ureg_MUL(ureg, ureg_writemask(e, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_X),
            ureg_scalar(imm, TGSI_SWIZZLE_Y));
   ureg_FLR(ureg, ureg_writemask(v, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(e), TGSI_SWIZZLE_X));
   ureg_MUL(ureg, ureg_writemask(e, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_X),
            ureg_scalar(imm, TGSI_SWIZZLE_Z));
   ureg_FLR(ureg, ureg_writemask(v, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(e), TGSI_SWIZZLE_X));
   /* e.x = v.x - 256 * v.y; e.y = v.y - 256 * v.z; e.z = v.z. */
   ureg_MAD(ureg, ureg_writemask(e, TGSI_WRITEMASK_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Y),
            ureg_scalar(imm2, TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_X));
   ureg_MAD(ureg, ureg_writemask(e, TGSI_WRITEMASK_Y),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Z),
            ureg_scalar(imm2, TGSI_SWIZZLE_X),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Y));
   ureg_MOV(ureg, ureg_writemask(e, TGSI_WRITEMASK_Z),
            ureg_scalar(ureg_src(v), TGSI_SWIZZLE_Z));
   /* out = (e.xyz, 0) / 255 -- the UNORM8 export round-trips each byte. */
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_XYZ), ureg_src(e),
            ureg_scalar(imm, TGSI_SWIZZLE_W));
   ureg_MOV(ureg, ureg_writemask(out, TGSI_WRITEMASK_W),
            ureg_scalar(imm2, TGSI_SWIZZLE_Y));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* AFFINE_IOTA VS+FS synthesis: shared passthrough VS + the index-affine FS.
 * The dispatch uploads (width, stride, offset) to CONST[0] and draws with a
 * texel-unit varying quad instead of the 0..1 fullscreen texcoord. */
static bool
r300vk_affine_iota_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_affine_iota_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* MAT4VEC FS: the general 4x4 vertex transform -- four DP4s of the per-element
 * vertex against the broadcast matrix rows, to the FP16 color export. */
static void *
r300vk_synthesize_mat4vec_fs(struct pipe_context *pipe)
{
   /* The 4x4 is uniform across every element, so it lives in the constant file
    * (CONST[0..3] = the four rows) rather than a texture: the dispatch uploads
    * the 64 bytes per draw and each output lane is one DP4 of a const row with
    * the per-element vertex.  That compiles to 1 TEX + 4 DP4.  The texture-matrix
    * variant needed 4 extra TEX (one per row) plus 4 coordinate-staging MOVs to
    * sample them at the fixed texel centres -- eight instructions and four
    * texture-cache fetches a const-file read does not cost.  The vertex is the
    * only sampler (stage 0), fetched at the interpolated fullscreen coord. */
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src row[4];
   for (unsigned i = 0; i < 4; i++)
      row[i] = ureg_DECL_constant(ureg, i);

   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst vtx = ureg_DECL_temporary(ureg);

   ureg_TEX(ureg, ureg_writemask(vtx, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tc, samp);
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_X), row[0], ureg_src(vtx));
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_Y), row[1], ureg_src(vtx));
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_Z), row[2], ureg_src(vtx));
   ureg_DP4(ureg, ureg_writemask(out, TGSI_WRITEMASK_W), row[3], ureg_src(vtx));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* MAT4VEC VS+FS synthesis: the passthrough VS shared with DP4 plus the transform
 * FS.  The dispatch wraps the matrix as a 4x1 view + vertices per-element. */
static bool
r300vk_mat4vec_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_mat4vec_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QFMUL FS: out = a * s, the per-element quaternion a sampled at stage 0 times
 * the BROADCAST scalar s read from the fragment constant file (CONST[0].x), the
 * way MAT4VEC reads its broadcast matrix from the const file.  One TEX + one MUL;
 * the scalar costs no per-element fetch. */
static void *
r300vk_synthesize_qfmul_fs(struct pipe_context *pipe)
{
   struct ureg_program *ureg = ureg_create(MESA_SHADER_FRAGMENT);
   if (!ureg)
      return NULL;

   struct ureg_src scal = ureg_DECL_constant(ureg, 0);   /* s in CONST[0].x */
   struct ureg_src samp = ureg_DECL_sampler(ureg, 0);
   ureg_DECL_sampler_view(ureg, 0, TGSI_TEXTURE_2D,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT,
                          TGSI_RETURN_TYPE_FLOAT, TGSI_RETURN_TYPE_FLOAT);
   struct ureg_src tc = ureg_DECL_fs_input(ureg, TGSI_SEMANTIC_GENERIC, 0,
                                           TGSI_INTERPOLATE_PERSPECTIVE);
   struct ureg_dst out  = ureg_DECL_output(ureg, TGSI_SEMANTIC_COLOR, 0);
   struct ureg_dst quat = ureg_DECL_temporary(ureg);

   ureg_TEX(ureg, ureg_writemask(quat, TGSI_WRITEMASK_XYZW),
            TGSI_TEXTURE_2D, tc, samp);
   ureg_MUL(ureg, ureg_writemask(out, TGSI_WRITEMASK_XYZW),
            ureg_src(quat), ureg_scalar(scal, TGSI_SWIZZLE_X));
   ureg_END(ureg);
   return ureg_create_shader_and_destroy(ureg, pipe);
}

/* QFMUL VS+FS synthesis: shared passthrough VS + the scalar-product FS.  The
 * dispatch uploads the broadcast scalar to CONST[0] and wraps the quaternions
 * per-element. */
static bool
r300vk_qfmul_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_qfmul_fs(pipe);
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

/* QNORMALIZE FS: a * rsqrt(dot(a,a)), the unit quaternion built by
 * r300vk_build_qnormalize_fs_nir -- one DP4, the US RSQ, one vec4 scale. */
static void *
r300vk_synthesize_qnormalize_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_qnormalize_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QNORMALIZE VS+FS synthesis: the passthrough VS plus the normalize FS. */
static bool
r300vk_qnormalize_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_qnormalize_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* OMUL VS+FS synthesis: the passthrough VS plus BOTH octonion-product half FSs.
 * fs_cso holds the lower-half FS (a*c - conj(d)*b), fs_cso2 the upper-half FS
 * (d*a + b*conj(c)); the two-pass dispatch (route A) runs one then the other.
 * When the screen advertises two-plus render targets AND an FP16 render target,
 * also synthesize the single-pass MRT FS into fs_cso_mrt -- its presence is the
 * gate the dispatch uses to prefer route B (both halves in one draw).  A failed
 * MRT-FS create is non-fatal: route A still works, so fs_cso_mrt stays NULL. */
static bool
r300vk_omul_synthesize_shaders(struct r300vk_device *device,
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

   nir_shader *lo = r300vk_build_omul_lo_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, lo, true);
   struct pipe_shader_state lo_state = { .type = PIPE_SHADER_IR_NIR, .ir.nir = lo };
   pl->fs_cso = pipe->create_fs_state(pipe, &lo_state);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }

   nir_shader *hi = r300vk_build_omul_hi_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, hi, true);
   struct pipe_shader_state hi_state = { .type = PIPE_SHADER_IR_NIR, .ir.nir = hi };
   pl->fs_cso2 = pipe->create_fs_state(pipe, &hi_state);
   if (!pl->fs_cso2) {
      pipe->delete_fs_state(pipe, pl->fs_cso);
      pl->fs_cso = NULL;
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }

   /* Route B (single-pass MRT) fast path, only when the hardware can bind two
    * simultaneous FP16 render targets.  R300 advertises four, so this is taken
    * on RS480; on a screen that advertised one it stays NULL and route A runs. */
   if (pipe->screen->caps.max_render_targets >= 2 &&
       pipe->screen->is_format_supported(pipe->screen,
          PIPE_FORMAT_R16G16B16A16_FLOAT, PIPE_TEXTURE_2D, 0, 0,
          PIPE_BIND_RENDER_TARGET)) {
      nir_shader *mrt = r300vk_build_omul_mrt_fs_nir(
         pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
      if (pipe->screen->finalize_nir)
         pipe->screen->finalize_nir(pipe->screen, mrt, true);
      struct pipe_shader_state mrt_state = { .type = PIPE_SHADER_IR_NIR,
                                             .ir.nir = mrt };
      pl->fs_cso_mrt = pipe->create_fs_state(pipe, &mrt_state);
   }
   return true;
}

/* Finalize a synthesized fragment NIR for the screen and create its CSO. */
static void *
r300vk_make_fs_cso(struct pipe_context *pipe, nir_shader *s)
{
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR, .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* True when the screen can bind two simultaneous FP16 render targets -- the gate
 * for the single-pass MRT octonion ops. */
static bool
r300vk_screen_supports_mrt_fp16(struct pipe_screen *screen)
{
   return screen->caps.max_render_targets >= 2 &&
          screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                      PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET);
}

/* ONORM: passthrough VS + the self-dot-sum FS in fs_cso (the 2-in/1-out core). */
static bool
r300vk_onorm_synthesize_shaders(struct r300vk_device *device,
                                struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r300vk_make_fs_cso(pipe, r300vk_build_onorm_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]));
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* OCONJ: passthrough VS + the MRT conjugate FS in fs_cso_mrt.  Needs two FP16
 * render targets; without them fs_cso_mrt stays NULL and the dispatch reports
 * the kernel inadmissible at replay (RS480 always has the targets). */
static bool
r300vk_oconj_synthesize_shaders(struct r300vk_device *device,
                                struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   if (r300vk_screen_supports_mrt_fp16(pipe->screen))
      pl->fs_cso_mrt = r300vk_make_fs_cso(pipe, r300vk_build_oconj_mrt_fs_nir(
         pipe->screen->nir_options[MESA_SHADER_FRAGMENT]));
   return true;
}

/* OADD/OSUB: passthrough VS + the MRT add/sub FS in fs_cso_mrt (is_sub from the
 * detected pattern).  Same FP16-MRT gate as OCONJ. */
static bool
r300vk_oaddsub_synthesize_shaders(struct r300vk_device *device,
                                  struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   if (r300vk_screen_supports_mrt_fp16(pipe->screen))
      pl->fs_cso_mrt = r300vk_make_fs_cso(pipe, r300vk_build_oaddsub_mrt_fs_nir(
         pipe->screen->nir_options[MESA_SHADER_FRAGMENT], pl->oaddsub.is_sub));
   return true;
}

/* ODIV: passthrough VS + the two division half-FSs (fs_cso = lower half, fs_cso2 =
 * upper half), each forming inv(y) from the reciprocal of |y|^2 and emitting one
 * half of the product.  Division is two single-output passes rather than one MRT
 * pass: the combined form is 73 ALU ops, over the 64-ALU R300 fragment limit.  The
 * detected handedness picks the half-shaders -- left division inv(y)*x swaps the
 * OMUL operands versus right division x*inv(y); the dispatch stays the same. */
static bool
r300vk_odiv_synthesize_shaders(struct r300vk_device *device,
                               struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   const struct nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   if (pl->odiv.is_left) {
      pl->fs_cso  = r300vk_make_fs_cso(pipe, r300vk_build_odiv_l_lo_fs_nir(opts));
      pl->fs_cso2 = r300vk_make_fs_cso(pipe, r300vk_build_odiv_l_hi_fs_nir(opts));
   } else {
      pl->fs_cso  = r300vk_make_fs_cso(pipe, r300vk_build_odiv_lo_fs_nir(opts));
      pl->fs_cso2 = r300vk_make_fs_cso(pipe, r300vk_build_odiv_hi_fs_nir(opts));
   }
   return pl->fs_cso != NULL && pl->fs_cso2 != NULL;
}

/* OTRANS: passthrough VS + four half-FSs for the two octonion products of the
 * sandwich x*v*conj(x).  Pass 1 (t = x*v) reuses the OMUL half-shaders in
 * fs_cso/fs_cso2; pass 2 (out = t*conj(x)) uses the dedicated half-shaders in
 * fs_cso3/fs_cso4.  The sandwich is 32 DP4s through a scratch intermediate t --
 * far past the single-pass fragment limit, so it runs as four single-output
 * passes (each one OMUL half, well under the 64-ALU R300 budget). */
static bool
r300vk_otrans_synthesize_shaders(struct r300vk_device *device,
                                 struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   const struct nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   pl->fs_cso  = r300vk_make_fs_cso(pipe, r300vk_build_omul_lo_fs_nir(opts));
   pl->fs_cso2 = r300vk_make_fs_cso(pipe, r300vk_build_omul_hi_fs_nir(opts));
   pl->fs_cso3 = r300vk_make_fs_cso(pipe, r300vk_build_otrans_p2_lo_fs_nir(opts));
   pl->fs_cso4 = r300vk_make_fs_cso(pipe, r300vk_build_otrans_p2_hi_fs_nir(opts));
   return pl->fs_cso != NULL && pl->fs_cso2 != NULL &&
          pl->fs_cso3 != NULL && pl->fs_cso4 != NULL;
}

/* QFMADD / QFMMUL synthesis: passthrough VS + the single fused FS into fs_cso. */
static bool
r300vk_qfmadd_synthesize_shaders(struct r300vk_device *device,
                                 struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r300vk_make_fs_cso(pipe, r300vk_build_qfmadd_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]));
   return pl->fs_cso != NULL;
}

static bool
r300vk_qfmmul_synthesize_shaders(struct r300vk_device *device,
                                 struct r300vk_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r300vk_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r300vk_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r300vk_make_fs_cso(pipe, r300vk_build_qfmmul_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]));
   return pl->fs_cso != NULL;
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

static void *
r300vk_synthesize_ieee16_classify_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_ieee16_classify_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

static bool
r300vk_ieee16_classify_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_ieee16_classify_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

static void *
r300vk_synthesize_ieee16_mul_fs(struct pipe_context *pipe)
{
   nir_shader *s = r300vk_build_ieee16_mul_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

static bool
r300vk_ieee16_mul_synthesize_shaders(struct r300vk_device *device,
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

   pl->fs_cso = r300vk_synthesize_ieee16_mul_fs(pipe);
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
   /* CONSTFILL lowers to a framebuffer clear: no vs/fs CSO to synthesize, the
    * dispatch is self-contained.  Report success so the pipeline stays valid. */
   if (pl->const_fill.is_const_fill)
      return true;
   if (pl->identity_map.is_identity_map) {
      if (!r300vk_identity_map_synthesize_shaders(device, pl))
         pl->identity_map.is_identity_map = false;
      return true;
   }
   if (pl->multilimb_mul.is_multilimb_mul) {
      if (!r300vk_multilimb_synthesize_shaders(device, pl))
         pl->multilimb_mul.is_multilimb_mul = false;
      return true;
   }
   if (pl->cas.is_cas) {
      if (!r300vk_cas_synthesize_shaders(device, pl))
         pl->cas.is_cas = false;
      return true;
   }
   if (pl->log4_pool.is_log4_pool) {
      if (!r300vk_log4_synthesize_shaders(device, pl))
         pl->log4_pool.is_log4_pool = false;
      return true;
   }
   if (pl->binary_map.is_binary_map) {
      if (!r300vk_binary_map_synthesize_shaders(device, pl))
         pl->binary_map.is_binary_map = false;
      return true;
   }
   if (pl->unary_map.is_unary_map) {
      if (!r300vk_unary_map_synthesize_shaders(device, pl))
         pl->unary_map.is_unary_map = false;
      return true;
   }
   if (pl->affine_iota.is_affine_iota) {
      if (!r300vk_affine_iota_synthesize_shaders(device, pl))
         pl->affine_iota.is_affine_iota = false;
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
   if (pl->qdiv.is_qdiv) {
      if (!r300vk_qdiv_synthesize_shaders(device, pl))
         pl->qdiv.is_qdiv = false;
      return true;
   }
   if (pl->mat4vec.is_mat4vec) {
      if (!r300vk_mat4vec_synthesize_shaders(device, pl))
         pl->mat4vec.is_mat4vec = false;
      return true;
   }
   if (pl->qfmul.is_qfmul) {
      if (!r300vk_qfmul_synthesize_shaders(device, pl))
         pl->qfmul.is_qfmul = false;
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
   if (pl->qnormalize.is_qnormalize) {
      if (!r300vk_qnormalize_synthesize_shaders(device, pl))
         pl->qnormalize.is_qnormalize = false;
      return true;
   }
   if (pl->omul.is_omul) {
      if (!r300vk_omul_synthesize_shaders(device, pl))
         pl->omul.is_omul = false;
      return true;
   }
   if (pl->oaddsub.is_oaddsub) {
      if (!r300vk_oaddsub_synthesize_shaders(device, pl))
         pl->oaddsub.is_oaddsub = false;
      return true;
   }
   if (pl->oconj.is_oconj) {
      if (!r300vk_oconj_synthesize_shaders(device, pl))
         pl->oconj.is_oconj = false;
      return true;
   }
   if (pl->onorm.is_onorm) {
      if (!r300vk_onorm_synthesize_shaders(device, pl))
         pl->onorm.is_onorm = false;
      return true;
   }
   if (pl->odiv.is_odiv) {
      if (!r300vk_odiv_synthesize_shaders(device, pl))
         pl->odiv.is_odiv = false;
      return true;
   }
   if (pl->otrans.is_otrans) {
      if (!r300vk_otrans_synthesize_shaders(device, pl))
         pl->otrans.is_otrans = false;
      return true;
   }
   if (pl->qfmadd.is_qfmadd) {
      if (!r300vk_qfmadd_synthesize_shaders(device, pl))
         pl->qfmadd.is_qfmadd = false;
      return true;
   }
   if (pl->qfmmul.is_qfmmul) {
      if (!r300vk_qfmmul_synthesize_shaders(device, pl))
         pl->qfmmul.is_qfmmul = false;
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
   if (pl->ieee16_classify.is_ieee16_classify) {
      if (!r300vk_ieee16_classify_synthesize_shaders(device, pl))
         pl->ieee16_classify.is_ieee16_classify = false;
      return true;
   }
   if (pl->ieee16_mul.is_ieee16_mul) {
      if (!r300vk_ieee16_mul_synthesize_shaders(device, pl))
         pl->ieee16_mul.is_ieee16_mul = false;
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
   struct r300_compute_unary_map_pattern unary_pat = {0};
   struct r300_compute_blend_acc_reduction_pattern blendacc = {0};
   struct r300_compute_zpass_reduction_pattern zpass = {0};
   struct r300_compute_multipass_scan_pattern multiscan = {0};
   struct r300_compute_predicated_store_pattern predstore = {0};
   struct r300_compute_multitap_gather_pattern gather = {0};
   struct r300_compute_dp4_pattern dp4_pat = {0};
   struct r300_compute_qmul_pattern qmul_pat = {0};
   struct r300_compute_qdiv_pattern qdiv_pat = {0};
   struct r300_compute_mat4vec_pattern mat4vec_pat = {0};
   struct r300_compute_qfmul_pattern qfmul_pat = {0};
   struct r300_compute_qrotate_pattern qrotate_pat = {0};
   struct r300_compute_qconj_pattern qconj_pat = {0};
   struct r300_compute_qnorm_pattern qnorm_pat = {0};
   struct r300_compute_qnormalize_pattern qnormalize_pat = {0};
   struct r300_compute_omul_pattern omul_pat = {0};
   struct r300_compute_oaddsub_pattern oaddsub_pat = {0};
   struct r300_compute_oconj_pattern oconj_pat = {0};
   struct r300_compute_onorm_pattern onorm_pat = {0};
   struct r300_compute_odiv_pattern odiv_pat = {0};
   struct r300_compute_otrans_pattern otrans_pat = {0};
   struct r300_compute_qfmadd_pattern qfmadd_pat = {0};
   struct r300_compute_qfmmul_pattern qfmmul_pat = {0};
   struct r300_compute_ieee16_classify_pattern ieee16_classify_pat = {0};
   struct r300_compute_ieee16_mul_pattern ieee16_mul_pat = {0};
   struct r300_compute_const_fill_pattern constfill_pat = {0};
   struct r300_compute_index_pattern index_pat = {0};
   struct r300_compute_affine_iota_pattern affine_iota_pat = {0};
   struct r300_compute_multilimb_mul_pattern multilimb_pat = {0};
   struct r300_compute_cas_pattern cas_pat = {0};
   struct r300_compute_log4_pool_pattern log4_pat = {0};
   uint32_t local_size[3];

   if (!r300vk_classify_compute_kernel(device, &pCreateInfo->stage,
                                       &adm, &ident, &binmap, &unary_pat,
                                       &blendacc, &zpass,
                                       &multiscan, &predstore, &gather, &dp4_pat,
                                       &qmul_pat, &qdiv_pat, &mat4vec_pat, &qfmul_pat,
                                       &qrotate_pat,
                                       &qconj_pat, &qnorm_pat, &qnormalize_pat,
                                       &omul_pat,
                                       &oaddsub_pat, &oconj_pat, &onorm_pat,
                                       &odiv_pat, &otrans_pat,
                                       &qfmadd_pat, &qfmmul_pat,
                                       &ieee16_classify_pat, &ieee16_mul_pat,
                                       &constfill_pat, &index_pat,
                                       &affine_iota_pat, &multilimb_pat,
                                       &cas_pat, &log4_pat,
                                       local_size))
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: SPIR-V to NIR failed for compute kernel %u",
                       i);

   /* Kernels the RS482 substrate classifier cannot map to a raster pattern are
    * still valid VkPipeline objects.  vkCreateComputePipelines does not permit
    * VK_ERROR_FEATURE_NOT_PRESENT; inadmissible pipelines dispatched at replay
    * time are silent no-ops (R300_COMPUTE_REJECT_UNKNOWN_SHAPE) that return
    * VK_SUCCESS without writing the output buffer, keeping the queue alive.
    * The reject reason still surfaces here through the debug messenger so an
    * app (or probe harness) can see WHY the kernel will no-op without
    * grepping driver logs. */
   if (!adm.admissible) {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r300vk: compute kernel %u is not lowerable to the RS482 raster "
              "substrate: %s (%s); dispatches will no-op",
              i, r300_compute_reject_name(adm.reason),
              adm.detail ? adm.detail : "no detail");
   }
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
   pl->unary_map = unary_pat;
   pl->dp4 = dp4_pat;
   pl->qmul = qmul_pat;
   pl->qdiv = qdiv_pat;
   pl->mat4vec = mat4vec_pat;
   pl->qfmul = qfmul_pat;
   pl->qrotate = qrotate_pat;
   pl->qconj = qconj_pat;
   pl->qnorm = qnorm_pat;
   pl->qnormalize = qnormalize_pat;
   pl->omul = omul_pat;
   pl->oaddsub = oaddsub_pat;
   pl->oconj = oconj_pat;
   pl->onorm = onorm_pat;
   pl->odiv = odiv_pat;
   pl->otrans = otrans_pat;
   pl->qfmadd = qfmadd_pat;
   pl->qfmmul = qfmmul_pat;
   pl->ieee16_classify = ieee16_classify_pat;
   pl->ieee16_mul = ieee16_mul_pat;
   pl->const_fill = constfill_pat;
   pl->index_consumption = index_pat;
   pl->affine_iota = affine_iota_pat;
   pl->multilimb_mul = multilimb_pat;
   pl->cas = cas_pat;
   pl->log4_pool = log4_pat;
   /* The multilimb path is exact for every u32 operand pair; the binary-map
    * imul lowering is only exact below the FP24 window.  When both detect
    * the same kernel, multilimb wins and the elementwise route is cleared. */
   if (pl->multilimb_mul.is_multilimb_mul)
      pl->binary_map.is_binary_map = false;
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
   if (pl->fs_cso2)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso2);
   if (pl->fs_cso_mrt)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso_mrt);
   if (pl->fs_cso3)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso3);
   if (pl->fs_cso4)
      device->pipe->delete_fs_state(device->pipe, pl->fs_cso4);
   for (unsigned k = 0; k < 9; k++)
      if (pl->multilimb_fs[k] && pl->multilimb_fs[k] != pl->fs_cso)
         device->pipe->delete_fs_state(device->pipe, pl->multilimb_fs[k]);
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
