/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_pipeline.h"
#include "r3v_cmd_buffer.h"
#include "r3v_descriptor.h"
#include "r3v_format.h"
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_from_mesa.h"
#include "compiler/nir/nir_opcodes.h"
#include "r3v_device.h"
#include "r3v_dp4_fs_nir.h"
#include "r3v_shader_module.h"

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
#include "util/log.h"
#include "util/macros.h"

#include <string.h>

static const VkVertexInputBindingDescription *
r3v_find_vertex_binding_desc(const VkPipelineVertexInputStateCreateInfo *vi,
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
r3v_vertex_attr_data_size(enum pipe_format format)
{
   /* The robust vertex count counts vertices whose ATTRIBUTE DATA lies inside
    * the bound buffer range -- that span, not a dword-rounded fetch width, is
    * what robustBufferAccess defines as in-bounds.  Use the format block size
    * (the bytes the shader actually consumes).
    *
    * r300 fetches vertex data in dword units, so the final vertex of a tightly
    * packed 8/16/24-bit attribute makes the hardware read up to 3 bytes past the
    * attribute end.  Those bytes fall inside the vertex BO -- the radeon winsys
    * rounds every allocation up to a page -- and the attribute's component count
    * discards them, so the over-read is harmless.  r300_emit_vertex_arrays emits
    * its own dword-aligned R300_VBPNTR_SIZE from the gallium velem format_size,
    * independent of this value, and the kernel CS validator bounds that against
    * the page-aligned BO, not the tight binding range.  Counting by the dword
    * span instead dropped the final in-bounds vertex and violated the
    * robustBufferAccess this driver advertises (manifested as a one-pixel
    * image-compare miss on the sparsest draw of
    * dEQP-VK.memory.pipeline_barrier.transfer_dst_vertex_buffer.*_stride_2). */
   return util_format_get_blocksize(format);
}

static VkResult
r3v_validate_vertex_input(struct r3v_device *device,
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
                       "r3v: vertex attribute count %u exceeds %u",
                       vi->vertexAttributeDescriptionCount,
                       PIPE_MAX_ATTRIBS);

   for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &vi->pVertexBindingDescriptions[i];
      if (desc->binding >= R3V_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex binding %u exceeds %u",
                          desc->binding, R3V_MAX_VERTEX_BINDINGS - 1);
   }

   for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &vi->pVertexAttributeDescriptions[i];
      if (attr->location >= R3V_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex attribute location %u exceeds %u",
                          attr->location, R3V_MAX_VERTEX_BINDINGS - 1);
      if (location_mask & BITFIELD_BIT(attr->location))
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: duplicate vertex attribute location %u",
                          attr->location);
      if (attr->binding >= R3V_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex attribute binding %u exceeds %u",
                          attr->binding, R3V_MAX_VERTEX_BINDINGS - 1);
      if (!r3v_find_vertex_binding_desc(vi, attr->binding))
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex attribute binding %u has no "
                          "matching binding description", attr->binding);
      location_mask |= BITFIELD_BIT(attr->location);
      input_slot = MAX2(input_slot, attr->location + 1);
      *used_binding_mask |= BITFIELD_BIT(attr->binding);
   }

   if (input_slot > 0 && location_mask != BITFIELD_MASK(input_slot))
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: sparse vertex attribute locations are not "
                       "representable by r300g vertex elements");

   *next_input_slot = input_slot;
   return VK_SUCCESS;
}

static VkResult
r3v_reserve_vs_system_value_streams(
   struct r3v_device *device,
   struct r3v_pipeline *pl,
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
      r3v_validate_vertex_input(device, vi, &used_binding_mask,
                                   &next_input_slot);
   if (val_res != VK_SUCCESS)
      return val_res;

   if (next_input_slot > PIPE_MAX_ATTRIBS - synth_count ||
       next_input_slot > R3V_MAX_VERTEX_BINDINGS - synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: no vertex input slot available for "
                       "the synthetic VS system-value stream");

   uint8_t synth_bindings[2];
   uint32_t reserved_count = 0;
   for (uint32_t b = 0; b < R3V_MAX_VERTEX_BINDINGS; b++) {
      if (used_binding_mask & BITFIELD_BIT(b))
         continue;
      synth_bindings[reserved_count++] = (uint8_t)b;
      if (reserved_count == synth_count)
         break;
   }

   if (reserved_count < synth_count)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: no vertex buffer binding available for "
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

static const struct spirv_to_nir_options r3v_spirv_opts = {
   .environment            = NIR_SPIRV_VULKAN,
   .ubo_addr_format        = nir_address_format_32bit_index_offset,
   .ssbo_addr_format       = nir_address_format_32bit_index_offset,
   .push_const_addr_format = nir_address_format_32bit_offset,
   .shared_addr_format     = nir_address_format_32bit_offset,
};

/* r300 has one constant file (RC_FILE_CONSTANT), and ntr_emit_load_ubo asserts
 * the UBO block index is 0.  r3v_nir_lower_vulkan_resource_index_single has
 * already rejected any shader that needs more than the single uniform buffer
 * r3v_bind_descriptor_ubo binds at CONST[0], and lowered the surviving
 * descriptor chain to a constant block-0 address.  nir_lower_explicit_io then
 * rebuilds that block index as a vec construct + component extract (block =
 * vec2(addr.x, ...).x), i.e. a mov of a vec, not a load_const, and no pass folds
 * it back to a constant before nir_to_rc consumes it.  Force every
 * load_ubo[_vec4] block index to a literal 0 so the index-0 assert holds; the
 * single bound buffer lives at CONST[0]. */
static void
r3v_nir_remap_single_ubo_to_index0(nir_shader *nir)
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

/* r300 exposes a single constant file per stage, and r3v_bind_descriptor_ubo
 * binds the one bound uniform buffer at CONST[0].  vk_spirv_to_nir emits each
 * UBO access as a vulkan_resource_index -> load_vulkan_descriptor -> deref ->
 * load chain, carrying the resource address in 32bit_index_offset form, where
 * component .x is the constant-file block index and .y the base offset.
 * nir_lower_explicit_io derives load_ubo's block index from that chain, which is
 * a non-constant SSA value, so r3v_nir_remap_single_ubo_to_index0 cannot fold
 * it to index 0 and the pipeline is rejected.  Lower the chain here, before
 * explicit I/O, so the single supported UBO resolves to a literal block 0:
 *   vulkan_resource_index(set, binding) -> imm ivec2(0, 0)
 *   load_vulkan_descriptor(addr)        -> addr   (identity for index_offset)
 * The byte offset inside the buffer is applied at bind time through
 * pipe_constant_buffer::buffer_offset, so the in-shader base offset is 0.  The
 * post-explicit-I/O remap then sees only block 0 and is a no-op safety net.
 *
 * Reject every descriptor shape r300's single read-only constant file cannot
 * represent, so pipeline creation fails instead of aliasing distinct buffers
 * onto CONST[0] and rendering garbage:
 *   - a storage-buffer descriptor (the constant file is read-only),
 *   - a vulkan_resource_reindex (a dynamically indexed descriptor array),
 *   - a non-constant or non-zero resource array index,
 *   - more than one distinct (set, binding) uniform buffer.
 */
static bool
r3v_nir_lower_vulkan_resource_index_single(nir_shader *nir,
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
 * (i+0.5, j+0.5), so the product lands on that texel center.  Replay translates
 * viewport state into tile-local framebuffer coordinates, so W/H are the bound
 * pipe_resource dimensions: the full image for a single-tile attachment or the
 * matching split tile for a tiled attachment.  The r300 fragment ALU is FP24
 * (16-bit mantissa): the product error is at most W*2^-17 (about 0.03 texel at
 * W=4096), well inside the half-texel NEAREST margin, so the correct texel is
 * always resolved.  inv_extent is read from the fragment CONST[0] (load_ubo
 * block 0, offset 0); the replay binds (1/W,1/H) there per draw, the same slot
 * the keystone UBO uses -- so an input-attachment shader that also reads an app
 * UBO or push constants is rejected at compile (one CONST[0]).
 * nir_lower_samplers (in nir_to_rc) assigns the Gallium unit from the synthetic
 * sampler variable's data.binding.  The descriptor identity stays separate:
 * replay matches the original (set, binding), while the texture fetch uses
 * R3V_INPUT_ATTACHMENT_SAMPLER_UNIT because r300g sampler updates must start
 * at unit zero.  A multisample subpass input (GLSL_SAMPLER_DIM_SUBPASS_MS) does
 * not match the GLSL_SAMPLER_DIM_SUBPASS filter below and is left unlowered, but
 * it sets no reject flag -- it is unreachable because r300 is single-sample and
 * r3v_CreateImage rejects samples != 1, so no multisample image (hence no
 * GLSL_SAMPLER_DIM_SUBPASS_MS input) can ever exist to drive this pass.
 *
 * The replay binds one input attachment per pipeline (the single descriptor
 * identity), so reading two distinct input descriptors cannot be honored:
 * out_multiple_bindings reports that for the caller to reject.  r300's FP24
 * fragment ALU has no integer texture path either, so an integer
 * (isubpassInput/usubpassInput) result type sets out_has_integer and the load
 * is left unlowered for the same reject path. */
static bool
r3v_nir_lower_subpass_input(nir_shader *nir, bool *out_has_input,
                               uint32_t *out_set,
                               uint32_t *out_binding,
                               bool *out_multiple_bindings,
                               bool *out_has_integer)
{
   bool progress = false;
   bool have_first_descriptor = false;
   uint32_t first_set = 0;
   uint32_t first_binding = 0;
   /* One synthesized sampler variable backs every subpassLoad of the single
    * permitted input attachment.  Created lazily so the texture deref's type
    * matches its own variable (nir_validate requires deref->type == var->type);
    * the original subpass-input image variable and its derefs stay
    * self-consistent and become dead, removed by later DCE. */
   nir_variable *sampler_var = NULL;
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
            const uint32_t descriptor_set = var ? var->data.descriptor_set : 0;
            const uint32_t binding = var ? var->data.binding : 0;
            const enum glsl_base_type rbt =
               glsl_get_sampler_result_type(deref->type);

            *out_has_input = true;
            if (have_first_descriptor &&
                (descriptor_set != first_set || binding != first_binding))
               *out_multiple_bindings = true;
            if (!have_first_descriptor) {
               have_first_descriptor = true;
               first_set = descriptor_set;
               first_binding = binding;
            }
            *out_set = descriptor_set;
            *out_binding = binding;

            /* An integer subpass input would lower to an integer texel fetch the
             * FP24 fragment ALU cannot emit.  Leave it unlowered so the pipeline
             * reject path sees out_has_integer rather than emitting a tex op with
             * an integer destination type. */
            if (rbt == GLSL_TYPE_INT || rbt == GLSL_TYPE_UINT) {
               *out_has_integer = true;
               continue;
            }

            b.cursor = nir_before_instr(instr);
            nir_def *fragcoord_xy = nir_build_frag_coord(&b, 2);
            nir_def *inv_extent = nir_load_ubo(&b, 2, 32, nir_imm_int(&b, 0),
                                               nir_imm_int(&b, 0),
                                               .align_mul = 8, .range_base = 0,
                                               .range = 8);
            nir_def *coord = nir_fmul(&b, fragcoord_xy, inv_extent);

            /* Build (once) a 2D sampler variable and sample it: r300 has no
             * input-attachment hardware, so subpassLoad is a NEAREST texture
             * fetch at the fragment center. */
            if (!sampler_var) {
               sampler_var = nir_variable_create(
                  nir, nir_var_uniform,
                  glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, rbt),
                  "r3v_subpass_input");
               sampler_var->data.binding = R3V_INPUT_ATTACHMENT_SAMPLER_UNIT;
               sampler_var->data.descriptor_set = descriptor_set;
            }
            nir_deref_instr *tex_deref = nir_build_deref_var(&b, sampler_var);

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
 * nir_var_mem_push_const variable; r3v's nir_lower_explicit_io call lowers
 * only UBO/SSBO, so that deref would reach nir_to_rc, which has no push-constant
 * handler and would treat it as an unknown load_deref.  Check the variable mode
 * and the lowered load_push_constant intrinsic so the test holds wherever it
 * runs in the pipeline. */
static bool
r3v_nir_uses_push_constants(nir_shader *nir)
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
 * r3v_nir_lower_vulkan_resource_index_single rewrites the chain away, to
 * detect a push-constant + UBO collision: both resolve to CONST[0] and r300's
 * single constant file cannot hold both. */
static bool
r3v_nir_uses_ubo(nir_shader *nir)
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
 * the Gallium draw module, and r3v binds no sampler views/states to that
 * draw module, so a tex executed by the SW-TCL vertex shader dereferences a NULL
 * sampler in tgsi_exec fetch_texel and segfaults at draw.  r3v_compile_shader
 * uses this to reject a vertex shader that samples rather than crash. */
static bool
r3v_nir_uses_texture(nir_shader *nir)
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

/* True if the shader declares a buffer texture or buffer image: a uniform or storage
 * texel buffer reaches NIR as a GLSL_SAMPLER_DIM_BUF sampler, texture, or image
 * variable.  r300 has no buffer-resource unit and no storage path, so the fragment
 * translator lowers such a shader to a dummy and the SW-TCL vertex shader reaches
 * nir_to_tgsi's unassigned-source assert at draw.  r3v_compile_shader rejects the
 * pipeline at compile so neither shader runs. */
static bool
r3v_nir_uses_buffer_resource(nir_shader *nir)
{
   nir_foreach_variable_in_shader(var, nir) {
      const struct glsl_type *type = glsl_without_array(var->type);
      if ((glsl_type_is_image(type) || glsl_type_is_texture(type) ||
           glsl_type_is_sampler(type)) &&
          glsl_get_sampler_dim(type) == GLSL_SAMPLER_DIM_BUF)
         return true;
   }
   return false;
}

/* Match r300_create_vs_state's NIR optimization point before deciding whether
 * a vertex texture instruction can reach the SW-TCL draw module. */
static bool
r3v_nir_uses_live_texture_after_r300_opt(struct pipe_screen *pscreen,
                                            nir_shader *nir)
{
   if (!r3v_nir_uses_texture(nir))
      return false;

   nir_shader *check = nir_shader_clone(NULL, nir);
   r300_optimize_nir(check, r300_screen(pscreen));
   bool uses_texture = r3v_nir_uses_texture(check);
   ralloc_free(check);
   return uses_texture;
}

/* Rewrite each fragment sampler variable's binding to the flat Gallium unit the
 * pipeline assigned its (descriptor set, binding) in fs_sampler_map.
 * nir_lower_samplers (run later in nir_to_rc) keys the texture unit on
 * data.binding, so after this rewrite a sampler in any descriptor set lands on
 * the same unit the replay binds it to. The descriptor set remains intact so a
 * sampler from a nonzero set cannot collapse into the set-0 namespace and
 * alias a UBO after its binding is rewritten. */
static void
r3v_nir_remap_sampler_units(nir_shader *nir, const struct r3v_pipeline *pl)
{
   nir_foreach_variable_with_modes(var, nir, nir_var_uniform) {
      if (!glsl_type_is_sampler(glsl_without_array(var->type)))
         continue;
      for (uint16_t i = 0; i < pl->fs_sampler_map_count; i++) {
         if (pl->fs_sampler_map[i].set == var->data.descriptor_set &&
             pl->fs_sampler_map[i].binding == var->data.binding) {
            /* Rewrite only the binding to the flat unit nir_lower_samplers keys on.
             * The descriptor set is left intact: zeroing it would alias the sampler
             * onto a set-0 UBO variable at the same binding and break that UBO's
             * lowering (the lowered sampler unit is what the replay matches, not the
             * set). */
            var->data.binding = pl->fs_sampler_map[i].unit;
            break;
         }
      }
   }
}

/* One per-tile NEAREST fetch for the stitch expansion: sample the tile sampler
 * at unit base_unit+tile_index at a local coordinate, cloning the source tex's
 * sampler dimension and result type.  The tile sampler variables are created
 * once per function and cached in tile_var. */
static nir_def *
r3v_stitch_tile_sample(nir_builder *b, nir_shader *nir,
                          nir_variable **tile_var, uint32_t base_unit,
                          unsigned tile_index, const nir_tex_instr *src,
                          nir_def *coord)
{
   static const char *const tile_names[R3V_NEAREST_STITCH_TILE_UNITS] = {
      "r3v_stitch_t00", "r3v_stitch_t10",
      "r3v_stitch_t01", "r3v_stitch_t11",
   };
   if (!tile_var[tile_index]) {
      nir_variable *v = nir_variable_create(
         nir, nir_var_uniform,
         glsl_sampler_type(src->sampler_dim, false, false, GLSL_TYPE_FLOAT),
         tile_names[tile_index]);
      v->data.binding = base_unit + tile_index;
      v->data.descriptor_set = 0;
      tile_var[tile_index] = v;
   }
   nir_deref_instr *deref = nir_build_deref_var(b, tile_var[tile_index]);
   nir_tex_instr *tex = nir_tex_instr_create(nir, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = src->sampler_dim;
   tex->coord_components = 2;
   tex->is_array = false;
   tex->is_shadow = false;
   tex->dest_type = src->dest_type;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   nir_def_init(&tex->instr, &tex->def, nir_tex_instr_dest_size(tex),
                src->def.bit_size);
   nir_builder_instr_insert(b, &tex->instr);
   return &tex->def;
}

/* Expand each fragment combined-image-sampler texture() into r300's tiled
 * sampler form under the experimental NEAREST-stitch gate: four per-tile NEAREST
 * fetches at piecewise-affine local coordinates plus a branchless tile select.
 * Two block-0 CONST vec4s carry the per-image geometry the replay uploads --
 * cu = {scale_u0, scale_u1, bias_u1, sx}, cv = {scale_v0, scale_v1, bias_v1, sy}
 * -- so a column-0 sample is u*scale_u0 and a column-1 sample is u*scale_u1+bias_u1,
 * and the select picks the tile whose threshold the coordinate crosses.  For a
 * single-tile image the thresholds are 2.0, so right/bottom are always false and
 * the expansion collapses to tile 0.  Phase 1 stitches one sampler at base_unit
 * with its geometry at const_byte_offset. */
static bool
r3v_nir_stitch_samplers(nir_shader *nir, uint32_t base_unit,
                           unsigned const_byte_offset)
{
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      nir_variable *tile_var[R3V_NEAREST_STITCH_TILE_UNITS] = {0};
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block_safe(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            if (tex->op != nir_texop_tex || tex->coord_components != 2)
               continue;
            const int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
            if (ci < 0)
               continue;
            nir_def *coord = tex->src[ci].src.ssa;

            b.cursor = nir_after_instr(instr);
            nir_def *cu = nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0),
                                       nir_imm_int(&b, const_byte_offset),
                                       .align_mul = 16,
                                       .range_base = const_byte_offset, .range = 32);
            nir_def *cv = nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0),
                                       nir_imm_int(&b, const_byte_offset + 16),
                                       .align_mul = 16,
                                       .range_base = const_byte_offset, .range = 32);
            nir_def *u = nir_channel(&b, coord, 0);
            nir_def *v = nir_channel(&b, coord, 1);
            /* r300's nir_to_rc has no ffma opcode (it fuses fmul+fadd into a MAD
             * itself), so emit the multiply and add separately. */
            nir_def *uc[2] = {
               nir_fmul(&b, u, nir_channel(&b, cu, 0)),
               nir_fadd(&b, nir_fmul(&b, u, nir_channel(&b, cu, 1)),
                        nir_channel(&b, cu, 2)),
            };
            nir_def *vc[2] = {
               nir_fmul(&b, v, nir_channel(&b, cv, 0)),
               nir_fadd(&b, nir_fmul(&b, v, nir_channel(&b, cv, 1)),
                        nir_channel(&b, cv, 2)),
            };
            nir_def *s[R3V_NEAREST_STITCH_TILE_UNITS];
            for (unsigned row = 0; row < 2; row++)
               for (unsigned col = 0; col < 2; col++) {
                  nir_def *tc = nir_vec2(&b, uc[col], vc[row]);
                  s[row * 2 + col] = r3v_stitch_tile_sample(
                     &b, nir, tile_var, base_unit, row * 2 + col, tex, tc);
               }
            nir_def *right  = nir_fge(&b, u, nir_channel(&b, cu, 3));
            nir_def *bottom = nir_fge(&b, v, nir_channel(&b, cv, 3));
            nir_def *top = nir_bcsel(&b, right, s[1], s[0]);
            nir_def *bot = nir_bcsel(&b, right, s[3], s[2]);
            nir_def *res = nir_bcsel(&b, bottom, bot, top);
            nir_def_rewrite_uses(&tex->def, res);
            nir_instr_remove(instr);
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_control_flow);
   }
   return progress;
}

static const struct glsl_type *
r3v_block0_ubo_type(unsigned size_bytes)
{
   return glsl_array_type(glsl_vec4_type(), DIV_ROUND_UP(size_bytes, 16), 16);
}

static const struct glsl_type *
r3v_block0_ubo_interface_type(const struct glsl_type *ubo_type)
{
   struct glsl_struct_field field = {
      .type = ubo_type,
      .name = "data",
      .location = -1,
   };
   return glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD430, false,
                              "__r3v_block0_ubo");
}

static unsigned
r3v_ubo_interface_size(const nir_variable *ubo)
{
   return ubo->interface_type
          ? glsl_get_explicit_size(ubo->interface_type, false) : 0;
}

static nir_variable *
r3v_find_block0_ubo(nir_shader *nir)
{
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo) {
      if (var->data.driver_location == 0)
         return var;
   }

   return NULL;
}

static void
r3v_shape_block0_ubo(nir_variable *ubo, unsigned size_bytes)
{
   const struct glsl_type *ubo_type = r3v_block0_ubo_type(size_bytes);

   ubo->type = ubo_type;
   ubo->data.driver_location = 0;
   ubo->data.binding = 0;
   ubo->data.explicit_binding = 1;
   ubo->interface_type = r3v_block0_ubo_interface_type(ubo_type);
}

/* Ensure block 0 has a sized UBO declaration before r300g constant-file setup.
 * The compiler sizes RC constants from nir_var_mem_ubo interface declarations;
 * load_ubo instructions alone do not carry the declaration size.  Reusing a
 * prior UBO0 declaration prevents an unused application block at index 0 from
 * colliding with a second synthetic UBO0 variable of a different interface
 * size. */
static void
r3v_declare_block0_ubo(nir_shader *nir, unsigned size_bytes)
{
   nir_variable *ubo = r3v_find_block0_ubo(nir);
   if (!ubo) {
      ubo = nir_variable_create(nir, nir_var_mem_ubo,
                                r3v_block0_ubo_type(size_bytes),
                                "r3v_block0_ubo");
   }
   if (r3v_ubo_interface_size(ubo) < size_bytes)
      r3v_shape_block0_ubo(ubo, size_bytes);

   nir->info.num_ubos = MAX2(nir->info.num_ubos, 1);
   nir->info.first_ubo_is_default_ubo = true;
}

/* Lower push-constant loads onto the single constant file.  nir_lower_explicit_io
 * with push_const has turned each access into load_push_constant(offset) with a
 * BASE/RANGE; rewrite it to load_ubo(block 0, BASE + offset) so it flows through
 * the same nir_lower_ubo_vec4 + index-0 path as the descriptor UBO.  Replay binds
 * the running push-constant window at CONST[0] (r3v_bind_push_constants), and a
 * push-constant + UBO collision is already rejected, so block 0 is unambiguous. */
static void
r3v_nir_lower_push_constant_to_ubo0(nir_shader *nir)
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

   /* Declare a block-0 UBO sized to the 128-byte push-constant window
    * (maxPushConstantsSize, the same .range bound above) so the constant file
    * covers every push slot.  Without it nir_to_tgsi/nir_to_rc would size an
    * empty constant file from the (absent) UBO variable and the gallivm draw
    * backend (draw-use-llvm) would assert on the CONST[0] read in
    * lp_build_emit_fetch_src (Register.Index <= file_max).  See
    * r3v_declare_block0_ubo for the externals_count mechanism. */
   r3v_declare_block0_ubo(nir, 128);
}

/* R300's constant file (RC_FILE_CONSTANT) is float-typed and the ISA has no native
 * integers (r300_screen.c caps->integers = false).  nir_lower_int_to_float rewrites
 * integer load_const literals to their float value but SKIPS intrinsics, so an
 * integer push-constant value reaches the shader as raw bits while the literals it
 * is compared against become floats -- e.g. switch(kind) compares the bit pattern
 * 0x00000002 (a denormal) against 2.0f and never matches.  Rather than reject,
 * the replay converts each integer push-constant word from its raw int bits to
 * the float value those float-encoded integer ops expect; classify which words
 * are integer here so the replay knows what to convert.
 *
 * Mark one bit per 4-byte word a leaf integer (or bool) member occupies.  Walks
 * the block layout (explicit std140/std430 offsets), so an integer member that
 * is declared but never read is converted harmlessly; words past the 128-byte
 * (32-word) window are ignored (a larger push range is rejected elsewhere). */
static void
r3v_mark_push_const_int_words(const struct glsl_type *type,
                                 unsigned base_off, uint32_t *mask)
{
   if (glsl_type_is_array(type)) {
      const struct glsl_type *elem = glsl_get_array_element(type);
      unsigned stride = glsl_get_explicit_stride(type);
      for (unsigned i = 0; i < glsl_get_length(type); i++)
         r3v_mark_push_const_int_words(elem, base_off + i * stride, mask);
   } else if (glsl_type_is_struct(type) || glsl_type_is_interface(type)) {
      for (unsigned i = 0; i < glsl_get_length(type); i++)
         r3v_mark_push_const_int_words(
            glsl_get_struct_field(type, i),
            base_off + glsl_get_struct_field_offset(type, i), mask);
   } else {
      const enum glsl_base_type bt = glsl_get_base_type(type);
      if (bt != GLSL_TYPE_INT && bt != GLSL_TYPE_UINT && bt != GLSL_TYPE_BOOL)
         return;
      /* A matrix column has the explicit stride; a scalar/vector packs its
       * components contiguously at 4 bytes each. */
      const unsigned cols = glsl_type_is_matrix(type)
                            ? glsl_get_matrix_columns(type) : 1;
      const unsigned col_stride = glsl_type_is_matrix(type)
                                  ? glsl_get_explicit_stride(type) : 0;
      const unsigned rows = glsl_get_vector_elements(type);
      for (unsigned c = 0; c < cols; c++)
         for (unsigned r = 0; r < rows; r++) {
            const unsigned word = (base_off + c * col_stride + r * 4) / 4;
            if (word < 32)
               *mask |= 1u << word;
         }
   }
}

/* Integer-word mask for the shader's push-constant block, or 0 if it has none.
 * Run before nir_lower_explicit_io removes the block variable. */
static uint32_t
r3v_classify_push_const_ints(nir_shader *nir)
{
   uint32_t mask = 0;
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_push_const)
      r3v_mark_push_const_int_words(var->type, 0, &mask);
   return mask;
}

/* r300's constant file is addressed by a compile-time vec4 slot plus component,
 * so a runtime (dynamically-indexed) offset cannot be represented.  Two distinct
 * shader shapes trip this: a push-constant offset and a UBO byte offset.  After
 * nir_lower_ubo_vec4 a non-constant offset becomes a runtime vector_extract; the
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
 * whether every constant access fits and whether any offset is non-constant; only
 * then pay for a clone + r300_optimize_nir (the same optimization pass used by
 * r300_create_*_state) to fold the loop-bounded case before judging it dynamic
 * (dynamic_index_vert indexes by a gl_Position-derived value that never folds).
 *
 * Shared by the push-constant gate (load_push_constant, src[0], slot-straddle
 * checked) and the UBO-offset gate (load_ubo_vec4, src[1]); together with the
 * dynamic-UBO-index reject in r3v_nir_lower_vulkan_resource_index_single (the
 * index selects which UBO, the offset selects where within it) they form the
 * complete "r300 constant-file representability" gate. */
static bool
r3v_nir_static_offset_ok(nir_intrinsic_instr *intr, unsigned off_src,
                            bool straddle, bool *maybe_dynamic)
{
   if (!nir_src_is_const(intr->src[off_src])) {
      *maybe_dynamic = true;
      return true;
   }

   if (!straddle)
      return true;

   uint32_t off = (uint32_t)nir_src_as_uint(intr->src[off_src]);
   unsigned byte_width = intr->def.num_components * (intr->def.bit_size / 8u);
   return (off & 15u) + byte_width <= 16u;
}

static bool
r3v_nir_scan_static_offsets(nir_shader *nir, nir_intrinsic_op op,
                               unsigned off_src, bool straddle,
                               bool *maybe_dynamic)
{
   *maybe_dynamic = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != op)
               continue;
            if (!r3v_nir_static_offset_ok(intr, off_src, straddle,
                                             maybe_dynamic))
               return false;
            if (*maybe_dynamic)
               return true;
         }
      }
   }

   return true;
}

static bool
r3v_nir_offsets_static(struct pipe_screen *pscreen, nir_shader *nir,
                          nir_intrinsic_op op, unsigned off_src, bool straddle)
{
   bool maybe_dynamic = false;
   if (!r3v_nir_scan_static_offsets(nir, op, off_src, straddle,
                                       &maybe_dynamic))
      return false;
   if (!maybe_dynamic)
      return true;

   nir_shader *check = nir_shader_clone(NULL, nir);
   r300_optimize_nir(check, r300_screen(pscreen));

   bool ok = r3v_nir_scan_static_offsets(check, op, off_src, straddle,
                                            &maybe_dynamic);
   ralloc_free(check);
   return ok && !maybe_dynamic;
}

/* BASE is 0 for push constants (after nir_lower_explicit_io), so src[0] is the
 * full byte offset; slot-straddle matters. */
static bool
r3v_nir_push_const_shape_ok(struct pipe_screen *pscreen, nir_shader *nir)
{
   return r3v_nir_offsets_static(pscreen, nir,
                                    nir_intrinsic_load_push_constant, 0, true);
}

/* Defined below; the input-attachment path needs the identity NEAREST sampler
 * CSO that this lazily creates, so the FS compile ensures it exists. */
static bool
r3v_device_init_identity_map_state(struct r3v_device *device);

/* True if the shader reads gl_ViewIndex (multiview), in either the deref form
 * vk_spirv_to_nir emits or the lowered load_view_index intrinsic. */
static bool
r3v_nir_uses_view_index(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_view_index)
               return true;
            if (intr->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
               if (d && d->deref_type == nir_deref_type_var && d->var &&
                   d->var->data.mode == nir_var_system_value &&
                   d->var->data.location == SYSTEM_VALUE_VIEW_INDEX)
                  return true;
            }
         }
      }
   }
   return false;
}

/* Gallium's vertex rows use driver_location as a dense AOS index.  Vulkan
 * vertex locations are already validated as a contiguous prefix, so preserve
 * that location mapping and publish the resulting row span in num_inputs. */
static bool
r3v_assign_vs_input_locations(nir_shader *nir)
{
   unsigned input_span = 0;

   nir_foreach_shader_in_variable(var, nir) {
      if (var->data.location < VERT_ATTRIB_GENERIC0)
         return false;

      const unsigned driver_location =
         var->data.location - VERT_ATTRIB_GENERIC0;
      const unsigned slots =
         glsl_count_attribute_slots(var->type, false);
      if (driver_location >= PIPE_MAX_ATTRIBS ||
          slots > PIPE_MAX_ATTRIBS - driver_location)
         return false;

      var->data.driver_location = driver_location;
      input_span = MAX2(input_span, driver_location + slots);
   }

   nir->num_inputs = input_span;
   return true;
}

static VkResult
r3v_compile_shader(struct r3v_device *device,
                       const VkPipelineShaderStageCreateInfo *stage_info,
                       struct r3v_pipeline *pl,
                       const VkPipelineVertexInputStateCreateInfo *vi)
{
   /* r300g exposes VS and FS only; geometry, tessellation, and compute are
    * unsupported on R300-class hardware. */
   if (stage_info->stage != VK_SHADER_STAGE_VERTEX_BIT &&
       stage_info->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: unsupported shader stage 0x%x",
                       stage_info->stage);

   VK_FROM_HANDLE(r3v_shader_module, mod, stage_info->module);
   mesa_shader_stage stage = vk_to_mesa_shader_stage(stage_info->stage);

   const struct nir_shader_compiler_options *nir_opts =
      device->screen->nir_options[stage];

   nir_shader *nir = vk_spirv_to_nir(&device->vk,
                                      mod->code, mod->code_size,
                                      stage, stage_info->pName,
                                      stage_info->pSpecializationInfo,
                                      &r3v_spirv_opts, nir_opts,
                                      false, NULL);
   if (!nir)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: vk_spirv_to_nir failed for %s shader",
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

   /* r300 has no buffer-texture unit and no storage path.  A shader that reads a
    * uniform or storage texel buffer lowers to a dummy in the fragment translator and
    * asserts in the SW-TCL draw module's nir_to_tgsi for the vertex stage, so reject
    * the pipeline at compile. */
   if (r3v_nir_uses_buffer_resource(nir)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: %s shader uses a uniform or storage texel buffer; "
                       "r300 has no buffer-resource unit",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* Lower a fragment subpassLoad to a normalized texture() (r300 has no
    * texelFetch) before the constant/UBO lowering below.  It injects inv_extent
    * at CONST[0], so the collision check below rejects an input-attachment shader
    * that also reads an app UBO or push constants.  Runs before
    * nir_lower_explicit_io/nir_lower_ubo_vec4 so the emitted load_ubo(0) follows
    * the same vec4-slot path as the keystone UBO. */
   const bool stage_had_texture =
      stage_info->stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
      r3v_nir_uses_texture(nir);
   /* Flatten fragment sampler descriptors from every set into r300's one texture-
    * unit space using the map create_one_pipeline built from the pipeline layout,
    * so nir_lower_samplers and the replay agree on the unit for a sampler in any
    * descriptor set.  The map is bounded by R3V_MAX_FS_SAMPLER_UNITS at create
    * time, so a layout that overflows the units is already rejected there. */
   if (stage_had_texture)
      r3v_nir_remap_sampler_units(nir, pl);

   /* Experimental NEAREST tile-stitch: expand each fragment texture() into the
    * tiled-sampler form so a >2048 (multi-tile) sampled image samples the correct
    * tile instead of only the first.  Phase 1 wires one stitched sampler at unit 0
    * with its per-image affine/split geometry in the first two block-0 CONST vec4s
    * (declared here so externals_count covers them); the replay binds the four
    * tile views and uploads the geometry.  Off unless the gate is set. */
   if (stage_had_texture && r3v_experimental_nearest_stitch_enabled()) {
      if (r3v_nir_stitch_samplers(nir, 0, 0))
         r3v_declare_block0_ubo(nir, R3V_NEAREST_STITCH_CONST_VEC4S * 16);
   }

   bool stage_has_input = false;
   uint32_t stage_input_set = 0;
   uint32_t stage_input_binding = 0;
   bool stage_input_multiple = false;
   bool stage_input_integer = false;
   if (stage_info->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
      r3v_nir_lower_subpass_input(nir, &stage_has_input, &stage_input_set,
                                     &stage_input_binding, &stage_input_multiple,
                                     &stage_input_integer);

   /* r300 has no integer texture path: an isubpassInput/usubpassInput cannot be
    * read on the FP24 fragment ALU.  Reject rather than emit a malformed tex. */
   if (stage_has_input && stage_input_integer) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: fragment shader reads an integer (SINT/UINT) "
                       "subpass input; r300's FP24 fragment ALU has no integer "
                       "texture path");
   }

   /* The replay binds a single input attachment descriptor per pipeline, so a
    * fragment shader reading two distinct input descriptors
    * would leave the others reading whatever was last bound.  Reject rather than
    * render the wrong attachment. */
   if (stage_has_input && stage_input_multiple) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: fragment shader reads more than one distinct "
                       "input attachment; the replay binds a single input "
                       "attachment per pipeline");
   }

   /* The lowered subpass input occupies sampler unit zero to satisfy r300g's
    * sampler-update contract.  Until ordinary texture descriptors are gathered
    * into one unit-zero sampler array, an app texture in the same fragment
    * shader can collide with the input attachment; reject rather than let the
    * last replay bind decide which image the shader samples. */
   if (stage_has_input && stage_had_texture) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: fragment shader combines a subpass input with "
                       "a sampled image; r3v reserves sampler unit zero for "
                       "the lowered input attachment");
   }

   /* Push constants and a UBO both resolve to CONST[0]; r300's single constant
    * file cannot host both, so reject the pair before the lowering below rewrites
    * the UBO chain away.  A shader using only one of them is supported. */
   const bool uses_push_const = r3v_nir_uses_push_constants(nir);
   if (uses_push_const && r3v_nir_uses_ubo(nir)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: %s shader uses both push constants and a uniform "
                       "buffer; r300's single constant file holds only one at "
                       "CONST[0]",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* A lowered subpassLoad reads inv_extent from CONST[0]; an input-attachment
    * fragment shader that also reads an app UBO or push constants would need two
    * CONST[0] contents.  Reject rather than render wrong pixels. */
   if (stage_has_input && (uses_push_const || r3v_nir_uses_ubo(nir))) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: fragment shader combines a subpass input with a "
                       "uniform buffer or push constants; r300's single CONST[0] "
                       "holds only the input's inv_extent");
   }
   if (stage_has_input) {
      pl->fs_has_input_attachment = true;
      pl->fs_input_attachment_set = stage_input_set;
      pl->fs_input_attachment_binding = stage_input_binding;
      /* The lowered subpassLoad reads inv_extent from a CONST[0] load_ubo but
       * leaves no UBO variable behind.  Declare one vec4 of block-0 UBO so
       * externals_count >= 1 and the gl_FragCoord wpos viewport-transform state
       * constants land above c0 instead of colliding with inv_extent there
       * (which would corrupt the window-space coordinate the sample uses).
       * A subpass input combined with an app UBO or push constants was rejected
       * above, so this is the only block-0 UBO. */
      r3v_declare_block0_ubo(nir, 16);
      /* r3v_bind_input_attachment binds device->identity_map_cso.sampler
       * (NEAREST, CLAMP_TO_EDGE) as the input-attachment sampler at draw time.
       * That CSO is created lazily and otherwise only by the compute identity-map
       * paths, so a graphics-only input-attachment pipeline would bind a NULL
       * sampler and the fragment texture fetch would read undefined data.  Create
       * it now so the draw has a valid sampler. */
      if (!r3v_device_init_identity_map_state(device)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "r3v: failed to create the input-attachment sampler "
                          "state");
      }
   }

   /* Resolve the descriptor resource chain (vulkan_resource_index ->
    * load_vulkan_descriptor) to a constant block-0 address, or reject the
    * pipeline when the shader needs a resource r300's single read-only constant
    * file cannot represent.  See r3v_nir_lower_vulkan_resource_index_single. */
   bool stage_has_ubo = false;
   uint32_t stage_ubo_set = 0, stage_ubo_binding = 0;
   if (!r3v_nir_lower_vulkan_resource_index_single(nir, &stage_has_ubo,
                                                      &stage_ubo_set,
                                                      &stage_ubo_binding)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: %s shader uses a descriptor resource r300's "
                       "single read-only constant file cannot represent "
                       "(only one static UBO descriptor is supported; storage "
                       "buffers, sampled resources, multiple UBOs, and dynamic "
                       "descriptor indices are rejected)",
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
    * (r3v_bind_push_constants).  A push-constant + UBO collision was rejected
    * above, so block 0 is unambiguous.  r300's constant file is float-only, so an
    * integer push-constant word is uploaded as the float value its lowered ops
    * expect: classify the integer words here (before nir_lower_explicit_io drops
    * the block variable) and the replay converts them.  A dynamic/slot-straddling
    * offset still cannot be represented and is rejected below. */
   if (uses_push_const) {
      pl->push_const_int_word_mask |= r3v_classify_push_const_ints(nir);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
               nir_address_format_32bit_offset);
      if (!r3v_nir_push_const_shape_ok(device->screen, nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: %s shader uses a dynamic or slot-straddling "
                          "push-constant offset; r300 constant-file addressing is "
                          "static and 16-byte-slot granular",
                          stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                          ? "vertex" : "fragment");
      }
      r3v_nir_lower_push_constant_to_ubo0(nir);
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
   r3v_nir_remap_single_ubo_to_index0(nir);

   /* Reject a dynamic UBO offset for the FRAGMENT stage only.  The PFS constant
    * file is addressed by a static vec4 slot with no relative addressing, so a
    * runtime load_ubo_vec4 offset cannot be emitted there.  The vertex shader
    * runs on the SW-TCL draw module (tgsi_exec), which the r300 screen now
    * advertises as integer- and indirect-const-capable, so a runtime UBO offset
    * (ubuf.arr[gl_VertexIndex]) is representable for the vertex stage. */
   if (stage_info->stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
       !r3v_nir_offsets_static(device->screen, nir,
                                  nir_intrinsic_load_ubo_vec4, 1, false)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: %s shader uses a dynamic uniform-buffer offset; "
                       "r300's constant file is addressed by a static vec4 slot",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* r3v is apiVersion 1.0 with no VK_KHR_multiview: it reserves no per-view
    * VS system-value stream and the SW-TCL nir_to_tgsi path has no mapping for the
    * view-index builtin, so a shader reading gl_ViewIndex reaches ureg_swizzle as a
    * null TGSI source and aborts (tgsi_ureg.h reg.File != TGSI_FILE_NULL).  Reject
    * the multiview shader rather than crash. */
   if (r3v_nir_uses_view_index(nir)) {
      ralloc_free(nir);
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: %s shader reads gl_ViewIndex; multiview is not "
                       "supported on the RS480 SW-TCL path",
                       stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT
                       ? "vertex" : "fragment");
   }

   /* vk_spirv_to_nir sets data.location for VS inputs (VERT_ATTRIB_GENERIC0+n)
    * but leaves data.driver_location at zero for all variables.  nir_lower_io
    * inside r300g's nir_to_rc uses driver_location as the RC input base, so
    * all inputs would collapse to IN[0] without this assignment. */
   if (stage_info->stage == VK_SHADER_STAGE_VERTEX_BIT) {
      /* RS480-family has no hardware vertex texture units, and the SW-TCL draw
       * module that runs the vertex shader has no sampler views bound by r3v,
       * so a vertex texture fetch reaches tgsi_exec fetch_texel with a NULL
       * sampler (vs_exec_run_linear -> r300_swtcl_draw_vbo) and segfaults at
       * draw.  Reject a vertex shader that samples rather than crash.  A
       * conformant vertex-texturing path must bind the draw module's
       * PIPE_SHADER_VERTEX sampler views and sampler state before this gate can
       * be removed. */
      if (r3v_nir_uses_live_texture_after_r300_opt(device->screen, nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex shader samples a texture; the RS480 "
                          "SW-TCL vertex path binds no sampler and cannot fetch "
                          "from a vertex stage");
      }

      /* vk_spirv_to_nir leaves gl_VertexIndex / gl_InstanceIndex as a load_deref
       * of a nir_var_system_value variable and never gathers system_values_read,
       * so nir->info cannot be trusted to flag the read here.  Scan the NIR for
       * both the deref and the lowered-intrinsic form instead. */
      bool needs_vid = false, needs_iid = false;
      r300_nir_vs_reads_system_values(nir, &needs_vid, &needs_iid);
      /* The synthetic-attribute stream is the default: it delivers
       * VertexIndex/InstanceIndex through an ordinary shader_in variable, the
       * same load_deref form nir_lower_int_to_float leaves untouched as any
       * other integer vertex attribute, so a shader that compares
       * gl_VertexIndex against a reference attribute (both sides' raw bit
       * patterns reinterpreted as float compare equal iff the original ints
       * did) reads correctly.  The native-intrinsic path
       * (r300_nir_lower_vs_system_values_to_intrinsics) instead becomes a
       * load_vertex_id / load_instance_id the draw module's tgsi_exec supplies
       * directly, consuming no PSC velem slot -- but r300_vs_draw.c's
       * r300_nir_float_encode_int_sysvals float-encodes that intrinsic's
       * result (needed so instance/vertex-id arithmetic, e.g. an array index,
       * lands in the same float domain as the rest of the shader), which
       * breaks the raw-bit-pattern symmetry the synthetic path relied on.  Try
       * the synthetic reservation first and take the native, slot-freeing path
       * only when the synthetic stream has no room -- the worst-case
       * maxVertexInputAttributes-plus-VertexIndex shape this frees is rare,
       * and every ordinary shader keeps the synthetic path's exact prior
       * behavior. */
      if (needs_vid || needs_iid) {
         int vid_slot = -1;
         int iid_slot = -1;
         VkResult r = r3v_reserve_vs_system_value_streams(
            device, pl, vi, needs_vid, needs_iid, &vid_slot, &iid_slot);
         if (r == VK_ERROR_FEATURE_NOT_PRESENT) {
            if (!r300_nir_lower_vs_system_values_to_intrinsics(nir)) {
               ralloc_free(nir);
               return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                                "r3v: failed to lower VS system values to "
                                "native intrinsics");
            }
            needs_vid = needs_iid = false;
         } else if (r != VK_SUCCESS) {
            ralloc_free(nir);
            return r;
         } else {
            if (!r300_nir_lower_vs_system_values_to_inputs(nir, vid_slot,
                                                           iid_slot)) {
               ralloc_free(nir);
               return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                                "r3v: failed to lower VS system values");
            }
            needs_vid = needs_iid = false;
         }
      }
      if (needs_vid || needs_iid) {
         bool still_needs_vid = false, still_needs_iid = false;
         r300_nir_vs_reads_system_values(nir, &still_needs_vid,
                                         &still_needs_iid);
         if (still_needs_vid || still_needs_iid) {
            ralloc_free(nir);
            return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                             "r3v: VS system-value lowering left an "
                             "unsupported read");
         }
      }

      if (!r3v_assign_vs_input_locations(nir)) {
         ralloc_free(nir);
         return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                          "r3v: vertex shader input locations exceed the "
                          "r300 Gallium attribute span");
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
 * r3v_create_one_pipeline within the CCN budget. */
static VkResult
r3v_vertex_element_count(struct r3v_device *device,
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
      if (attr->location >= R3V_MAX_VERTEX_BINDINGS)
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r3v: vertex attribute location %u exceeds %u",
                          attr->location, R3V_MAX_VERTEX_BINDINGS - 1);
      if (location_mask & BITFIELD_BIT(attr->location))
         return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                          "r3v: duplicate vertex attribute location %u",
                          attr->location);

      location_mask |= BITFIELD_BIT(attr->location);
      element_count = MAX2(element_count, attr->location + 1);
   }

   if (element_count > 0 && location_mask != BITFIELD_MASK(element_count))
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: sparse vertex attribute locations are not "
                       "representable by r300g vertex elements");

   *element_count_out = element_count;
   return VK_SUCCESS;
}

static VkResult
r3v_populate_vertex_element(struct r3v_device *device,
                                struct r3v_pipeline *pl,
                                const VkPipelineVertexInputStateCreateInfo *vi,
                                uint32_t attr_index,
                                struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS])
{
   const VkVertexInputAttributeDescription *attr =
      &vi->pVertexAttributeDescriptions[attr_index];
   if (attr->binding >= R3V_MAX_VERTEX_BINDINGS)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: vertex attribute binding %u exceeds %u",
                       attr->binding, R3V_MAX_VERTEX_BINDINGS - 1);

   enum pipe_format elem_fmt = r3v_vk_format_to_pipe_format(attr->format);
   if (elem_fmt == PIPE_FORMAT_NONE)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r3v: unsupported vertex attribute format %d "
                       "at location %u", attr->format, attr->location);
   const uint32_t attr_size = r3v_vertex_attr_data_size(elem_fmt);
   if (attr->offset > UINT32_MAX - attr_size)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: vertex attribute offset %u exceeds "
                       "representable binding extent", attr->offset);

   struct pipe_vertex_element *elem = &ve[attr->location];
   elem->src_offset          = (uint16_t)attr->offset;
   elem->vertex_buffer_index = (uint8_t)attr->binding;
   elem->src_format          = (uint8_t)elem_fmt;
   const VkVertexInputBindingDescription *binding_desc =
      r3v_find_vertex_binding_desc(vi, attr->binding);
   if (!binding_desc)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: vertex attribute binding %u has no "
                       "matching binding description", attr->binding);

   elem->src_stride = binding_desc->stride;
   elem->instance_divisor =
      binding_desc->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 1 : 0;
   pl->vertex_stride[attr->binding] = binding_desc->stride;
   pl->vertex_binding_extent[attr->binding] =
      MAX2(pl->vertex_binding_extent[attr->binding],
           attr->offset + attr_size);
   pl->vertex_binding_mask |= BITFIELD_BIT(attr->binding);
   if (binding_desc->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
      pl->vertex_instance_binding_mask |= BITFIELD_BIT(attr->binding);

   return VK_SUCCESS;
}

static VkResult
r3v_build_velems_cso(struct r3v_device *device,
                         struct r3v_pipeline *pl,
                         const VkPipelineVertexInputStateCreateInfo *vi)
{
   struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
   uint32_t n = 0;
   VkResult result = r3v_vertex_element_count(device, vi, &n);
   if (result != VK_SUCCESS)
      return result;

   if (vi) {
      for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++) {
         const VkVertexInputBindingDescription *desc =
            &vi->pVertexBindingDescriptions[b];
         if (desc->binding >= R3V_MAX_VERTEX_BINDINGS)
            return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                             "r3v: vertex binding %u exceeds %u",
                             desc->binding, R3V_MAX_VERTEX_BINDINGS - 1);
      }
   }

   memset(ve, 0, sizeof(ve));
   for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
      VkResult res = r3v_populate_vertex_element(device, pl, vi, i, ve);
      if (res != VK_SUCCESS)
         return res;
   }

   /* Append synthetic VS-system-value elements the VS reads via the lowering
    * in r3v_compile_shader.  instance_divisor 0 steps the vertex-id element
    * per vertex; 1 steps the instance-id element per instance.
    *
    * The element format is R32_FLOAT, not R32_SINT: r300_vs_draw.c runs
    * nir_lower_int_to_float over the whole SW-TCL vertex shader, and every
    * ALU op downstream expects its integer-valued operands already encoded
    * as a genuine float (2 stored as the bits of 2.0f).  An R32_SINT element
    * hits the pure_integer path in lp_build_fetch_rgba_aos_array
    * (lp_bld_format_aos_array.c), which bitcasts the raw int32 element index
    * into the float-typed vertex-fetch register instead of converting it --
    * the LLVM draw-JIT backend always fetches vertex elements at a float
    * dst_type, so index 2 arrives as the subnormal 2.8e-45, not 2.0f, and a
    * float-domain selection ladder keyed on the synthetic instance index
    * always takes its first branch.  R32_FLOAT skips the pure_integer branch
    * entirely: r3v_bind_synthetic_identity_stream writes the identity value
    * as a real float, so the fetch's ordinary floating-point conversion path
    * delivers it unchanged. */
   uint32_t velem_count = n;
   if (pl->needs_vertex_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->vertex_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_FLOAT;
      ve[velem_count].src_stride          = sizeof(float);
      ve[velem_count].instance_divisor    = 0;
      velem_count++;
   }
   if (pl->needs_instance_id_stream && velem_count < PIPE_MAX_ATTRIBS) {
      ve[velem_count].src_offset          = 0;
      ve[velem_count].vertex_buffer_index = pl->instance_id_vb_binding;
      ve[velem_count].src_format          = (uint8_t)PIPE_FORMAT_R32_FLOAT;
      ve[velem_count].src_stride          = sizeof(float);
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
r3v_blend_factor_to_pipe(VkBlendFactor f)
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
r3v_blend_op_to_pipe(VkBlendOp op)
{
   STATIC_ASSERT((unsigned)VK_BLEND_OP_ADD == PIPE_BLEND_ADD &&
                 (unsigned)VK_BLEND_OP_SUBTRACT == PIPE_BLEND_SUBTRACT &&
                 (unsigned)VK_BLEND_OP_REVERSE_SUBTRACT == PIPE_BLEND_REVERSE_SUBTRACT &&
                 (unsigned)VK_BLEND_OP_MIN == PIPE_BLEND_MIN &&
                 (unsigned)VK_BLEND_OP_MAX == PIPE_BLEND_MAX);
   return (unsigned)op <= PIPE_BLEND_MAX ? (unsigned)op : PIPE_BLEND_ADD;
}

unsigned
r3v_cull_mode_to_pipe(VkCullModeFlags cull)
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
r3v_compare_op_to_pipe(VkCompareOp op)
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
r3v_stencil_op_to_pipe(VkStencilOp op)
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
r3v_stencil_face_to_pipe(const VkStencilOpState *vk_face,
                            bool enabled,
                            struct pipe_stencil_state *out)
{
   out->enabled   = enabled;
   out->func      = r3v_compare_op_to_pipe(vk_face->compareOp);
   out->fail_op   = r3v_stencil_op_to_pipe(vk_face->failOp);
   out->zpass_op  = r3v_stencil_op_to_pipe(vk_face->passOp);
   out->zfail_op  = r3v_stencil_op_to_pipe(vk_face->depthFailOp);
   out->valuemask = (uint8_t)vk_face->compareMask;
   out->writemask = (uint8_t)vk_face->writeMask;
}

static VkResult
r3v_init_graphics_pipeline_cso_state(struct r3v_device *device,
                                        struct r3v_pipeline *pl,
                                        const VkGraphicsPipelineCreateInfo *info)
{
   /* Translate the pipeline-static colour-blend state.  r300 shares one blend
    * state and colour mask across all MRT cbufs, so independentBlend is false
    * and Vulkan guarantees every pAttachments[] entry is identical; taking
    * pAttachments[0] is therefore correct for all bound attachments.
    * VkColorComponentFlags shares the R/G/B/A bit order with PIPE_MASK_*; absent
    * state (rasterizer discard or no colour attachment) leaves blending off with
    * a full writemask. */
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
      bs.rt[0].rgb_func         = r3v_blend_op_to_pipe(att->colorBlendOp);
      bs.rt[0].rgb_src_factor   = r3v_blend_factor_to_pipe(att->srcColorBlendFactor);
      bs.rt[0].rgb_dst_factor   = r3v_blend_factor_to_pipe(att->dstColorBlendFactor);
      bs.rt[0].alpha_func       = r3v_blend_op_to_pipe(att->alphaBlendOp);
      bs.rt[0].alpha_src_factor = r3v_blend_factor_to_pipe(att->srcAlphaBlendFactor);
      bs.rt[0].alpha_dst_factor = r3v_blend_factor_to_pipe(att->dstAlphaBlendFactor);
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
    * overlays only the R3V_DYN_* bits the pipeline declared dynamic AND
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
   /* Vulkan clip space is half-z: NDC z is [0,1], not GL's [-1,1].  The r300
    * backend reads clip_halfz to set the depth clip range (r300_set_rs_state ->
    * r300->clip_halfz, consumed by the SW-TCL draw clip stage), and
    * viewport_vk_to_gallium already maps [0,1] to [minDepth,maxDepth] directly.
    * Without clip_halfz the near-plane clip admits NDC z in [-1,0) that Vulkan
    * discards; setting it tightens the near clip to z=0 without changing the
    * (already correct) window-depth values. */
   rs.clip_halfz  = true;
   /* Vulkan's scissor test always applies; the replay supplies the rectangle
    * (pipeline-static or CmdSetScissor) translated to live tile space. */
   rs.scissor     = true;
   rs.line_width  = 1.0f;
   rs.point_size  = 1.0f;
   if (vk_rs) {
      /* VK_EXT_depth_clip_enable: explicit clip control overrides the
       * default-on near/far clip (r3v does not expose depthClampEnable,
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
      rs.cull_face = r3v_cull_mode_to_pipe(vk_rs->cullMode);
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
      dsa.depth_func      = r3v_compare_op_to_pipe(vk_ds->depthCompareOp);
      r3v_stencil_face_to_pipe(&vk_ds->front, vk_ds->stencilTestEnable,
                                  &dsa.stencil[0]);
      r3v_stencil_face_to_pipe(&vk_ds->back, vk_ds->stencilTestEnable,
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
r3v_capture_dynamic_state(struct r3v_pipeline *pl,
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
         /* The R3V_DYN_* family: a vkCmdSet* value applies to a draw only
          * when the bound pipeline declared that state dynamic, so the replay
          * masks its merged Set* shadow with dyn_mask before overlaying the
          * pipeline's rs/dsa templates. */
         case VK_DYNAMIC_STATE_CULL_MODE:
            pl->dyn_mask |= R3V_DYN_CULL;
            break;
         case VK_DYNAMIC_STATE_FRONT_FACE:
            pl->dyn_mask |= R3V_DYN_FRONT_FACE;
            break;
         case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
            pl->dyn_mask |= R3V_DYN_TOPOLOGY;
            break;
         case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
            pl->dyn_mask |= R3V_DYN_DEPTH_TEST;
            break;
         case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
            pl->dyn_mask |= R3V_DYN_DEPTH_WRITE;
            break;
         case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
            pl->dyn_mask |= R3V_DYN_DEPTH_OP;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE:
            pl->dyn_mask |= R3V_DYN_DEPTH_BOUNDS;
            break;
         case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
            pl->dyn_mask |= R3V_DYN_STENCIL_TEST;
            break;
         case VK_DYNAMIC_STATE_STENCIL_OP:
            pl->dyn_mask |= R3V_DYN_STENCIL_OP;
            break;
         case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
            pl->dyn_mask |= R3V_DYN_STENCIL_CMP_MASK;
            break;
         case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
            pl->dyn_mask |= R3V_DYN_STENCIL_WR_MASK;
            break;
         case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
            pl->dyn_mask |= R3V_DYN_STENCIL_REF;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BIAS:
            pl->dyn_mask |= R3V_DYN_DEPTH_BIAS;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
            pl->dyn_mask |= R3V_DYN_DEPTH_BIAS_EN;
            break;
         case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
            pl->dyn_mask |= R3V_DYN_BLEND_CONST;
            break;
         case VK_DYNAMIC_STATE_LINE_WIDTH:
            pl->dyn_mask |= R3V_DYN_LINE_WIDTH;
            break;
         case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT:
            pl->dyn_mask |= R3V_DYN_LINE_STIPPLE;
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
r3v_graphics_pipeline_create_result(VkResult result)
{
   switch (result) {
   case VK_SUCCESS:
   case VK_ERROR_OUT_OF_HOST_MEMORY:
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:
   case VK_ERROR_UNKNOWN:
   case VK_ERROR_VALIDATION_FAILED:
   case VK_ERROR_INVALID_SHADER_NV:
   case VK_PIPELINE_COMPILE_REQUIRED:
      return result;
   default:
      return VK_ERROR_UNKNOWN;
   }
}

/* Build a minimal fragment program that writes a constant colour.  A graphics
 * pipeline can omit the fragment stage (a depth- or stencil-only draw produces
 * no colour), but the draw still rasterizes: the depth and stencil tests are ROP
 * functions that run without a fragment program.  r300_update_rs_block, however,
 * dereferences r300_fs()->shader unconditionally, so the SW-TCL draw needs a
 * bound fragment program -- and r3v_replay_draw skips any draw whose fs_cso is
 * NULL, which silently drops the depth/stencil work (the
 * dEQP-VK.pipeline.monolithic.stencil.nocolor.* cluster renders nothing).  Binding
 * this no-op fragment program lets the rasterizer execute the depth/stencil ops;
 * with no colour attachment the colour write is masked. */
static void *
r3v_synthesize_noop_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts, "r3v_noop_fs");

   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(&b, out, nir_imm_vec4(&b, 0.0f, 0.0f, 0.0f, 0.0f), 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   nir_shader *fs_nir = b.shader;
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, fs_nir, true);

   struct pipe_shader_state state = { .type   = PIPE_SHADER_IR_NIR,
                                      .ir.nir = fs_nir };
   void *fs_cso = pipe->create_fs_state(pipe, &state);
   if (!fs_cso)
      ralloc_free(fs_nir);
   return fs_cso;
}

static VkResult
r3v_create_one_pipeline(struct r3v_device *device,
                             const VkGraphicsPipelineCreateInfo *info,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   struct r3v_pipeline *pl;

   pl = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*pl), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pl)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &pl->base, VK_OBJECT_TYPE_PIPELINE);

#define FAIL_PIPELINE(r) \
   do { \
      VkResult fail_result = r3v_graphics_pipeline_create_result(r); \
      r3v_DestroyPipeline(r3v_device_to_handle(device), \
                             r3v_pipeline_to_handle(pl), pAllocator); \
      return fail_result; \
   } while (0)

   /* r300 exposes a single constant-buffer slot (max_const_buffers = 1) and binds
    * one flat push-constant window at CONST[0]; it cannot represent more than one
    * push-constant range (per-stage divergent or overlapping ranges share the slot
    * and alias).  Reject a multi-range layout at create time rather than render
    * wrong pixels. */
   VK_FROM_HANDLE(vk_pipeline_layout, pc_layout, info->layout);
   if (pc_layout && pc_layout->push_range_count > 1)
      FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                              "r3v: %u push-constant ranges; r300's single "
                              "constant slot supports at most one",
                              pc_layout->push_range_count));

   /* Assign every combined-image-sampler in the pipeline layout a flat fragment
    * texture unit, in (set, binding) order across all sets, so a sampler in any
    * descriptor set gets a unit the shader rewrite (r3v_nir_remap_sampler_units)
    * and the replay (r3v_bind_descriptor_textures) both honour.  A layout that
    * needs more units than r300 has is rejected here rather than aliasing them. */
   pl->fs_sampler_map_count = 0;
   const bool nearest_stitch = r3v_experimental_nearest_stitch_enabled();
   if (pc_layout) {
      uint32_t next_unit = 0;
      for (uint32_t set = 0; set < pc_layout->set_count; set++) {
         struct vk_descriptor_set_layout *vk_dsl = pc_layout->set_layouts[set];
         if (!vk_dsl)
            continue;
         const struct r3v_descriptor_set_layout *dsl =
            container_of(vk_dsl, struct r3v_descriptor_set_layout, base);
         for (uint32_t b = 0; b < dsl->binding_count; b++) {
            const struct r3v_dsl_binding *bnd = &dsl->bindings[b];
            if (bnd->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
               continue;
            /* Under the stitch gate each sampler reserves a 2x2 tile-unit grid so
             * its four per-tile fetches land on distinct units. */
            const uint32_t span = nearest_stitch
               ? MAX2(bnd->count, R3V_NEAREST_STITCH_TILE_UNITS)
               : bnd->count;
            if (pl->fs_sampler_map_count >= R3V_MAX_FS_SAMPLER_UNITS ||
                next_unit + span > R3V_MAX_FS_SAMPLER_UNITS)
               FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                             "r3v: pipeline layout declares more combined image "
                             "samplers than r300's %u fragment texture units",
                             R3V_MAX_FS_SAMPLER_UNITS));
            pl->fs_sampler_map[pl->fs_sampler_map_count].set = set;
            pl->fs_sampler_map[pl->fs_sampler_map_count].binding = bnd->binding;
            pl->fs_sampler_map[pl->fs_sampler_map_count].unit = next_unit;
            pl->fs_sampler_map_count++;
            next_unit += span;
            if (nearest_stitch)
               pl->fs_nearest_stitch = true;
         }
      }
   }

   for (uint32_t i = 0; i < info->stageCount; i++) {
      VkResult r = r3v_compile_shader(device, &info->pStages[i], pl,
                                         info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

   /* A pipeline with no fragment stage still rasterizes depth/stencil, but
    * r3v_replay_draw skips a draw whose fs_cso is NULL (r300_update_rs_block
    * would otherwise NULL-deref r300_fs()->shader).  Supply a no-op fragment
    * program so the depth/stencil ops run.  A pipeline that statically discards
    * rasterization produces nothing, so leave its fs_cso NULL and let the replay
    * skip it. */
   if (!pl->fs_cso) {
      const VkPipelineRasterizationStateCreateInfo *rs =
         info->pRasterizationState;
      if (!(rs && rs->rasterizerDiscardEnable)) {
         pl->fs_cso = r3v_synthesize_noop_fs(device->pipe);
         if (!pl->fs_cso)
            FAIL_PIPELINE(vk_error(device, VK_ERROR_INITIALIZATION_FAILED));
         pl->fs_hw_valid = r300_fs_get_hw_code(pl->fs_cso, &pl->fs_hw);
      }
   }

   /* r300 has separate vertex and fragment constant files, so a single shader
    * using both push constants and a UBO is the only true CONST[0] collision,
    * and r3v_compile_shader already rejects that per stage.  A pipeline that
    * splits them across stages (push constants in one, a UBO in the other) is
    * representable in hardware, but the replay is not split: it binds the
    * push-constant window to BOTH stages' CONST[0] (r3v_bind_push_constants)
    * whenever any stage uses push constants, which then cannot also bind the
    * other stage's UBO.  Reject the cross-stage mix rather than silently
    * overwrite the UBO stage's CONST[0] with the push-constant window. */
   if (pl->uses_push_constants && (pl->vs_has_ubo || pl->fs_has_ubo))
      FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                              "r3v: pipeline uses push constants in one stage "
                              "and a uniform buffer in another; the replay binds "
                              "the push-constant window to both stages' CONST[0] "
                              "and cannot also bind a per-stage UBO"));

   /* The stitch geometry occupies fragment CONST[0], so an app fragment UBO,
    * push constants, or a subpass input -- all of which also resolve to CONST[0]
    * -- cannot coexist with it.  Reject rather than overwrite the geometry. */
   if (pl->fs_nearest_stitch &&
       (pl->fs_has_ubo || pl->uses_push_constants || pl->fs_has_input_attachment))
      FAIL_PIPELINE(vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                              "r3v: experimental NEAREST tile-stitch needs "
                              "fragment CONST[0] for the per-image tile geometry, "
                              "so a fragment UBO, push constants, or a subpass "
                              "input cannot be combined with it"));

   VkResult cso_res = r3v_init_graphics_pipeline_cso_state(device, pl, info);
   if (cso_res != VK_SUCCESS)
      FAIL_PIPELINE(cso_res);

   if (info->pVertexInputState || pl->needs_vertex_id_stream ||
       pl->needs_instance_id_stream) {
      VkResult r = r3v_build_velems_cso(device, pl, info->pVertexInputState);
      if (r != VK_SUCCESS)
         FAIL_PIPELINE(r);
   }

#undef FAIL_PIPELINE

   pl->topology = info->pInputAssemblyState
                  ? info->pInputAssemblyState->topology
                  : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

   r3v_capture_dynamic_state(pl, info);

   *pPipeline = r3v_pipeline_to_handle(pl);
   return VK_SUCCESS;
}

VkResult
r3v_CreateGraphicsPipelines(VkDevice _device,
                                 VkPipelineCache pipelineCache,
                                 uint32_t createInfoCount,
                                 const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult r = r3v_create_one_pipeline(device, &pCreateInfos[i],
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
r3v_classify_compute_kernel(struct r3v_device *device,
                               const VkPipelineShaderStageCreateInfo *stage_info,
                               struct r300_compute_admission *adm,
                               struct r300_compute_identity_pattern *ident,
                               struct r300_compute_binary_map_pattern *binmap,
                               struct r300_compute_unary_map_pattern *unary,
                               struct r300_compute_unary_transcendental_pattern *transc,
                               struct r300_compute_binary_transcendental_pattern *btransc,
                               struct r300_compute_bitwise_logicop_pattern *bitwise,
                               struct r300_compute_shift_logical_pattern *shift,
                               struct r300_compute_shift_variable_pattern *shiftvar,
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
   VK_FROM_HANDLE(r3v_shader_module, mod, stage_info->module);
   if (!mod)
      return false;

   nir_shader *nir = vk_spirv_to_nir(&device->vk, mod->code, mod->code_size,
                                     MESA_SHADER_COMPUTE, stage_info->pName,
                                     stage_info->pSpecializationInfo,
                                     &r3v_spirv_opts,
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

   /* Detector-eye view of the kernel: R3V_DEBUG=classify_nir dumps the
    * exact NIR the classify + pattern detectors walk, the first thing to
    * read when a kernel that should match a raster verb dispatches as an
    * unknown-shape no-op. */
   if (device->dbg_classify_nir)
      nir_print_shader(nir, mesa_log_get_file());

   r300_nir_classify_compute(nir, adm);
   r300_nir_detect_identity_map(nir, ident);
   r300_nir_detect_binary_map(nir, binmap);
   r300_nir_detect_unary_map(nir, unary);
   r300_nir_detect_unary_transcendental(nir, transc);
   r300_nir_detect_binary_transcendental(nir, btransc);
   r300_nir_detect_bitwise_logicop(nir, bitwise);
   r300_nir_detect_shift_logical(nir, shift);
   r300_nir_detect_shift_variable(nir, shiftvar);
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
 * skip recreation.  The matching delete_*_state runs in r3v_DestroyDevice
 * before the pipe_context itself is destroyed. */
static bool
r3v_device_init_identity_map_state_locked(struct r3v_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;

   if (!device->identity_map_cso.blend) {
      struct pipe_blend_state blend = {0};
      blend.rt[0].colormask = PIPE_MASK_RGBA;
      device->identity_map_cso.blend =
         pipe->create_blend_state(pipe, &blend);
      if (!device->identity_map_cso.blend)
         return false;
   }

   if (!device->identity_map_cso.rasterizer) {
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
      device->identity_map_cso.rasterizer =
         pipe->create_rasterizer_state(pipe, &raster);
      if (!device->identity_map_cso.rasterizer)
         return false;
   }

   if (!device->identity_map_cso.dsa) {
      struct pipe_depth_stencil_alpha_state dsa = {0};
      /* All zero: depth test off, stencil off, alpha test off. */
      device->identity_map_cso.dsa =
         pipe->create_depth_stencil_alpha_state(pipe, &dsa);
      if (!device->identity_map_cso.dsa)
         return false;
   }

   if (!device->identity_map_cso.sampler) {
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
      device->identity_map_cso.sampler =
         pipe->create_sampler_state(pipe, &samp);
      if (!device->identity_map_cso.sampler)
         return false;
   }

   return true;
}

static bool
r3v_device_init_identity_map_state(struct r3v_device *device)
{
   simple_mtx_lock(&device->identity_map_cso_lock);
   bool ok = r3v_device_init_identity_map_state_locked(device);
   simple_mtx_unlock(&device->identity_map_cso_lock);
   return ok;
}

/* Create one 32x1 RGBA8 lookup texture whose texel j holds the little-endian
 * bytes of values[j], matching the RGBA8 byte order the carrier decodes (R =
 * byte 0 = LSB).  Used for both variable-shift lookups (2^j and the sign-fill
 * mask).  Returns the resource + a NEAREST-swizzle sampler view through out
 * params; the caller holds identity_map_cso_lock. */
static bool
r3v_create_shift_lut_locked(struct pipe_context *pipe,
                               struct pipe_screen *screen,
                               const uint32_t values[32],
                               struct pipe_resource **out_res,
                               struct pipe_sampler_view **out_view)
{
   uint8_t texels[32][4];
   for (unsigned j = 0; j < 32; j++) {
      texels[j][0] = (uint8_t)(values[j] & 0xFF);
      texels[j][1] = (uint8_t)((values[j] >> 8) & 0xFF);
      texels[j][2] = (uint8_t)((values[j] >> 16) & 0xFF);
      texels[j][3] = (uint8_t)((values[j] >> 24) & 0xFF);
   }

   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target     = PIPE_TEXTURE_2D;
   templ.format     = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0     = 32;
   templ.height0    = 1;
   templ.depth0     = 1;
   templ.array_size = 1;
   templ.usage      = PIPE_USAGE_DEFAULT;
   templ.bind       = PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *lut = screen->resource_create(screen, &templ);
   if (!lut)
      return false;

   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.width = 32; box.height = 1; box.depth = 1;
   pipe->texture_subdata(pipe, lut, 0, PIPE_MAP_WRITE, &box,
                         texels, sizeof(texels[0]) * 32, 0);

   struct pipe_sampler_view sv_templ;
   memset(&sv_templ, 0, sizeof(sv_templ));
   sv_templ.format            = PIPE_FORMAT_R8G8B8A8_UNORM;
   sv_templ.target            = PIPE_TEXTURE_2D;
   sv_templ.u.tex.first_level = 0;
   sv_templ.u.tex.last_level  = 0;
   sv_templ.swizzle_r = PIPE_SWIZZLE_X;
   sv_templ.swizzle_g = PIPE_SWIZZLE_Y;
   sv_templ.swizzle_b = PIPE_SWIZZLE_Z;
   sv_templ.swizzle_a = PIPE_SWIZZLE_W;
   struct pipe_sampler_view *view =
      pipe->create_sampler_view(pipe, lut, &sv_templ);
   if (!view) {
      pipe_resource_reference(&lut, NULL);
      return false;
   }

   *out_res  = lut;
   *out_view = view;
   return true;
}

/* Build the two variable-shift lookups the gather and sign-fill passes sample.
 * The 2^j lookup (texel j = 2^j) feeds the multiply; the fill lookup (texel b =
 * 0xFFFFFFFF << (32-b), the top b bits, b=0 -> 0) supplies the ishr sign
 * extension.  Device-global and read-only, created once; the caller holds
 * identity_map_cso_lock.  Freed in r3v_DestroyDevice. */
static bool
r3v_device_ensure_shift_variable_lut_locked(struct r3v_device *device)
{
   if (device->shift_variable_lut_view && device->shift_variable_fill_lut_view)
      return true;
   struct pipe_context *pipe   = device->pipe;
   struct pipe_screen  *screen = device->screen;
   if (!pipe || !screen)
      return false;

   uint32_t pow2[32], fill[32];
   for (unsigned j = 0; j < 32; j++) {
      pow2[j] = 1u << j;
      fill[j] = j == 0 ? 0u : (uint32_t)(0xFFFFFFFFu << (32 - j));
   }

   if (!device->shift_variable_lut_view &&
       !r3v_create_shift_lut_locked(pipe, screen, pow2,
                                       &device->shift_variable_lut,
                                       &device->shift_variable_lut_view))
      return false;
   if (!device->shift_variable_fill_lut_view &&
       !r3v_create_shift_lut_locked(pipe, screen, fill,
                                       &device->shift_variable_fill_lut,
                                       &device->shift_variable_fill_lut_view))
      return false;
   return true;
}

/* Shared nir_builder helpers for the synthesized replay shaders.  The
 * detected NIR opcode maps directly onto builder arithmetic.  The
 * admitted op set mirrors r300_compute_admission.c
 * binary_map_op_admitted().  TGSI ADD / SUB / MUL / MIN / MAX are float;
 * integer NIR opcodes fold into the same float ALU because the texture
 * sampling normalises UNORM8 bytes to [0,1] floats anyway -- the byte
 * round-trip stays bit-exact when the operator obeys the FP24 integer-exact
 * envelope. */
/* Shared nir_builder plumbing for the synthesized replay shaders.  The
 * interpolant is VARYING_SLOT_TEX0 on both the VS output and the FS input:
 * nir_to_rc maps TEX0 to generic0 while VARYING_SLOT_VAR0 shifts to generic9
 * during varying-slot fixup, so TEX0 is the slot that pairs across the
 * synthesized pipeline (the same law r3v_dp4_fs_nir.c records). */
static nir_def *
r3v_synth_load_varying(nir_builder *b)
{
   nir_variable *in_tc = nir_variable_create(b->shader, nir_var_shader_in,
                                             glsl_vec4_type(), "tc");
   in_tc->data.location = VARYING_SLOT_TEX0;
   return nir_load_var(b, in_tc);
}

static nir_def *
r3v_synth_load_texcoord(nir_builder *b)
{
   return nir_trim_vector(b, r3v_synth_load_varying(b), 2);
}

/* Push-window constant read: the dispatch replay binds the 128-byte push
 * window at FS CONST[0]; a load at byte offset N in UBO 0 is the NIR
 * spelling of CONST[N/16] channel (N%16)/4.  Declare the sized block-0
 * UBO so ntr_setup_uniforms sizes the constant file from the interface
 * type; load_ubo alone does not carry that size. */
static nir_def *
r3v_synth_push_load(nir_builder *b, unsigned num_components,
                    unsigned byte_offset)
{
   r3v_declare_block0_ubo(b->shader, 128);
   return nir_load_ubo(b, num_components, 32, nir_imm_int(b, 0),
                       nir_imm_int(b, byte_offset),
                       .align_mul = 4, .range_base = 0, .range = 128);
}

static nir_def *
r3v_synth_sample2d(nir_builder *b, unsigned binding, nir_def *coord)
{
   /* Fixed sampler name: binding is recorded on the variable; unique
    * string names are not required for single-sampler synth shaders. */
   nir_variable *samp = nir_variable_create(
      b->shader, nir_var_uniform,
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
      "samp");
   samp->data.binding = binding;
   nir_deref_instr *d = nir_build_deref_var(b, samp);
   return nir_tex(b, coord, .texture_deref = d, .sampler_deref = d);
}

static void
r3v_synth_store_color(nir_builder *b, nir_def *value)
{
   nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                           glsl_vec4_type(), "color");
   out->data.location = FRAG_RESULT_COLOR;
   nir_store_var(b, out, value, 0xf);
}

static void *
r3v_synth_fs_cso(struct pipe_context *pipe, nir_builder *b)
{
   nir_shader_gather_info(b->shader, nir_shader_get_entrypoint(b->shader));
   nir_assign_io_var_locations(b->shader, nir_var_shader_in);
   nir_assign_io_var_locations(b->shader, nir_var_shader_out);

   nir_shader *fs_nir = b->shader;
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, fs_nir, true);

   struct pipe_shader_state state = { .type   = PIPE_SHADER_IR_NIR,
                                      .ir.nir = fs_nir };
   void *fs_cso = pipe->create_fs_state(pipe, &state);
   if (!fs_cso)
      ralloc_free(fs_nir);
   return fs_cso;
}

/* Synthesise the 2-sampler fragment program for the binary-map lowering:
 * sample in_a and in_b at the fullscreen texcoord, apply the binary ALU op,
 * and write the result to the color export.  Integer NIR opcodes fold into
 * the same float ALU because the texture sampling normalises UNORM8 bytes
 * to [0,1] floats anyway -- the byte round-trip stays bit-exact when the
 * operator obeys the FP24 integer-exact envelope.
 *
 * Costs: 2 TEX + 2 ALU = 4/96 of the R300 PFS budget
 * (R300_PFS_MAX_ALU_INST=64 / R300_PFS_MAX_TEX_INST=32). */
static void *
r3v_synthesize_binary_map_fs(struct pipe_context *pipe, uint16_t alu_op)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_binary_map");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *va = r3v_synth_sample2d(&b, 0, coord);
   nir_def *vb = r3v_synth_sample2d(&b, 1, coord);

   nir_def *result;
   switch (alu_op) {
   case nir_op_fadd: case nir_op_iadd:
      result = nir_fadd(&b, va, vb); break;
   case nir_op_fsub: case nir_op_isub:
      result = nir_fsub(&b, va, vb); break;
   case nir_op_fmul: case nir_op_imul:
      result = nir_fmul(&b, va, vb); break;
   case nir_op_fmin: case nir_op_imin: case nir_op_umin:
      result = nir_fmin(&b, va, vb); break;
   case nir_op_fmax: case nir_op_imax: case nir_op_umax:
      result = nir_fmax(&b, va, vb); break;
   default:
      ralloc_free(b.shader);
      return NULL;
   }

   r3v_synth_store_color(&b, result);
   return r3v_synth_fs_cso(pipe, &b);
}

/* Synthesise the binary-map VS + FS pair on the pipeline.  Reuses the
 * device-cached state CSOs (blend / raster / dsa / sampler) the identity-map
 * synthesis populates -- the binary-map and identity-map paths share every
 * per-draw state object; only the FS differs. */
/* Fullscreen-quad vertex shader synthesis: 2 attributes (position + texcoord).
 * Identity-map coordinate interpolation and per-vertex reduction values use
 * this passthrough shape.  Cached on the pipeline object; the existing
 * destroy path frees it.  Pure NIR: attribute 0 passes through to
 * VARYING_SLOT_POS, attribute 1 to VARYING_SLOT_TEX0 -- the slot every
 * synthesized replay FS names its interpolant (nir_to_rc maps TEX0 to
 * generic0, the pairing r3v_dp4_fs_nir.c records). */
static void *
r3v_synthesize_passthrough_vs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_VERTEX];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts,
                                                  "r3v_passthrough_vs");

   nir_variable *in_pos = nir_variable_create(b.shader, nir_var_shader_in,
                                              glsl_vec4_type(), "in_pos");
   in_pos->data.location = VERT_ATTRIB_GENERIC0;
   nir_variable *in_tc = nir_variable_create(b.shader, nir_var_shader_in,
                                             glsl_vec4_type(), "in_tc");
   in_tc->data.location = VERT_ATTRIB_GENERIC0 + 1;

   nir_variable *out_pos = nir_variable_create(b.shader, nir_var_shader_out,
                                               glsl_vec4_type(), "pos");
   out_pos->data.location = VARYING_SLOT_POS;
   nir_variable *out_tc = nir_variable_create(b.shader, nir_var_shader_out,
                                              glsl_vec4_type(), "tc");
   out_tc->data.location = VARYING_SLOT_TEX0;

   nir_store_var(&b, out_pos, nir_load_var(&b, in_pos), 0xf);
   nir_store_var(&b, out_tc, nir_load_var(&b, in_tc), 0xf);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   nir_assign_io_var_locations(b.shader, nir_var_shader_in);
   nir_assign_io_var_locations(b.shader, nir_var_shader_out);

   nir_shader *vs_nir = b.shader;
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, vs_nir, false);

   struct pipe_shader_state state = { .type   = PIPE_SHADER_IR_NIR,
                                      .ir.nir = vs_nir };
   void *vs_cso = pipe->create_vs_state(pipe, &state);
   if (!vs_cso)
      ralloc_free(vs_nir);
   return vs_cso;
}

static bool
r3v_binary_map_synthesize_shaders(struct r3v_device *device,
                                      struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_binary_map_fs(pipe, pl->binary_map.alu_op);
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
static nir_def *
r3v_unary_map_const(nir_builder *b, bool from_push,
                    uint16_t push_offset, float literal)
{
   nir_def *scalar;
   if (!from_push) {
      scalar = nir_imm_float(b, literal);
   } else {
      /* The dispatch replay binds the 128-byte push window at FS CONST[0];
       * push byte offset N is a scalar load at that offset in UBO 0. */
      scalar = nir_load_ubo(b, 1, 32, nir_imm_int(b, 0),
                            nir_imm_int(b, push_offset),
                            .align_mul = 4, .range_base = 0, .range = 128);
   }
   return nir_swizzle(b, scalar, (unsigned[]){0, 0, 0, 0}, 4);
}

static void *
r3v_synthesize_unary_map_fs(struct pipe_context *pipe,
                               const struct r300_compute_unary_map_pattern *um)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_unary_map");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *value = r3v_synth_sample2d(&b, 0, coord);
   nir_def *c0 = r3v_unary_map_const(
      &b, um->mul_const_from_push, um->mul_const_push_offset, um->mul_const);
   nir_def *c1 = r3v_unary_map_const(
      &b, um->add_const_from_push, um->add_const_push_offset, um->add_const);

   /* MUL then ADD (not one fused MAD) preserves the exact operation order
    * the FP24 integer-exact envelope was validated against. */
   r3v_synth_store_color(&b, nir_fadd(&b, nir_fmul(&b, value, c0), c1));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Synthesize the unary-map VS + FS.  The dispatch replay uses the unary-map
 * metadata directly and binds the affine fragment program, so the same
 * fullscreen draw computes out = tex*c0 + c1 instead of a copy. */
static bool
r3v_unary_map_synthesize_shaders(struct r3v_device *device,
                                    struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_unary_map_fs(pipe, &pl->unary_map);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }

   return true;
}

/* Synthesize the DP4 fragment-program CSO.  The NIR itself is built by
 * r3v_build_dp4_fs_nir (r3v_dp4_fs_nir.c) so a build-time test can
 * validate the shader shape -- notably the 2-component 2D-sampler coordinate --
 * without a pipe_context; here we only finalize for the screen and create the
 * gallium state. */
static void *
r3v_synthesize_dp4_fs(struct pipe_context *pipe, uint8_t components)
{
   nir_shader *fs_nir = r3v_build_dp4_fs_nir(
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
r3v_dp4_synthesize_shaders(struct r3v_device *device,
                              struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_dp4_fs(pipe, pl->dp4.components);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Unary-transcendental FS: 1 TEX + one native US scalar transcendental,
 * parameterized by the detector's recorded nir_op.  Pure NIR paired with the
 * NIR passthrough VS: both sides name the interpolant VARYING_SLOT_TEX0, so
 * nir_to_rc maps VS output and FS input to the same generic0 -- the
 * TEX0-vs-GENERIC mismatch that broke the earlier mixed NIR-FS/TGSI-VS
 * pairing in the scalar carrier cannot recur when one convention feeds both
 * stages.  fsqrt lowers through the r300 NIR pipeline (the US ALU has no
 * SQRT).  Returns NULL for an op outside the admitted set. */
static void *
r3v_synthesize_unary_transcendental_fs(struct pipe_context *pipe,
                                          uint16_t alu_op)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   /* Deliberately NOT r3v_-prefixed: the virtual-FP gate in r300_nir.c
    * overloads fsin/fpow/fldexp as 4-component placeholders in shaders
    * carrying the prefix, and this program computes the REAL
    * transcendental -- a prefixed name would hijack it. */
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "replay_unary_transc");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *value = r3v_synth_sample2d(&b, 0, coord);

   nir_def *result;
   switch ((nir_op)alu_op) {
   case nir_op_fsqrt:       result = nir_fsqrt(&b, value); break;
   case nir_op_frsq:        result = nir_frsq(&b, value); break;
   case nir_op_frcp:        result = nir_frcp(&b, value); break;
   case nir_op_fexp2:       result = nir_fexp2(&b, value); break;
   case nir_op_flog2:       result = nir_flog2(&b, value); break;
   case nir_op_fsin:        result = nir_fsin(&b, value); break;
   case nir_op_fcos:        result = nir_fcos(&b, value); break;
   case nir_op_ffract:      result = nir_ffract(&b, value); break;
   case nir_op_ffloor:      result = nir_ffloor(&b, value); break;
   case nir_op_fround_even: result = nir_fround_even(&b, value); break;
   default:
      ralloc_free(b.shader);
      return NULL;
   }

   r3v_synth_store_color(&b, result);
   return r3v_synth_fs_cso(pipe, &b);
}

/* Unary-transcendental VS+FS synthesis: the passthrough VS plus the
 * transcendental FS.  Reuses the device-cached identity-map state CSOs and the
 * scalar 1-in/1-out replay core. */
static bool
r3v_unary_transcendental_synthesize_shaders(struct r3v_device *device,
                                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_unary_transcendental_fs(
      pipe, pl->unary_transcendental.alu_op);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Binary-transcendental FS: 2 TEX + a componentwise non-commutative binary,
 * built like the binary_map FS (TEX0 interpolant, two samplers).
 * POW and RCP are scalar r300 ALU ops, so each lane is computed separately:
 * out.c = pow(a.c, b.c) for fpow, out.c = a.c * rcp(b.c) for fdiv.  A scalar
 * kernel (components == 1) computes lane 0 and broadcasts it -- the scalar
 * carrier's X-lane gather reads channel 0.  Returns NULL for an op outside
 * {fpow, fdiv}. */
static void *
r3v_synthesize_binary_transcendental_fs(struct pipe_context *pipe,
                                           uint16_t alu_op, unsigned components)
{
   const bool is_pow = (nir_op)alu_op == nir_op_fpow;
   const bool is_div = (nir_op)alu_op == nir_op_fdiv;
   if (!is_pow && !is_div)
      return NULL;

   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   /* Deliberately NOT r3v_-prefixed: fpow is one of the opcodes the
    * virtual-FP gate overloads as a placeholder in prefixed shaders, and
    * this program computes the real power/divide. */
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "replay_binary_transc");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *va = r3v_synth_sample2d(&b, 0, coord);
   nir_def *vb = r3v_synth_sample2d(&b, 1, coord);

   nir_def *result;
   if (components == 1) {
      /* Scalar: compute lane 0 and broadcast across the export -- the
       * scalar carrier's X-lane gather reads channel 0. */
      nir_def *a0 = nir_channel(&b, va, 0);
      nir_def *b0 = nir_channel(&b, vb, 0);
      nir_def *r0 = is_div ? nir_fmul(&b, a0, nir_frcp(&b, b0))
                           : nir_fpow(&b, a0, b0);
      result = nir_swizzle(&b, r0, (unsigned[]){0, 0, 0, 0}, 4);
   } else if (is_div) {
      result = nir_fmul(&b, va, nir_frcp(&b, vb));
   } else {
      result = nir_fpow(&b, va, vb);
   }

   r3v_synth_store_color(&b, result);
   return r3v_synth_fs_cso(pipe, &b);
}

/* Binary-transcendental VS+FS synthesis: the passthrough VS plus the two-input
 * transcendental FS.  Reuses the device-cached identity-map state CSOs and the
 * two-in/one-out replay core the binary_map float path uses. */
static bool
r3v_binary_transcendental_synthesize_shaders(struct r3v_device *device,
                                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_binary_transcendental_fs(
      pipe, pl->binary_transcendental.alu_op,
      pl->binary_transcendental.value_components);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* Logical-shift FS: out[gid] = a << k (ishl) or a >> k (ushr).  The uint32 packs
 * as RGBA8 with R = byte 0 (LSB) ... A = byte 3 (MSB).  Write k = 8*q + r.  The
 * four bytes are recovered exactly with round(c*255) (the UNORM8 value), each
 * output lane is gathered from its source byte(s) with the byte distance q as a
 * channel permutation, and the within-byte bit move r combines the low 8-r bits
 * of one byte with the high r bits of its neighbor.  Every intermediate is a
 * byte times a power of two below 2^17, an exact FP24 integer, so the result is
 * bit-exact.  The carry term vanishes for r = 0 (pure byte shift). */
static void *
r3v_synthesize_shift_logical_fs(struct pipe_context *pipe, bool is_left,
                                   bool is_arithmetic, unsigned shift_amount)
{
   /* The detector admits only k in [1,31]; the power-of-two and sign-fill
    * constants below shift by 8-r and 32-k, which are defined exactly in that
    * range.  Assert the contract locally so a future miscall traps instead of
    * reaching a >= width shift (C11 6.5.7p3 undefined behaviour). */
   assert(shift_amount >= 1 && shift_amount <= 31);
   const unsigned q = shift_amount / 8;
   const unsigned r = shift_amount % 8;
   const float pow2_r   = (float)(1u << r);
   const float pow2_8mr = (float)(1u << (8 - r));

   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_shift_logical");
   nir_def *tc = r3v_synth_load_texcoord(&b);
   /* B = floor(tex * 255 + 0.5): the four byte values, 0..255.  Match the
    * UNORM snap used by the other replay shaders so plane-equation
    * rounding just below an integer still lands on the intended byte. */
   nir_def *B = nir_ffloor(
      &b, nir_fadd_imm(&b,
                       nir_fmul_imm(&b, r3v_synth_sample2d(&b, 0, tc), 255.0),
                       0.5));

   /* Per output lane, the main source byte is q away (toward the LSB for a
    * left shift, toward the MSB for a right shift) and the carry byte is one
    * further; a lane outside [0,3] is a zero fill. */
   nir_def *zero = nir_imm_float(&b, 0.0);
   nir_def *mainl[4], *carryl[4];
   for (int j = 0; j < 4; j++) {
      const int step = is_left ? -(int)q : (int)q;
      const int carry_step = is_left ? -1 : 1;
      const int mc = j + step;
      const int cc = j + step + carry_step;
      mainl[j]  = (mc >= 0 && mc <= 3) ? nir_channel(&b, B, mc) : zero;
      carryl[j] = (cc >= 0 && cc <= 3) ? nir_channel(&b, B, cc) : zero;
   }
   nir_def *Bmain = nir_vec4(&b, mainl[0], mainl[1], mainl[2], mainl[3]);
   nir_def *Bcar  = nir_vec4(&b, carryl[0], carryl[1], carryl[2], carryl[3]);

   nir_def *term1, *term2;
   if (is_left) {
      /* term1 = (Bmain << r) mod 256 = Bmain*2^r - 256*floor(Bmain*2^r/256);
       * term2 = Bcar >> (8-r) = floor(Bcar / 2^(8-r)). */
      nir_def *t = nir_fmul_imm(&b, Bmain, pow2_r);
      nir_def *f = nir_ffloor(&b, nir_fmul_imm(&b, t, 1.0 / 256.0));
      term1 = nir_fadd(&b, nir_fmul_imm(&b, f, -256.0), t);
      term2 = nir_ffloor(&b, nir_fmul_imm(&b, Bcar, 1.0 / pow2_8mr));
   } else {
      /* term1 = Bmain >> r; term2 = (Bcar << (8-r)) mod 256. */
      term1 = nir_ffloor(&b, nir_fmul_imm(&b, Bmain, 1.0 / pow2_r));
      nir_def *t = nir_fmul_imm(&b, Bcar, pow2_8mr);
      nir_def *f = nir_ffloor(&b, nir_fmul_imm(&b, t, 1.0 / 256.0));
      term2 = nir_fadd(&b, nir_fmul_imm(&b, f, -256.0), t);
   }
   nir_def *result = nir_fadd(&b, term1, term2);

   /* Arithmetic right shift fills the top k bits -- the ones ushr zeroed --
    * with the sign bit (bit 31 = high bit of byte 3).  Those bits are
    * disjoint from the logical result, so adding sign * fill recovers ishr
    * exactly.  fill is the byte decomposition of (0xFFFFFFFF << (32-k)),
    * baked per amount. */
   if (is_arithmetic) {
      const uint32_t fill = 0xFFFFFFFFu << (32 - shift_amount);
      nir_def *sign = nir_ffloor(
         &b, nir_fmul_imm(&b, nir_channel(&b, B, 3), 1.0 / 128.0));
      nir_def *fillb = nir_imm_vec4(
         &b, (float)(fill & 0xFF), (float)((fill >> 8) & 0xFF),
         (float)((fill >> 16) & 0xFF), (float)((fill >> 24) & 0xFF));
      result = nir_fadd(
         &b,
         nir_fmul(&b, nir_swizzle(&b, sign, (unsigned[]){0, 0, 0, 0}, 4),
                  fillb),
         result);
   }

   /* Pack back to UNORM8 as out_byte / 255. */
   r3v_synth_store_color(&b, nir_fmul_imm(&b, result, 1.0 / 255.0));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Logical-shift VS+FS synthesis: the passthrough VS plus the byte-recombination
 * FS.  Reuses the device-cached identity-map state CSOs and the scalar 1-in/1-out
 * replay core (UNORM8 in/out). */
static bool
r3v_shift_logical_synthesize_shaders(struct r3v_device *device,
                                        struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_shift_logical_fs(
      pipe, pl->shift_logical.is_left, pl->shift_logical.is_arithmetic,
      pl->shift_logical.shift_amount);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QMUL FS: the Hamilton-product fragment program built by r3v_build_qmul_fs_nir
 * (r3v_dp4_fs_nir.c) -- four sign-permuted DP4s writing the four-lane product
 * to the FP16 color export.  Finalize for the screen and create the gallium
 * state, as the DP4 FS does. */
static void *
r3v_synthesize_qmul_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qmul_fs_nir(
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
r3v_qmul_synthesize_shaders(struct r3v_device *device,
                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qmul_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QDIV FS: the quaternion-division fragment program built by r3v_build_qdiv_fs_nir
 * -- one Hamilton product over the scaled conjugate of the divisor, four DP4s plus
 * the US RCP, to the FP16 color export.  Same finalize+CSO as QMUL. */
static void *
r3v_synthesize_qdiv_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qdiv_fs_nir(
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
r3v_qdiv_synthesize_shaders(struct r3v_device *device,
                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qdiv_fs(pipe);
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
multilimb_extract_limbs(nir_builder *b, nir_def *bytes, nir_def *limb[5])
{
   nir_def *floors = nir_ffloor(
      b, nir_fmul(b, bytes,
                  nir_imm_vec4(b, 1.0f / 128.0f, 1.0f / 64.0f, 1.0f / 32.0f,
                               1.0f / 16.0f)));
   nir_def *B[4], *f[4];
   for (unsigned c = 0; c < 4; c++) {
      B[c] = nir_channel(b, bytes, c);
      f[c] = nir_channel(b, floors, c);
   }
   limb[0] = nir_fadd(b, nir_fmul_imm(b, f[0], -128.0), B[0]);
   limb[1] = nir_fadd(b, nir_fmul_imm(b, f[1], -128.0),
                      nir_fadd(b, f[0], nir_fmul_imm(b, B[1], 2.0)));
   limb[2] = nir_fadd(b, nir_fmul_imm(b, f[2], -128.0),
                      nir_fadd(b, f[1], nir_fmul_imm(b, B[2], 4.0)));
   limb[3] = nir_fadd(b, nir_fmul_imm(b, f[3], -128.0),
                      nir_fadd(b, f[2], nir_fmul_imm(b, B[3], 8.0)));
   limb[4] = f[3];
}

static void *
r3v_synthesize_multilimb_fs(struct pipe_context *pipe, unsigned column)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_multilimb");
   nir_def *tc = r3v_synth_load_texcoord(&b);

   #define SNAP(v) nir_ffloor(&b, nir_fadd_imm(&b, nir_fmul_imm(&b, (v), 255.0), 0.5))
   nir_def *bytes_a = SNAP(r3v_synth_sample2d(&b, 0, tc));
   nir_def *bytes_b = SNAP(r3v_synth_sample2d(&b, 1, tc));
   #undef SNAP

   nir_def *la[5], *lb[5];
   multilimb_extract_limbs(&b, bytes_a, la);
   multilimb_extract_limbs(&b, bytes_b, lb);

   /* c = sum over the column's limb pairs. */
   nir_def *c = NULL;
   for (unsigned i = 0; i < 5; i++) {
      if (column < i || column - i > 4)
         continue;
      nir_def *term = nir_fmul(&b, la[i], lb[column - i]);
      c = c ? nir_fadd(&b, c, term) : term;
   }

   /* Byte-decompose c <= 80645 < 2^17 little-endian into the RGBA8 export. */
   nir_def *vy = nir_ffloor(&b, nir_fmul_imm(&b, c, 1.0 / 256.0));
   nir_def *vz = nir_ffloor(&b, nir_fmul_imm(&b, c, 1.0 / 65536.0));
   nir_def *e0 = nir_fadd(&b, nir_fmul_imm(&b, vy, -256.0), c);
   nir_def *e1 = nir_fadd(&b, nir_fmul_imm(&b, vz, -256.0), vy);
   r3v_synth_store_color(
      &b, nir_vec4(&b, nir_fmul_imm(&b, e0, 1.0 / 255.0),
                   nir_fmul_imm(&b, e1, 1.0 / 255.0),
                   nir_fmul_imm(&b, vz, 1.0 / 255.0),
                   nir_imm_float(&b, 0.0)));
   return r3v_synth_fs_cso(pipe, &b);
}

/* MULTILIMB VS+FS synthesis: shared passthrough VS + one specialized column
 * program per convolution column.  Any column failing to compile clears the
 * pattern so the kernel falls to the binary-map or no-op lifecycle. */
static bool
r3v_multilimb_synthesize_shaders(struct r3v_device *device,
                                    struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   for (unsigned k = 0; k < 9; k++) {
      pl->multilimb_fs[k] = r3v_synthesize_multilimb_fs(pipe, k);
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

/* Variable-shift gather FS: turn a per-element shift amount into the 2^M
 * multiplier the convolution needs.  Sample b through sampler 0 (RGBA8, the
 * amount in byte 0), recover the integer amount with the exact UNORM8 round-trip
 * round(b.x * 255), form the lookup index (b for left, 31-b for right), then do
 * a NEAREST dependent read of the 2^j texture through sampler 1 at
 * (index + 0.5)/32.  The sampled texel IS 2^M as RGBA8, copied to the color
 * export; the transient it renders becomes the convolution's second operand. */
static void *
r3v_synthesize_shift_variable_gather_fs(struct pipe_context *pipe,
                                           bool is_left)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_shift_var_gather");
   nir_def *tc = r3v_synth_load_texcoord(&b);

   /* idx = round(b.x * 255): the exact integer amount in byte 0. */
   nir_def *byte0 = nir_channel(&b, r3v_synth_sample2d(&b, 0, tc), 0);
   nir_def *idx = nir_ffloor(
      &b, nir_fadd_imm(&b, nir_fmul_imm(&b, byte0, 255.0), 0.5));
   if (!is_left)
      idx = nir_fadd(&b, nir_imm_float(&b, 31.0), nir_fneg(&b, idx));

   /* coord.x = (idx + 0.5)/32 lands NEAREST on texel idx; coord.y = 0.5
    * picks the single row of the 32x1 lookup. */
   nir_def *coord = nir_vec2(
      &b,
      nir_fadd_imm(&b, nir_fmul_imm(&b, idx, 1.0 / 32.0), 0.5 / 32.0),
      nir_imm_float(&b, 0.5));
   r3v_synth_store_color(&b, r3v_synth_sample2d(&b, 1, coord));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Variable-shift sign-extension fill FS for ishr.  After the gather + convolution
 * have produced the logical ushr result, add sign(a) * fill[b].  Sampler 0 is the
 * logical ushr bytes, sampler 1 the original a (sign = bit 31 of byte 3), sampler
 * 2 the amount b (the fill index), sampler 3 the fill lookup.  out_byte = ushr +
 * sign * fill_byte; ushr occupies bits [0,31-b] and the fill bits [32-b,31], so
 * the per-byte sum is disjoint, never carries, and never exceeds 255. */
static void *
r3v_synthesize_shift_variable_signfill_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_shift_var_signfill");
   nir_def *tc = r3v_synth_load_texcoord(&b);

   /* Byte-snap helper shape: round(v * 255) as floor(v*255 + 0.5). */
   #define SNAP(v) nir_ffloor(&b, nir_fadd_imm(&b, nir_fmul_imm(&b, (v), 255.0), 0.5))
   nir_def *ushr = SNAP(r3v_synth_sample2d(&b, 0, tc));
   nir_def *aval = r3v_synth_sample2d(&b, 1, tc);
   /* sign = floor(round(a.w * 255) / 128): 1 for a negative operand. */
   nir_def *sign = nir_ffloor(
      &b, nir_fmul_imm(&b, SNAP(nir_channel(&b, aval, 3)), 1.0 / 128.0));
   nir_def *idx = SNAP(nir_channel(&b, r3v_synth_sample2d(&b, 2, tc), 0));
   /* LUT coord: NEAREST texel idx of the 32x1 fill table. */
   nir_def *coord = nir_vec2(
      &b,
      nir_fadd_imm(&b, nir_fmul_imm(&b, idx, 1.0 / 32.0), 0.5 / 32.0),
      nir_imm_float(&b, 0.5));
   nir_def *fill = SNAP(r3v_synth_sample2d(&b, 3, coord));
   #undef SNAP
   /* ushr + sign * fill, back to UNORM8. */
   nir_def *merged = nir_fadd(
      &b, nir_fmul(&b, nir_swizzle(&b, sign, (unsigned[]){0, 0, 0, 0}, 4),
                   fill),
      ushr);
   r3v_synth_store_color(&b, nir_fmul_imm(&b, merged, 1.0 / 255.0));
   return r3v_synth_fs_cso(pipe, &b);
}

static bool
r3v_shift_variable_synthesize_shaders(struct r3v_device *device,
                                         struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   simple_mtx_lock(&device->identity_map_cso_lock);
   bool lut_ok = r3v_device_ensure_shift_variable_lut_locked(device);
   simple_mtx_unlock(&device->identity_map_cso_lock);
   if (!lut_ok)
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->shift_variable_gather_fs = r3v_synthesize_shift_variable_gather_fs(
      pipe, pl->shift_variable.is_left);
   if (!pl->shift_variable_gather_fs) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }

   for (unsigned k = 0; k < 9; k++) {
      pl->multilimb_fs[k] = r3v_synthesize_multilimb_fs(pipe, k);
      if (!pl->multilimb_fs[k]) {
         for (unsigned j = 0; j < k; j++) {
            pipe->delete_fs_state(pipe, pl->multilimb_fs[j]);
            pl->multilimb_fs[j] = NULL;
         }
         pipe->delete_fs_state(pipe, pl->shift_variable_gather_fs);
         pl->shift_variable_gather_fs = NULL;
         pipe->delete_vs_state(pipe, pl->vs_cso);
         pl->vs_cso = NULL;
         return false;
      }
   }

   /* ishr needs a third pass that adds the sign-extension fill onto the logical
    * ushr result; ishl/ushr leave it NULL. */
   if (pl->shift_variable.is_arithmetic) {
      pl->shift_variable_signfill_fs =
         r3v_synthesize_shift_variable_signfill_fs(pipe);
      if (!pl->shift_variable_signfill_fs) {
         for (unsigned k = 0; k < 9; k++) {
            pipe->delete_fs_state(pipe, pl->multilimb_fs[k]);
            pl->multilimb_fs[k] = NULL;
         }
         pipe->delete_fs_state(pipe, pl->shift_variable_gather_fs);
         pl->shift_variable_gather_fs = NULL;
         pipe->delete_vs_state(pipe, pl->vs_cso);
         pl->vs_cso = NULL;
         return false;
      }
   }

   /* The dispatch validation prologue requires a non-NULL fs_cso; the gather and
    * column draws bind their own shaders explicitly. */
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
r3v_synthesize_log4_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_log4");
   nir_def *tc = r3v_synth_load_texcoord(&b);
   /* CONST[0] = (W/2, H/2, 1/W, 1/H) in the push window. */
   nir_def *cst = r3v_synth_push_load(&b, 4, 0);

   /* cell = floor(tc.xy * (W2, H2)): the snapped integer output cell;
    * coord = (2*cell + 1) * (1/W, 1/H): the exact 2x2 corner. */
   nir_def *cell = nir_ffloor(
      &b, nir_fmul(&b, tc, nir_trim_vector(&b, cst, 2)));
   nir_def *coord = nir_fmul(
      &b, nir_fadd_imm(&b, nir_fmul_imm(&b, cell, 2.0), 1.0),
      nir_channels(&b, cst, 0x3 << 2));
   nir_def *t = r3v_synth_sample2d(&b, 0, coord);
   r3v_synth_store_color(
      &b, nir_vec4(&b, nir_channel(&b, t, 0), nir_imm_float(&b, 0.0),
                   nir_imm_float(&b, 0.0), nir_imm_float(&b, 0.0)));
   return r3v_synth_fs_cso(pipe, &b);
}

static bool
r3v_log4_synthesize_shaders(struct r3v_device *device,
                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r3v_synthesize_log4_fs(pipe);
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
r3v_synthesize_cas_fs(struct pipe_context *pipe, uint32_t expect,
                         uint32_t value_new)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_cas");
   nir_def *tc = r3v_synth_load_texcoord(&b);

   nir_def *expb = nir_imm_vec4(
      &b, (float)(expect & 0xFF), (float)((expect >> 8) & 0xFF),
      (float)((expect >> 16) & 0xFF), (float)((expect >> 24) & 0xFF));
   nir_def *newb = nir_imm_vec4(
      &b, (float)(value_new & 0xFF), (float)((value_new >> 8) & 0xFF),
      (float)((value_new >> 16) & 0xFF), (float)((value_new >> 24) & 0xFF));

   nir_def *g = nir_ffloor(
      &b, nir_fadd_imm(
             &b, nir_fmul_imm(&b, r3v_synth_sample2d(&b, 0, tc), 255.0),
             0.5));

   /* SEQ per byte, then AND the four lanes as a float product: t is 1.0
    * only when all four decoded bytes equal the expected word. */
   nir_def *eq = nir_b2f32(&b, nir_feq(&b, g, expb));
   nir_def *t = nir_fmul(
      &b, nir_fmul(&b, nir_channel(&b, eq, 0), nir_channel(&b, eq, 1)),
      nir_fmul(&b, nir_channel(&b, eq, 2), nir_channel(&b, eq, 3)));

   /* d = g + t*(new - g): the compare-and-swap select, kept as the
    * MUL-into-MAD shape the FP24 envelope was validated against. */
   nir_def *d = nir_fadd(
      &b,
      nir_fmul(&b, nir_fadd(&b, newb, nir_fneg(&b, g)),
               nir_swizzle(&b, t, (unsigned[]){0, 0, 0, 0}, 4)),
      g);
   r3v_synth_store_color(&b, nir_fmul_imm(&b, d, 1.0 / 255.0));
   return r3v_synth_fs_cso(pipe, &b);
}

static bool
r3v_cas_synthesize_shaders(struct r3v_device *device,
                              struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r3v_synthesize_cas_fs(pipe, pl->cas.expect,
                                         pl->cas.value_new);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* AFFINE_IOTA FS: out[gid] = stride * gid + offset, the index materialized
 * in the FP24 fragment ALU.  The RS482 affine_iota_index probe validates the
 * texel-unit varying path at (x + 0.5, y + 0.5) fragment centers; the
 * dispatch-known scalars ride the fragment constant file as CONST[0] =
 * (width, stride, offset, unused).
 * gid = floor(tc.y) * width + floor(tc.x); v = gid * stride + offset; the
 * integer result is byte-decomposed little-endian into the RGBA8 export:
 * r = v mod 256, g = floor(v/256) mod 256, b = floor(v/65536).  Every
 * intermediate is an exact FP24 integer while v <= 2^17 (the dispatch gate
 * bounds stride * (total - 1) + offset by exactly that), and the high
 * byte b = floor(v/65536) <= 2 needs no mod.  Divisions are by powers of
 * two -- exponent shifts, exact -- and floor is the FLR opcode. */
static void *
r3v_synthesize_affine_iota_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_affine_iota");
   nir_def *tc = r3v_synth_load_texcoord(&b);
   /* CONST[0] = (width, stride, offset) in the push window (3 components). */
   nir_def *cst = r3v_synth_push_load(&b, 3, 0);

   /* floor(tc.xy): SNAP the interpolated texel-center varying back to
    * the integer coordinate.  The rasterizer interpolant carries x + 0.5
    * plus a sub-texel plane-equation error (the RS482 probe measured cliff
    * flips at byte boundaries when the raw value fed the decompose), and
    * floor absorbs any error below half a texel, so every downstream
    * operand is an exact FP24 integer.  Snapping per axis matters: the
    * combined linear index exceeds 2^16 where a +0.5 round-bias would no
    * longer be representable, but per-axis coordinates stay <= 2048. */
   nir_def *cell = nir_ffloor(&b, tc);
   /* gid = cell.y * width + cell.x; v = gid * stride + offset. */
   nir_def *gid = nir_fadd(
      &b, nir_fmul(&b, nir_channel(&b, cell, 1), nir_channel(&b, cst, 0)),
      nir_channel(&b, cell, 0));
   nir_def *v = nir_fadd(
      &b, nir_fmul(&b, gid, nir_channel(&b, cst, 1)),
      nir_channel(&b, cst, 2));
   /* Byte decomposition: vy = floor(v/256), vz = floor(v/65536);
    * e = (v - 256*vy, vy - 256*vz, vz). */
   nir_def *vy = nir_ffloor(&b, nir_fmul_imm(&b, v, 1.0 / 256.0));
   nir_def *vz = nir_ffloor(&b, nir_fmul_imm(&b, v, 1.0 / 65536.0));
   nir_def *e0 = nir_fadd(&b, nir_fmul_imm(&b, vy, -256.0), v);
   nir_def *e1 = nir_fadd(&b, nir_fmul_imm(&b, vz, -256.0), vy);
   /* out = (e / 255, 0) -- the UNORM8 export round-trips each byte. */
   r3v_synth_store_color(
      &b, nir_vec4(&b, nir_fmul_imm(&b, e0, 1.0 / 255.0),
                   nir_fmul_imm(&b, e1, 1.0 / 255.0),
                   nir_fmul_imm(&b, vz, 1.0 / 255.0),
                   nir_imm_float(&b, 0.0)));
   return r3v_synth_fs_cso(pipe, &b);
}

/* AFFINE_IOTA VS+FS synthesis: shared passthrough VS + the index-affine FS.
 * The dispatch uploads (width, stride, offset) to CONST[0] and draws with a
 * texel-unit varying quad instead of the 0..1 fullscreen texcoord. */
static bool
r3v_affine_iota_synthesize_shaders(struct r3v_device *device,
                                      struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_affine_iota_fs(pipe);
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
r3v_synthesize_mat4vec_fs(struct pipe_context *pipe)
{
   /* The 4x4 is uniform across every element, so it lives in the constant file
    * (CONST[0..3] = the four rows) rather than a texture: the dispatch uploads
    * the 64 bytes per draw and each output lane is one DP4 of a const row with
    * the per-element vertex.  That compiles to 1 TEX + 4 DP4.  The texture-matrix
    * variant needed 4 extra TEX (one per row) plus 4 coordinate-staging MOVs to
    * sample them at the fixed texel centres -- eight instructions and four
    * texture-cache fetches a const-file read does not cost.  The vertex is the
    * only sampler (stage 0), fetched at the interpolated fullscreen coord. */
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_mat4vec");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *vtx = r3v_synth_sample2d(&b, 0, coord);

   nir_def *lane[4];
   for (unsigned i = 0; i < 4; i++) {
      nir_def *row = r3v_synth_push_load(&b, 4, i * 16);
      lane[i] = nir_fdot4(&b, row, vtx);
   }

   r3v_synth_store_color(&b, nir_vec4(&b, lane[0], lane[1], lane[2], lane[3]));
   return r3v_synth_fs_cso(pipe, &b);
}

/* MAT4VEC VS+FS synthesis: the passthrough VS shared with DP4 plus the transform
 * FS.  The dispatch wraps the matrix as a 4x1 view + vertices per-element. */
static bool
r3v_mat4vec_synthesize_shaders(struct r3v_device *device,
                                  struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_mat4vec_fs(pipe);
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
r3v_synthesize_qfmul_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_qfmul");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *quat = r3v_synth_sample2d(&b, 0, coord);
   /* s in CONST[0].x = push byte 0, broadcast across the quaternion. */
   nir_def *s = r3v_synth_push_load(&b, 1, 0);
   r3v_synth_store_color(
      &b, nir_fmul(&b, quat, nir_swizzle(&b, s, (unsigned[]){0, 0, 0, 0}, 4)));
   return r3v_synth_fs_cso(pipe, &b);
}

/* QFMUL VS+FS synthesis: shared passthrough VS + the scalar-product FS.  The
 * dispatch uploads the broadcast scalar to CONST[0] and wraps the quaternions
 * per-element. */
static bool
r3v_qfmul_synthesize_shaders(struct r3v_device *device,
                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qfmul_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QROTATE FS: the sandwich q*embed(v)*conj(q) built by r3v_build_qrotate_fs_nir
 * -- two Hamilton products, eight DP4s, to the FP16 color export. */
static void *
r3v_synthesize_qrotate_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qrotate_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QROTATE VS+FS synthesis: the passthrough VS plus the sandwich FS. */
static bool
r3v_qrotate_synthesize_shaders(struct r3v_device *device,
                                  struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qrotate_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QCONJ FS: the sign-flip conjugate built by r3v_build_qconj_fs_nir -- one
 * sampled quaternion written as (a.x,-a.y,-a.z,-a.w) to the FP16 color export. */
static void *
r3v_synthesize_qconj_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qconj_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QCONJ VS+FS synthesis: the passthrough VS plus the conjugate FS. */
static bool
r3v_qconj_synthesize_shaders(struct r3v_device *device,
                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qconj_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QNORM FS: the squared-norm splat built by r3v_build_qnorm_fs_nir -- one
 * sampled quaternion written as vec4(dot(a,a)) to the FP16 color export. */
static void *
r3v_synthesize_qnorm_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qnorm_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QNORM VS+FS synthesis: the passthrough VS plus the self-dot FS. */
static bool
r3v_qnorm_synthesize_shaders(struct r3v_device *device,
                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qnorm_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

/* QNORMALIZE FS: a * rsqrt(dot(a,a)), the unit quaternion built by
 * r3v_build_qnormalize_fs_nir -- one DP4, the US RSQ, one vec4 scale. */
static void *
r3v_synthesize_qnormalize_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_qnormalize_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* QNORMALIZE VS+FS synthesis: the passthrough VS plus the normalize FS. */
static bool
r3v_qnormalize_synthesize_shaders(struct r3v_device *device,
                                     struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_qnormalize_fs(pipe);
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
r3v_omul_synthesize_shaders(struct r3v_device *device,
                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   nir_shader *lo = r3v_build_omul_lo_fs_nir(
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

   nir_shader *hi = r3v_build_omul_hi_fs_nir(
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
      nir_shader *mrt = r3v_build_omul_mrt_fs_nir(
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
r3v_make_fs_cso(struct pipe_context *pipe, nir_shader *s)
{
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);
   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR, .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

/* True when the screen can bind two simultaneous FP16 render targets -- the gate
 * for the single-pass MRT octonion ops. */
static bool
r3v_screen_supports_mrt_fp16(struct pipe_screen *screen)
{
   return screen->caps.max_render_targets >= 2 &&
          screen->is_format_supported(screen, PIPE_FORMAT_R16G16B16A16_FLOAT,
                                      PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET);
}

/* ONORM: passthrough VS + the self-dot-sum FS in fs_cso (the 2-in/1-out core). */
static bool
r3v_onorm_synthesize_shaders(struct r3v_device *device,
                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r3v_make_fs_cso(pipe, r3v_build_onorm_fs_nir(
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
r3v_oconj_synthesize_shaders(struct r3v_device *device,
                                struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   if (r3v_screen_supports_mrt_fp16(pipe->screen))
      pl->fs_cso_mrt = r3v_make_fs_cso(pipe, r3v_build_oconj_mrt_fs_nir(
         pipe->screen->nir_options[MESA_SHADER_FRAGMENT]));
   return true;
}

/* OADD/OSUB: passthrough VS + the MRT add/sub FS in fs_cso_mrt (is_sub from the
 * detected pattern).  Same FP16-MRT gate as OCONJ. */
static bool
r3v_oaddsub_synthesize_shaders(struct r3v_device *device,
                                  struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   if (r3v_screen_supports_mrt_fp16(pipe->screen))
      pl->fs_cso_mrt = r3v_make_fs_cso(pipe, r3v_build_oaddsub_mrt_fs_nir(
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
r3v_odiv_synthesize_shaders(struct r3v_device *device,
                               struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   const struct nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   if (pl->odiv.is_left) {
      pl->fs_cso  = r3v_make_fs_cso(pipe, r3v_build_odiv_l_lo_fs_nir(opts));
      pl->fs_cso2 = r3v_make_fs_cso(pipe, r3v_build_odiv_l_hi_fs_nir(opts));
   } else {
      pl->fs_cso  = r3v_make_fs_cso(pipe, r3v_build_odiv_lo_fs_nir(opts));
      pl->fs_cso2 = r3v_make_fs_cso(pipe, r3v_build_odiv_hi_fs_nir(opts));
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
r3v_otrans_synthesize_shaders(struct r3v_device *device,
                                 struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   const struct nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   pl->fs_cso  = r3v_make_fs_cso(pipe, r3v_build_omul_lo_fs_nir(opts));
   pl->fs_cso2 = r3v_make_fs_cso(pipe, r3v_build_omul_hi_fs_nir(opts));
   pl->fs_cso3 = r3v_make_fs_cso(pipe, r3v_build_otrans_p2_lo_fs_nir(opts));
   pl->fs_cso4 = r3v_make_fs_cso(pipe, r3v_build_otrans_p2_hi_fs_nir(opts));
   return pl->fs_cso != NULL && pl->fs_cso2 != NULL &&
          pl->fs_cso3 != NULL && pl->fs_cso4 != NULL;
}

/* QFMADD, QFMSUB, and QFMMUL synthesis: passthrough VS plus one fused FS. */
static bool
r3v_qfmadd_synthesize_shaders(struct r3v_device *device,
                                 struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r3v_make_fs_cso(pipe, r3v_build_qfmadd_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT], pl->qfmadd.is_sub));
   return pl->fs_cso != NULL;
}

static bool
r3v_qfmmul_synthesize_shaders(struct r3v_device *device,
                                 struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe || !r3v_device_init_identity_map_state(device))
      return false;
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;
   pl->fs_cso = r3v_make_fs_cso(pipe, r3v_build_qfmmul_fs_nir(
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
 * The sampler FS is a pure-NIR texel copy (TEX at the TEX0 interpolant, MOV
 * to the color export); NEAREST filtering comes from the sampler state bound
 * at dispatch replay, not the shader.  Synthesis failure is an allocation
 * failure: return the VkResult from pipeline creation instead of hiding it
 * behind a no-op dispatch path. */
static void *
r3v_synthesize_identity_tex_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_identity_tex");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   r3v_synth_store_color(&b, r3v_synth_sample2d(&b, 0, coord));
   return r3v_synth_fs_cso(pipe, &b);
}

static VkResult
r3v_identity_map_synthesize_shaders(struct r3v_device *device,
                                        struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: identity-map shader synthesis has no "
                       "pipe_context");

   simple_mtx_lock(&device->identity_map_cso_lock);

   if (!r3v_device_init_identity_map_state_locked(device)) {
      simple_mtx_unlock(&device->identity_map_cso_lock);
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "r3v: failed to create identity-map Gallium state");
   }

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso) {
      simple_mtx_unlock(&device->identity_map_cso_lock);
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "r3v: failed to synthesize identity-map VS");
   }

   pl->fs_cso = r3v_synthesize_identity_tex_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      simple_mtx_unlock(&device->identity_map_cso_lock);
      return vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "r3v: failed to synthesize identity-map FS");
   }
   simple_mtx_unlock(&device->identity_map_cso_lock);
   return VK_SUCCESS;
}

/* Synthesise the VS + FS pair for the blend-add reduction lowering.  The
 * shaders are structurally identical to r3v_identity_map_synthesize_shaders
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
 * r3v_device_init_identity_map_state -- the blend-acc orchestrator binds
 * those unchanged from the identity-map set. */
bool
r3v_device_init_blend_acc_reduction_state(struct r3v_device *device);

bool
r3v_device_init_blend_acc_reduction_state(struct r3v_device *device)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
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
r3v_synthesize_blend_acc_reduction_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_blend_acc");
   /* The varying carries the per-vertex reduction value; pass it to the
    * color export and let the bound ADD blend accumulate. */
   r3v_synth_store_color(&b, r3v_synth_load_varying(&b));
   return r3v_synth_fs_cso(pipe, &b);
}

static bool
r3v_blend_acc_reduction_synthesize_shaders(struct r3v_device *device,
                                              struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;
   if (!r3v_device_init_blend_acc_reduction_state(device))
      return false;

   /* The VS is the same vertex-passthrough shape as the identity-map and
    * binary-map paths: 2 attributes (POSITION + GENERIC) feed the
    * rasterizer.  The GENERIC attribute carries the per-vertex color the
    * orchestrator stages into the VBO (a packed RGBA8 of the kernel's per-gid
    * input value). */
    pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
    if (!pl->vs_cso)
      return false;

    pl->fs_cso = r3v_synthesize_blend_acc_reduction_fs(pipe);
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
 * The canonical r300 predicate convention here is "1.0 = pass,
 * 0.0 = kill", so the discard condition is predicate < 0.5: kill the
 * 0.0-baked fragments, pass the 1.0 survivors. */
static void *
r3v_synthesize_zpass_reduction_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_zpass_reduction");
   /* The baked predicate lives in the varying's x channel (the unwritten
    * y/z/w follow the GL/D3D (0, 0, 1) convention and must not join the
    * kill test): discard when predicate < 0.5 (the 0.0-baked discard
    * case), pass when predicate >= 0.5. */
   nir_def *pred = nir_channel(&b, r3v_synth_load_varying(&b), 0);
   nir_discard_if(&b, nir_flt_imm(&b, pred, 0.5));
   /* Surviving fragments' color content doesn't matter for the ZPASS
    * count, just that A fragment lands and the depth/stencil unit
    * increments the counter.  Reusing the predicate (1.0 for survivors)
    * keeps the program minimal. */
   r3v_synth_store_color(&b,
                         nir_swizzle(&b, pred, (unsigned[]){0, 0, 0, 0}, 4));
   return r3v_synth_fs_cso(pipe, &b);
}

static bool
r3v_zpass_reduction_synthesize_shaders(struct r3v_device *device,
                                          struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   /* Same vertex-passthrough as the other compute-as-raster lowerings:
    * 2 attributes (POSITION + GENERIC predicate-value). */
   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_zpass_reduction_fs(pipe);
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
r3v_synthesize_multipass_scan_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_multipass_scan");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   /* Sample the prior pass's RT, then double every channel.  The per-byte
    * UNORM8 doubling matches the kernel's uint *2 only while each byte stays
    * below 256 / 2^pass_count (the probe seeds inputs within that bound); a
    * channel that would exceed 1.0 clamps, which the readback oracle catches
    * as a mismatch rather than silently passing. */
   nir_def *prior = r3v_synth_sample2d(&b, 0, coord);
   r3v_synth_store_color(&b, nir_fmul_imm(&b, prior, 2.0));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Synthesise the multipass-scan VS + per-pass FS.  Same vertex-passthrough as
 * the other compute-as-raster lowerings (POSITION + GENERIC texcoord); the FS
 * is the doubling sampler program the orchestrator rebinds for each ping-pong
 * pass. */
static bool
r3v_multipass_scan_synthesize_shaders(struct r3v_device *device,
                                         struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_multipass_scan_fs(pipe);
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
r3v_synthesize_predicated_store_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_predicated_store");
   nir_def *coord = r3v_synth_load_texcoord(&b);
   nir_def *pred = r3v_synth_sample2d(&b, 0, coord);
   /* Discard when predicate.x < 1/512 (a UNORM8 zero byte); TGSI KILL_IF
    * kills on any negative channel, so the NIR spelling is the direct
    * comparison against the threshold. */
   nir_discard_if(&b, nir_flt_imm(&b, nir_channel(&b, pred, 0),
                                  1.0 / 512.0));
   r3v_synth_store_color(&b, r3v_synth_sample2d(&b, 1, coord));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Synthesise the predicated masked-store VS + FS pair.  Same fullscreen-quad
 * vertex passthrough and device-cached state CSOs as the identity / binary
 * lowerings; only the KILL_IF FS differs. */
static bool
r3v_predicated_store_synthesize_shaders(struct r3v_device *device,
                                           struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_predicated_store_fs(pipe);
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
 * derive_raster_extent in r3v_identity_map.c), so the displacement is
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
r3v_synthesize_multitap_gather_fs(struct pipe_context *pipe)
{
   const nir_shader_compiler_options *opts =
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, opts,
                                                  "r3v_multitap_gather");
   nir_def *tc = r3v_synth_load_texcoord(&b);
   /* CONST[0].xy = the neighbor texel displacement in the push window. */
   nir_def *delta = r3v_synth_push_load(&b, 2, 0);

   nir_def *t_c = nir_fmul_imm(&b, r3v_synth_sample2d(&b, 0, tc), 255.0);
   nir_def *t_l = nir_fmul_imm(
      &b, r3v_synth_sample2d(&b, 0, nir_fsub(&b, tc, delta)), 255.0);
   nir_def *t_r = nir_fmul_imm(
      &b, r3v_synth_sample2d(&b, 0, nir_fadd(&b, tc, delta)), 255.0);
   nir_def *sum = nir_fadd(&b, nir_fadd(&b, t_c, t_l), t_r);

   /* Lane-serial byte-carry chain x -> y -> z -> w: each lane adds the
    * previous lane's carry, splits into carry = trunc(s/256) and byte =
    * fract(s/256), and rescales the byte to UNORM8.  The lanes carry
    * DIFFERENT expressions, so they stay explicit rather than vectorized. */
   nir_def *lane[4], *carry = NULL;
   for (unsigned c = 0; c < 4; c++) {
      nir_def *s0 = nir_channel(&b, sum, c);
      if (carry)
         s0 = nir_fadd(&b, s0, carry);
      nir_def *s1 = nir_fmul_imm(&b, s0, 1.0 / 256.0);
      if (c < 3)
         carry = nir_ftrunc(&b, s1);
      lane[c] = nir_fmul_imm(&b, nir_ffract(&b, s1), 256.0 / 255.0);
   }
   r3v_synth_store_color(&b, nir_vec4(&b, lane[0], lane[1], lane[2], lane[3]));
   return r3v_synth_fs_cso(pipe, &b);
}

/* Synthesise the multi-tap gather VS + FS pair.  Same fullscreen-quad vertex
 * passthrough (POSITION + one GENERIC texcoord) and device-cached state CSOs
 * as the identity / binary lowerings; only the box-3 FS differs.  The FS reads
 * CONST[0] (the neighbor texel delta) which the orchestrator uploads per
 * dispatch. */
static bool
r3v_multitap_gather_synthesize_shaders(struct r3v_device *device,
                                          struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_multitap_gather_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

static void *
r3v_synthesize_ieee16_classify_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_ieee16_classify_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

static bool
r3v_ieee16_classify_synthesize_shaders(struct r3v_device *device,
                                          struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_ieee16_classify_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

static void *
r3v_synthesize_ieee16_mul_fs(struct pipe_context *pipe)
{
   nir_shader *s = r3v_build_ieee16_mul_fs_nir(
      pipe->screen->nir_options[MESA_SHADER_FRAGMENT]);
   if (pipe->screen->finalize_nir)
      pipe->screen->finalize_nir(pipe->screen, s, true);

   struct pipe_shader_state state = { .type = PIPE_SHADER_IR_NIR,
                                      .ir.nir = s };
   return pipe->create_fs_state(pipe, &state);
}

static bool
r3v_ieee16_mul_synthesize_shaders(struct r3v_device *device,
                                     struct r3v_pipeline *pl)
{
   struct pipe_context *pipe = device->pipe;
   if (!pipe)
      return false;
   if (!r3v_device_init_identity_map_state(device))
      return false;

   pl->vs_cso = r3v_synthesize_passthrough_vs(pipe);
   if (!pl->vs_cso)
      return false;

   pl->fs_cso = r3v_synthesize_ieee16_mul_fs(pipe);
   if (!pl->fs_cso) {
      pipe->delete_vs_state(pipe, pl->vs_cso);
      pl->vs_cso = NULL;
      return false;
   }
   return true;
}

static VkResult
r3v_synthesize_compute_shaders(struct r3v_device *device,
                                  struct r3v_pipeline *pl)
{
   if (!pl->admission.admissible)
      return VK_SUCCESS;

   /* CONSTFILL lowers to a framebuffer clear: no vs/fs CSO to synthesize, the
    * dispatch is self-contained.  Report success so the pipeline stays valid. */
   if (pl->const_fill.is_const_fill)
      return VK_SUCCESS;
   if (pl->identity_map.is_identity_map) {
      VkResult result = r3v_identity_map_synthesize_shaders(device, pl);
      if (result != VK_SUCCESS)
         return result;
      return VK_SUCCESS;
   }
   if (pl->multilimb_mul.is_multilimb_mul) {
      if (!r3v_multilimb_synthesize_shaders(device, pl))
         pl->multilimb_mul.is_multilimb_mul = false;
      return VK_SUCCESS;
   }
   if (pl->cas.is_cas) {
      if (!r3v_cas_synthesize_shaders(device, pl))
         pl->cas.is_cas = false;
      return VK_SUCCESS;
   }
   if (pl->log4_pool.is_log4_pool) {
      if (!r3v_log4_synthesize_shaders(device, pl))
         pl->log4_pool.is_log4_pool = false;
      return VK_SUCCESS;
   }
   if (pl->binary_map.is_binary_map) {
      if (!r3v_binary_map_synthesize_shaders(device, pl))
         pl->binary_map.is_binary_map = false;
      return VK_SUCCESS;
   }
   if (pl->unary_map.is_unary_map) {
      if (!r3v_unary_map_synthesize_shaders(device, pl))
         pl->unary_map.is_unary_map = false;
      return VK_SUCCESS;
   }
   if (pl->unary_transcendental.is_unary_transcendental) {
      if (!r3v_unary_transcendental_synthesize_shaders(device, pl))
         pl->unary_transcendental.is_unary_transcendental = false;
      return VK_SUCCESS;
   }
   if (pl->binary_transcendental.is_binary_transcendental) {
      if (!r3v_binary_transcendental_synthesize_shaders(device, pl))
         pl->binary_transcendental.is_binary_transcendental = false;
      return VK_SUCCESS;
   }
   if (pl->bitwise_logicop.is_bitwise_logicop) {
      /* Both draws of the bitwise carrier are plain texel copies -- the ROP
       * logic op does the bitwise combine -- so the passthrough VS + copy FS the
       * identity map synthesizes serve unchanged. */
      if (r3v_identity_map_synthesize_shaders(device, pl) != VK_SUCCESS)
         pl->bitwise_logicop.is_bitwise_logicop = false;
      return VK_SUCCESS;
   }
   if (pl->shift_logical.is_shift_logical) {
      if (!r3v_shift_logical_synthesize_shaders(device, pl))
         pl->shift_logical.is_shift_logical = false;
      return VK_SUCCESS;
   }
   if (pl->shift_variable.is_shift_variable) {
      if (!r3v_shift_variable_synthesize_shaders(device, pl))
         pl->shift_variable.is_shift_variable = false;
      return VK_SUCCESS;
   }
   if (pl->affine_iota.is_affine_iota) {
      if (!r3v_affine_iota_synthesize_shaders(device, pl))
         pl->affine_iota.is_affine_iota = false;
      return VK_SUCCESS;
   }
   if (pl->dp4.is_dp4) {
      if (!r3v_dp4_synthesize_shaders(device, pl))
         pl->dp4.is_dp4 = false;
      return VK_SUCCESS;
   }
   if (pl->qmul.is_qmul) {
      if (!r3v_qmul_synthesize_shaders(device, pl))
         pl->qmul.is_qmul = false;
      return VK_SUCCESS;
   }
   if (pl->qdiv.is_qdiv) {
      if (!r3v_qdiv_synthesize_shaders(device, pl))
         pl->qdiv.is_qdiv = false;
      return VK_SUCCESS;
   }
   if (pl->mat4vec.is_mat4vec) {
      if (!r3v_mat4vec_synthesize_shaders(device, pl))
         pl->mat4vec.is_mat4vec = false;
      return VK_SUCCESS;
   }
   if (pl->qfmul.is_qfmul) {
      if (!r3v_qfmul_synthesize_shaders(device, pl))
         pl->qfmul.is_qfmul = false;
      return VK_SUCCESS;
   }
   if (pl->qrotate.is_qrotate) {
      if (!r3v_qrotate_synthesize_shaders(device, pl))
         pl->qrotate.is_qrotate = false;
      return VK_SUCCESS;
   }
   if (pl->qconj.is_qconj) {
      if (!r3v_qconj_synthesize_shaders(device, pl))
         pl->qconj.is_qconj = false;
      return VK_SUCCESS;
   }
   if (pl->qnorm.is_qnorm) {
      if (!r3v_qnorm_synthesize_shaders(device, pl))
         pl->qnorm.is_qnorm = false;
      return VK_SUCCESS;
   }
   if (pl->qnormalize.is_qnormalize) {
      if (!r3v_qnormalize_synthesize_shaders(device, pl))
         pl->qnormalize.is_qnormalize = false;
      return VK_SUCCESS;
   }
   if (pl->omul.is_omul) {
      if (!r3v_omul_synthesize_shaders(device, pl))
         pl->omul.is_omul = false;
      return VK_SUCCESS;
   }
   if (pl->oaddsub.is_oaddsub) {
      if (!r3v_oaddsub_synthesize_shaders(device, pl))
         pl->oaddsub.is_oaddsub = false;
      return VK_SUCCESS;
   }
   if (pl->oconj.is_oconj) {
      if (!r3v_oconj_synthesize_shaders(device, pl))
         pl->oconj.is_oconj = false;
      return VK_SUCCESS;
   }
   if (pl->onorm.is_onorm) {
      if (!r3v_onorm_synthesize_shaders(device, pl))
         pl->onorm.is_onorm = false;
      return VK_SUCCESS;
   }
   if (pl->odiv.is_odiv) {
      if (!r3v_odiv_synthesize_shaders(device, pl))
         pl->odiv.is_odiv = false;
      return VK_SUCCESS;
   }
   if (pl->otrans.is_otrans) {
      if (!r3v_otrans_synthesize_shaders(device, pl))
         pl->otrans.is_otrans = false;
      return VK_SUCCESS;
   }
   if (pl->qfmadd.is_qfmadd) {
      if (!r3v_qfmadd_synthesize_shaders(device, pl))
         pl->qfmadd.is_qfmadd = false;
      return VK_SUCCESS;
   }
   if (pl->qfmmul.is_qfmmul) {
      if (!r3v_qfmmul_synthesize_shaders(device, pl))
         pl->qfmmul.is_qfmmul = false;
      return VK_SUCCESS;
   }
   if (pl->blend_acc_reduction.is_blend_acc_reduction) {
      if (!r3v_blend_acc_reduction_synthesize_shaders(device, pl))
         pl->blend_acc_reduction.is_blend_acc_reduction = false;
      return VK_SUCCESS;
   }
   if (pl->zpass_reduction.is_zpass_reduction) {
      if (!r3v_zpass_reduction_synthesize_shaders(device, pl))
         pl->zpass_reduction.is_zpass_reduction = false;
      return VK_SUCCESS;
   }
   if (pl->multipass_scan.is_multipass_scan) {
      if (!r3v_multipass_scan_synthesize_shaders(device, pl))
         pl->multipass_scan.is_multipass_scan = false;
      return VK_SUCCESS;
   }
   if (pl->predicated_store.is_predicated_store) {
      if (!r3v_predicated_store_synthesize_shaders(device, pl))
         pl->predicated_store.is_predicated_store = false;
      return VK_SUCCESS;
   }
   if (pl->multitap_gather.is_multitap_gather) {
      if (!r3v_multitap_gather_synthesize_shaders(device, pl))
         pl->multitap_gather.is_multitap_gather = false;
      return VK_SUCCESS;
   }
   if (pl->ieee16_classify.is_ieee16_classify) {
      if (!r3v_ieee16_classify_synthesize_shaders(device, pl))
         pl->ieee16_classify.is_ieee16_classify = false;
      return VK_SUCCESS;
   }
   if (pl->ieee16_mul.is_ieee16_mul) {
      if (!r3v_ieee16_mul_synthesize_shaders(device, pl))
         pl->ieee16_mul.is_ieee16_mul = false;
      return VK_SUCCESS;
   }

   return VK_SUCCESS;
}

static VkResult
r3v_create_one_compute_pipeline(struct r3v_device *device,
                                    const VkComputePipelineCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct r3v_pipeline **out_pipeline,
                                    uint32_t i)
{
   struct r300_compute_admission adm;
   struct r300_compute_identity_pattern ident = {0};
   struct r300_compute_binary_map_pattern binmap = {0};
   struct r300_compute_unary_map_pattern unary_pat = {0};
   struct r300_compute_unary_transcendental_pattern transc_pat = {0};
   struct r300_compute_binary_transcendental_pattern btransc_pat = {0};
   struct r300_compute_bitwise_logicop_pattern bitwise_pat = {0};
   struct r300_compute_shift_logical_pattern shift_pat = {0};
   struct r300_compute_shift_variable_pattern shiftvar_pat = {0};
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

   if (!r3v_classify_compute_kernel(device, &pCreateInfo->stage,
                                       &adm, &ident, &binmap, &unary_pat,
                                       &transc_pat, &btransc_pat, &bitwise_pat,
                                       &shift_pat, &shiftvar_pat,
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
                       "r3v: SPIR-V to NIR failed for compute kernel %u",
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
              "r3v: compute kernel %u is not lowerable to the RS482 raster "
              "substrate: %s (%s); dispatches will no-op",
              i, r300_compute_reject_name(adm.reason),
              adm.detail ? adm.detail : "no detail");
   }
   struct r3v_pipeline *pl =
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
   pl->unary_transcendental = transc_pat;
   pl->binary_transcendental = btransc_pat;
   pl->bitwise_logicop = bitwise_pat;
   pl->shift_logical = shift_pat;
   pl->shift_variable = shiftvar_pat;
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

   /* An admitted kernel that matched no raster verb dispatches as a silent
    * no-op (R300_COMPUTE_REJECT_UNKNOWN_SHAPE in r3v_replay_dispatch).  The
    * reject-reason warning above only fires for kernels the classifier
    * rejected; this one passed admission yet no detector recognized its shape,
    * so surface it here too -- an app (or probe harness) then learns the
    * dispatch will not write its output at create time instead of discovering
    * a silently-unwritten buffer after submit. */
   if (adm.admissible && !r3v_pipeline_matched_raster_verb(pl)) {
      vk_logw(VK_LOG_OBJS(&device->vk.base),
              "r3v: compute kernel %u passed admission but matches no raster "
              "verb; dispatches will no-op (UNKNOWN_SHAPE)", i);
   }

   VkResult synth_result = r3v_synthesize_compute_shaders(device, pl);
   if (synth_result != VK_SUCCESS) {
      r3v_DestroyPipeline(r3v_device_to_handle(device),
                             r3v_pipeline_to_handle(pl), pAllocator);
      return synth_result;
   }

   *out_pipeline = pl;
   return VK_SUCCESS;
}

VkResult
r3v_CreateComputePipelines(VkDevice _device,
                              VkPipelineCache pipelineCache,
                              uint32_t createInfoCount,
                              const VkComputePipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   (void)pipelineCache;

   if (createInfoCount == 0)
      return VK_SUCCESS;

   for (uint32_t i = 0; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   if (!device->hybrid_compute_enabled)
      return vk_errorf(device, VK_ERROR_FEATURE_NOT_PRESENT,
                       "r3v: compute is not exposed (set "
                       R3V_HYBRID_COMPUTE_ENV "=1 for the experimental "
                       "hybrid-compute path)");

   for (uint32_t i = 0; i < createInfoCount; i++) {
      struct r3v_pipeline *pl = NULL;
      VkResult result = r3v_create_one_compute_pipeline(device, &pCreateInfos[i],
                                                            pAllocator, &pl, i);
      if (result != VK_SUCCESS)
         return result;
      pPipelines[i] = r3v_pipeline_to_handle(pl);
   }

   return VK_SUCCESS;
}

void
r3v_DestroyPipeline(VkDevice _device,
                        VkPipeline _pipeline,
                        const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_pipeline, pl, _pipeline);
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
   if (pl->shift_variable_gather_fs)
      device->pipe->delete_fs_state(device->pipe, pl->shift_variable_gather_fs);
   if (pl->shift_variable_signfill_fs)
      device->pipe->delete_fs_state(device->pipe, pl->shift_variable_signfill_fs);
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
