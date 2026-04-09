/*
 * Copyright (c) 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * terakan_nir_lower_abi.c — Instruction emission (HOW)
 *
 * Emits hardware instructions (VFETCH, TEX, KCACHE, MEM_RAT) from
 * physical hardware indices resolved by apply_pipeline_layout.
 *
 * Phase 0 mechanical file split of the former monolith
 * terakan_nir_lower_bindings.c.  See TERAKAN_SHADER_ABI_CONTRACT.md.
 */

#include "terakan_nir_lower_bindings_internal.h"

static void
terakan_nir_build_uav_instr_r600(nir_builder * const b, nir_def * const uav_array_index,
                                 nir_def * const coord, nir_def * const value,
                                 nir_def * const compare_value, unsigned const uav_op,
                                 enum gl_access_qualifier const access,
                                 unsigned const id_base)
{
   nir_intrinsic_instr * const intrin =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_uav_instr_r600);
   intrin->src[0] = nir_src_for_ssa(uav_array_index);
   intrin->src[1] = nir_src_for_ssa(coord);
   intrin->src[2] = nir_src_for_ssa(value);
   intrin->src[3] = nir_src_for_ssa(compare_value);
   nir_intrinsic_set_uav_op_r600(intrin, uav_op);
   nir_intrinsic_set_access(intrin, access);
   nir_intrinsic_set_id_base(intrin, id_base);
   nir_builder_instr_insert(b, &intrin->instr);
}

static nir_def *
terakan_nir_build_uav_returning_instr_r600(nir_builder * const b,
                                           unsigned const num_components,
                                           unsigned const bit_size,
                                           nir_def * const uav_array_index,
                                           nir_def * const coord, nir_def * const value,
                                           nir_def * const compare_value,
                                           nir_def * const immed_index,
                                           unsigned const uav_op,
                                           enum gl_access_qualifier const access,
                                           unsigned const id_base,
                                           unsigned const uav_return_id_base_r600)
{
   nir_intrinsic_instr * const intrin =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_uav_returning_instr_r600);
   intrin->num_components = (uint8_t)num_components;
   nir_def_init(&intrin->instr, &intrin->def, intrin->num_components, bit_size);
   intrin->src[0] = nir_src_for_ssa(uav_array_index);
   intrin->src[1] = nir_src_for_ssa(coord);
   intrin->src[2] = nir_src_for_ssa(value);
   intrin->src[3] = nir_src_for_ssa(compare_value);
   intrin->src[4] = nir_src_for_ssa(immed_index);
   nir_intrinsic_set_uav_op_r600(intrin, uav_op);
   nir_intrinsic_set_access(intrin, access);
   nir_intrinsic_set_id_base(intrin, id_base);
   nir_intrinsic_set_uav_return_id_base_r600(intrin, uav_return_id_base_r600);
   nir_intrinsic_set_mega_fetch_count_r600(intrin, 0);
   nir_builder_instr_insert(b, &intrin->instr);
   return &intrin->def;
}

/* Section 8.2 "Dataflow in Memory Hierarchy" of Evergreen Family Instruction Set Architecture says:
 *
 *     "Buffer objects are generally read and written directly by the work-items. Data is accessed
 *     through the L2 and L1 data caches on the GPU, but immediately invalidated at the end of a
 *     clause. [...] Similarly, writes are executed through the "fast-path" (depth buffer or DB) or
 *     "complete-path" (color buffer or CB), which have write-only caches that are invalidated, and
 *     all update bits are sent to memory at the end of a clause."
 *
 *     "Image objects are limited to read-only or write-only (no concurrent r/w). Thus, on reads,
 *     the data is cached through the L2 and L1 data caches; on writes, the data is cached through
 *     the CB/DB buffers."
 *
 * Therefore, for storage buffer and storage texel buffer loads, vertex fetch can always be used,
 * but for storage images, if write-read coherence is needed, the load must be performed via a
 * NOP_RTN UAV operation.
 */


static nir_def *
terakan_nir_uav_immed_index(nir_builder * const b,
                            struct terakan_physical_device_chip_info const * const chip_info)
{
   nir_def * wave_id = nir_load_hw_wave_id_r600(b);
   if (chip_info->two_shader_engines_max) {
      wave_id =
         nir_umad24_relaxed(b, nir_imm_int(b, 2), wave_id, nir_load_shader_engine_id_r600(b));
   }
   /* TODO(Triang3l): See how MBCNT behaves on wave32 chips and possibly scale the wave ID by 32
    * there.
    */
   return nir_umad24_relaxed(b, nir_imm_int(b, 64), wave_id,
                             nir_mbcnt_amd(b, nir_imm_int(b, ~0), nir_imm_zero(b, 1, 32)));
}

static nir_def *
terakan_nir_image_uav_coord(nir_builder * const b, nir_def * const image_coord,
                            enum glsl_sampler_dim const dim, bool const is_array)
{
   /* Buffers need separate handling due to the UAV base granularity offset. */
   assert(dim != GLSL_SAMPLER_DIM_BUF);

   unsigned uav_coord_num_components = 2;
   nir_def * uav_coord_components[3] = {
      nir_channel(b, image_coord, 0),
      /* 1D images may be promoted to 2D if they're tiled (such as if they're used by DB), so always
       * specify Y = 0 for them.
       */
      dim == GLSL_SAMPLER_DIM_1D ? nir_imm_zero(b, 1, 32) : nir_channel(b, image_coord, 1),
   };

   /* The hardware accepts the array layer in Z for both 1D and 2D/3D. It's relevant only if
    * RESOURCE_TYPE is TEXTURE#DARRAY or TEXTURE3D. UAV instructions don't accept a coordinate
    * swizzle, so don't initialize the array layer if it's not needed to avoid emitting an ALU
    * instruction for it.
    */
   if (dim == GLSL_SAMPLER_DIM_3D || is_array) {
      uav_coord_num_components = 3;
      uav_coord_components[2] = nir_channel(b, image_coord, dim == GLSL_SAMPLER_DIM_1D ? 1 : 2);
   }

   return nir_vec(b, uav_coord_components, uav_coord_num_components);
}


/*
 * terakan_nir_emit_write_guard
 *
 * Emits a software bounds check before a MEM_RAT write (store or atomic).
 * Returns an nir_def* that is true (non-zero) when the write is in-bounds,
 * false when OOB.  The caller wraps the actual MEM_RAT instruction in an
 * nir_push_if / nir_pop_if gated on this result.
 *
 * Hardware basis: MEM_RAT writes are NOT bounds-checked by Evergreen silicon
 * (Phase 5 Probe 7).  OOB MEM_RAT writes corrupt adjacent VRAM.  This is
 * the only defense.
 *
 * The UAV byte size is read from KCACHE bank 14 (robustness metadata buffer),
 * which holds uint32_t buffer_uav_byte_size[12] at dword offsets [0..11].
 *
 * Guard condition:  (write_end_offset <= uav_byte_size)
 *   where write_end_offset = byte_offset + write_size_bytes
 *
 * When the guard fails (OOB), the write is suppressed via IF/ENDIF.
 * Future optimization: trash-page redirect for deep CF stacks (Tier 2).
 */
static nir_def *
terakan_nir_emit_write_guard(nir_builder * const b,
                             nir_def * const byte_offset,
                             uint32_t const write_size_bytes,
                             uint32_t const uav_index_zero_based,
                             nir_def * const uav_array_index,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Mark KCACHE bank 14 (robustness metadata) as needed. */
   *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;

   /* Load the exact byte size for this UAV from KCACHE bank 14.
    * Layout: uint32_t buffer_uav_byte_size[12] starting at dword 0. */
   uint32_t const size_vec4_index = uav_index_zero_based / 4;
   uint32_t const size_component = uav_index_zero_based % 4;

   nir_def *uav_byte_size = nir_load_kcache_r600(
      b, 1, 32, uav_array_index,
      .access = ACCESS_CAN_REORDER,
      .id_base = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
      .base = size_vec4_index,
      .component = size_component);

   /* write_end = byte_offset + write_size_bytes (saturating add to prevent
    * overflow: if byte_offset is near UINT32_MAX, the add would wrap to a
    * small value and pass the bounds check). */
   nir_def *write_end = nir_uadd_sat(b, byte_offset, nir_imm_int(b, write_size_bytes));

   /* in_bounds = (write_end <= uav_byte_size) */
   return nir_uge(b, uav_byte_size, write_end);
}

/*
 * terakan_nir_buffer_uav_coord
 *
 * Emits the UAV coordinate computation for storage-buffer and texel-buffer
 * accesses.  When robust_access is true, injects a software ALU umin clamp
 * (nir_umin_imm) that bounds the coordinate so that no possible base
 * granularity offset can wrap it into an in-bounds address.
 *
 * --- MANDATORY SOFTWARE BOUNDS CLAMP (DO NOT BYPASS) ---
 *
 * Terascale silicon (Evergreen-class VLIW5, including the Brazos E-300
 * target) does NOT provide deterministic hardware out-of-bounds handling
 * for UAV writes, texel-buffer fetches, or byte-granular descriptor
 * clamping.  Empirically observed failure modes include GPU ring lockups
 * on raw buffer overruns via the texture fetch unit, and adjacent-block
 * corruption when the rasterizer write mask is not byte-aligned.
 *
 * Consequently, the software ALU clamp below is the ONLY guarantee of
 * deterministic Vulkan robustness semantics on this hardware.  Future
 * revisions must not replace it with silicon-trust heuristics, hardware
 * probes, or "fast paths" conditional on undocumented GPU behavior.  If
 * a future probe demonstrates that hardware behavior is clean across the
 * entire parameter space, the clamp may be dropped only under an explicit
 * `TERAKAN_ROBUSTNESS_HW_VERIFIED` compile-time flag, never by default.
 *
 * The effective robust_access flag is threaded from the caller via
 * terakan_nir_lower_bindings_state::robust_buffer_access, which the
 * pipeline compiler computes as:
 *     device->enabled_features.robustBufferAccess
 *   OR
 *     any per-stage/per-pipeline VK_EXT_pipeline_robustness state
 */
static nir_def *
terakan_nir_buffer_uav_coord(nir_builder * const b, nir_def * coord,
                             uint32_t const uav_index_zero_based, nir_def * const uav_array_index,
                             bool const robust_access, bool const include_helpers,
                             struct terakan_nir_lower_bindings_state * const state)
{
   /* Add the UAV base granularity offset. */

   if (robust_access) {
      /* SOFTWARE ALU BOUNDS CLAMP — MANDATORY on Terascale.  See function
       * header comment for the hardware-correctness rationale.
       *
       * If the coordinate provided by the application is already near
       * UINT32_MAX, adding the UAV base granularity offset may wrap an
       * out-of-bounds address into an in-bounds address near zero.
       * Clamp the coordinate so that no possible base granularity offset
       * value can wrap.
       *
       * For storage buffers, the offset is provided in bytes, but the UAV
       * uses a format with 4 bytes per element, for which
       * terakan_color_descriptor_buffer_uav_base_granularity_log2 is the
       * pipe interleave.
       * For texel buffers, the offset is in elements, but for all possible
       * element sizes terakan_color_descriptor_buffer_uav_base_granularity_log2
       * divided by the element size never exceeds the pipe interleave.
       * Moreover, texel buffers are fetched at a signed coordinate, not
       * unsigned, so any address > INT32_MAX is out-of-bounds.
       */
      struct terakan_physical_device const * const physical_device = container_of(
         state->layout->vk.base.device->physical, struct terakan_physical_device const, vk);
      uint32_t const max_uav_range =
         ~(((uint32_t)1 << physical_device->tiling_info.pipe_interleave_bytes_log2) - 1);
      assert(physical_device->vk.properties.maxStorageBufferRange <= max_uav_range);
      coord = nir_umin_imm(b, coord, max_uav_range);
   }

   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_BUFFER_UAV_BASE_GRANULARITY_OFFSET);
   /* TODO(Triang3l): If the array index is constant, load via kcache rather
    * than vertex fetch.  This would save 20-40 cycles by reading the UAV base
    * granularity offset from KCACHE bank 15 (push constants) at a static vec4
    * index rather than VFETCH.  Requires the index to be constant-folded by
    * NIR before this pass runs. */
   BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
   nir_def * const base_granularity_offset = nir_load_buffer_resource_r600(
      b, 1, 32, nir_imm_zero(b, 1, 32), nir_ishl_imm(b, uav_array_index, 2),
      .access = ACCESS_CAN_REORDER | (include_helpers ? ACCESS_INCLUDE_HELPERS : 0),
      .id_base = TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS,
      .base = offsetof(struct terakan_push_constants_driver, buffer_uav_base_granularity_offset) +
              sizeof(uint32_t) * uav_index_zero_based,
      .format = PIPE_FORMAT_R32_UINT);
   return nir_iadd_nuw(b, coord, base_granularity_offset);
}

/* If there was an error while lowering the binding, such as if the binding was not obtained
 * successfully, to avoid leaving the shader in an indeterminate state as it's very easy for an app
 * to cause issues like those, treating invalid instructions largely as null descriptor accesses -
 * returning zero for loads, ignoring stores.
 */
static void
terakan_nir_lower_bindings_instr_to_null(nir_instr * const instr)
{
   nir_def * const old_def = nir_instr_def(instr);
   if (old_def != NULL) {
      nir_builder b = nir_builder_at(nir_before_instr(instr));
      nir_def_rewrite_uses(old_def, nir_imm_zero(&b, old_def->num_components, old_def->bit_size));
   }
   nir_instr_remove(instr);
}

static bool
terakan_nir_lower_bindings_instr_tex(nir_builder * const b, nir_tex_instr * const tex,
                                     struct terakan_nir_lower_bindings_state * const state)
{
   bool shader_nir_progress = false;

   mesa_shader_stage const stage = b->shader->info.stage;

   /* If nir_tex_src_texture/sampler_deref isn't present, the lowering was possibly invoked multiple
    * times, just ignore the instruction if it has already been lowered.
    */

   struct terakan_nir_binding binding;

   int const texture_deref_src_index = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (likely(texture_deref_src_index != -1)) {
      nir_tex_src * const texture_src = &tex->src[texture_deref_src_index];
      if (unlikely(!terakan_nir_get_binding(texture_src->src, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                            state->layout, b->shader, &binding))) {
         terakan_nir_lower_bindings_instr_to_null(&tex->instr);
         return true;
      }
      uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                          binding.set_binding->first_shader_resources[stage];
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      shader_nir_progress = true;
      tex->texture_index = resource_index_base;
      if (binding.array_index != NULL) {
         if (nir_def_instr(binding.array_index)->type == nir_instr_type_load_const) {
            tex->texture_index +=
               nir_instr_as_load_const(nir_def_instr(binding.array_index))->value[0].u32;
            nir_tex_instr_remove_src(tex, texture_deref_src_index);
         } else {
            texture_src->src_type = nir_tex_src_texture_offset;
            nir_src_rewrite(&texture_src->src, binding.array_index);
         }
      } else {
         nir_tex_instr_remove_src(tex, texture_deref_src_index);
      }
   }

   int const sampler_deref_src_index = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
   if (likely(sampler_deref_src_index != -1)) {
      nir_tex_src * const sampler_src = &tex->src[sampler_deref_src_index];
      if (unlikely(!terakan_nir_get_binding(sampler_src->src, VK_DESCRIPTOR_TYPE_SAMPLER,
                                            state->layout, b->shader, &binding))) {
         terakan_nir_lower_bindings_instr_to_null(&tex->instr);
         return true;
      }
      uint8_t const sampler_index_base = binding.set->first_shader_samplers[stage] +
                                         binding.set_binding->first_shader_samplers[stage];
      *state->samplers_needed |=
         BITFIELD_RANGE(sampler_index_base + binding.array_index_range_first,
                        binding.array_index_range_last - binding.array_index_range_first + 1);
      shader_nir_progress = true;
      tex->sampler_index = sampler_index_base;
      if (binding.array_index != NULL) {
         if (nir_def_instr(binding.array_index)->type == nir_instr_type_load_const) {
            tex->sampler_index +=
               nir_instr_as_load_const(nir_def_instr(binding.array_index))->value[0].u32;
            nir_tex_instr_remove_src(tex, sampler_deref_src_index);
         } else {
            sampler_src->src_type = nir_tex_src_sampler_offset;
            nir_src_rewrite(&sampler_src->src, binding.array_index);
         }
      } else {
         nir_tex_instr_remove_src(tex, sampler_deref_src_index);
      }
   }

   return shader_nir_progress;
}

static void
terakan_nir_lower_bindings_instr_load_ubo(nir_builder * const b, nir_intrinsic_instr * const intrin,
                                          struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ubo);

   b->cursor = nir_before_instr(&intrin->instr);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL) {
      binding.array_index = nir_imm_zero(b, 1, 32);
   }

   mesa_shader_stage const stage = b->shader->info.stage;

   /* KCACHE fast path for static UBO offsets (1-cycle ALU-inline constant).
    * Dynamic offsets fall back to VFETCH (~20-40 cycles through texture cache).
    * Both paths work because CmdBindDescriptorSets dual-binds every UBO to
    * both SQ_TEX_RESOURCE (VFETCH) and KCACHE banks. */

   uint8_t const kcache_bank_base = binding.set->first_shader_uniform_buffers[stage] +
                                    binding.set_binding->first_shader_uniform_buffers[stage];

   nir_const_value *offset_const = nir_src_as_const_value(intrin->src[1]);

   if (offset_const != NULL && kcache_bank_base < TERAKAN_KCACHE_MAX_UNIFORM_BUFFERS) {
      /* Static offset: direct KCACHE read. */
      uint32_t byte_offset = offset_const->u32;
      uint32_t vec4_index = byte_offset / 16;
      uint32_t first_component = (byte_offset % 16) / 4;

      /* Mark this KCACHE bank as needed for hw_state emission. */
      if (binding.array_index_range_first == binding.array_index_range_last) {
         *state->kcache_needed |= (uint16_t)1 << (kcache_bank_base + binding.array_index_range_first);
      } else {
         for (unsigned ai = binding.array_index_range_first; ai <= binding.array_index_range_last; ++ai) {
            if (kcache_bank_base + ai < 16)
               *state->kcache_needed |= (uint16_t)1 << (kcache_bank_base + ai);
         }
      }

      nir_def *result = nir_load_kcache_r600(
         b, intrin->num_components, 32, binding.array_index,
         .access = nir_intrinsic_access(intrin) | ACCESS_CAN_REORDER,
         .id_base = kcache_bank_base,
         .base = vec4_index,
         .component = first_component);

      if (intrin->def.bit_size != 32) {
         result = nir_u2uN(b, result, intrin->def.bit_size);
      }

      nir_def_rewrite_uses(&intrin->def, result);
      nir_instr_remove(&intrin->instr);
   } else {
      /* Dynamic offset or bank overflow: VFETCH fallback. */
      uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                          binding.set_binding->first_shader_resources[stage];
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(&intrin->def,
                           terakan_nir_load_raw_resource_buffer(
                              b, intrin->num_components, intrin->def.bit_size,
                              nir_intrinsic_access(intrin) | ACCESS_CAN_REORDER,
                              resource_index_base, binding.array_index, 0, intrin->src[1].ssa));
      nir_instr_remove(&intrin->instr);
   }
}

static void
terakan_nir_lower_bindings_instr_load_push_constant(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_push_constant);

   b->cursor = nir_before_instr(&intrin->instr);

   /* Push constants don't have access robustness, simply add the base without an integer overflow
    * check.
    */

   /* KCACHE bank 15 for push constants (static offset = 1-cycle). */
   uint32_t static_base = TERAKAN_PUSH_CONSTANTS_APP_BASE_BYTES +
                           nir_intrinsic_base(intrin);

   nir_const_value *dyn_offset_const = nir_src_as_const_value(intrin->src[0]);

   if (dyn_offset_const != NULL) {
      uint32_t byte_offset = static_base + dyn_offset_const->u32;
      uint32_t vec4_index = byte_offset / 16;
      uint32_t first_component = (byte_offset % 16) / 4;

      /* Mark push constant bank as needed. */
      *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS;

      nir_def *result = nir_load_kcache_r600(
         b, intrin->num_components, 32, nir_imm_zero(b, 1, 32),
         .access = ACCESS_CAN_REORDER,
         .id_base = TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
         .base = vec4_index,
         .component = first_component);

      if (intrin->def.bit_size != 32) {
         result = nir_u2uN(b, result, intrin->def.bit_size);
      }

      nir_def_rewrite_uses(&intrin->def, result);
      nir_instr_remove(&intrin->instr);
   } else {
      BITSET_SET(state->resources_needed, TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS);
      nir_def_rewrite_uses(
         &intrin->def,
         terakan_nir_load_raw_resource_buffer(
            b, intrin->num_components, intrin->def.bit_size, ACCESS_CAN_REORDER,
            TERAKAN_RESOURCE_RANGE_PUSH_CONSTANTS, nir_imm_int(b, 0),
            static_base, intrin->src[0].ssa));
      nir_instr_remove(&intrin->instr);
   }
}


/*
 * Lower load_num_workgroups to a KCACHE read from the driver push constant
 * prefix in bank 15.
 *
 * The SFN backend handles load_num_workgroups via a VFETCH from
 * R600_BUFFER_INFO_CONST_BUFFER (gallium resource 15), but the Terakan Vulkan
 * driver does not bind that gallium-convention buffer.  Instead, the dispatch
 * code writes (group_count_x, group_count_y, group_count_z) into the driver
 * push constants, and we read them here via the always-bound KCACHE bank 15.
 *
 * Layout: num_workgroups[3] is at byte offset
 *   offsetof(terakan_push_constants_driver, num_workgroups) = 52
 *   → vec4_index = 3, first_component = 1 (components .yzw of vec4[3])
 */
static void
terakan_nir_lower_bindings_instr_load_num_workgroups(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_num_workgroups);
   assert(b->shader->info.stage == MESA_SHADER_COMPUTE);

   b->cursor = nir_before_instr(&intrin->instr);

   /* Byte offset of num_workgroups within terakan_push_constants_driver.
    * 12 × uint32 (UAV offsets) + 1 × uint32 (draw_id) = 52 bytes. */
   uint32_t const byte_offset =
      offsetof(struct terakan_push_constants_driver, num_workgroups);
   uint32_t const vec4_index = byte_offset / 16;
   uint32_t const first_component = (byte_offset % 16) / 4;

   /* Mark push constant KCACHE bank as needed. */
   *state->kcache_needed |= (uint16_t)1 << TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS;

   /* Mark the driver push constant slots as used so the dispatch code
    * knows to populate them. */
   *state->driver_push_constants_used |=
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_X) |
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Y) |
      BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Z);

   nir_def *result = nir_load_kcache_r600(
      b, 3, 32, nir_imm_zero(b, 1, 32),
      .access = ACCESS_CAN_REORDER,
      .id_base = TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
      .base = vec4_index,
      .component = first_component);

   if (intrin->def.bit_size != 32) {
      result = nir_u2uN(b, result, intrin->def.bit_size);
   }

   nir_def_rewrite_uses(&intrin->def, result);
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_load_ssbo(nir_builder * const b,
                                           nir_intrinsic_instr * const intrin,
                                           struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_load_ssbo);

   b->cursor = nir_before_instr(&intrin->instr);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   if (binding.array_index == NULL) {
      binding.array_index = nir_imm_zero(b, 1, 32);
   }

   /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
   uint8_t const resource_index_base =
      binding.set->first_shader_resources[b->shader->info.stage] +
      binding.set_binding->first_shader_resources[b->shader->info.stage];
   BITSET_SET_RANGE(state->resources_needed, resource_index_base + binding.array_index_range_first,
                    resource_index_base + binding.array_index_range_last);
   nir_def_rewrite_uses(&intrin->def, terakan_nir_load_raw_resource_buffer(
                                         b, intrin->num_components, intrin->def.bit_size,
                                         nir_intrinsic_access(intrin), resource_index_base,
                                         binding.array_index, 0, intrin->src[1].ssa));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_store_ssbo(nir_builder * const b,
                                            nir_intrinsic_instr * const intrin,
                                            struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_store_ssbo);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   unsigned const bytes_per_component = nir_src_bit_size(intrin->src[0]) / 8;
   unsigned uav_op;
   if (bytes_per_component == 4) {
      uav_op = V_RAT_INST_STORE_TYPED;
   } else {
      assert(!"Unsupported storage buffer component size");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   /* Robustness: the effective per-stage flag is threaded via
    * state->robust_buffer_access (computed by the pipeline compiler from
    * device feature + VK_EXT_pipeline_robustness).  See the mandatory
    * ALU-clamp rationale in terakan_nir_buffer_uav_coord(). */
   nir_def * coord;
   if (b->shader->info.stage == MESA_SHADER_COMPUTE) {
      /* Compute store_ssbo via store_global + KCACHE base address.
       *
       * terakan_emit_compute_kcache programs KC0[0].x with the SSBO base
       * byte address. We load it via KCACHE, add the store offset, and emit
       * store_global. SFN lowers this to MEM_RAT_CACHELESS STORE_RAW. */

      nir_def *ssbo_base = nir_load_kcache_r600(
         b, 1, 32, nir_imm_zero(b, 1, 32),
         .access = ACCESS_CAN_REORDER,
         .id_base = 0,
         .base = 0,
         .component = 0);

      nir_def *global_addr = nir_iadd(b, ssbo_base, intrin->src[2].ssa);

      nir_store_global(b, nir_channel(b, intrin->src[0].ssa, 0), global_addr,
                       .write_mask = nir_intrinsic_write_mask(intrin),
                       .access = nir_intrinsic_access(intrin));

      nir_instr_remove(&intrin->instr);
      return;
   } else {
      coord = terakan_nir_buffer_uav_coord(
         b, intrin->src[2].ssa, uav_index_zero_based, uav_array_index,
         state->robust_buffer_access,
         (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   }
   if (bytes_per_component > 1) {
      coord = nir_udiv_imm(b, coord, bytes_per_component);
   }

   /* No point in vectorizing, the hardware instruction stores only one channel. */
   assert(nir_intrinsic_write_mask(intrin) == 0b1);

   /* Write guard: MEM_RAT stores are NOT bounds-checked by Evergreen
    * hardware (Phase 5 Probe 7).  When robust_buffer_access is enabled,
    * emit an IF/ENDIF that suppresses the write when the store offset
    * exceeds the UAV's declared byte size. */
   bool const guarded = state->robust_buffer_access;
   if (guarded) {
      nir_def *in_bounds = terakan_nir_emit_write_guard(
         b, intrin->src[2].ssa, bytes_per_component,
         uav_index_zero_based, uav_array_index, state);
      nir_push_if(b, in_bounds);
   }

   terakan_nir_build_uav_instr_r600(
      b, uav_array_index, coord, nir_u2u32(b, nir_channel(b, intrin->src[0].ssa, 0)),
      nir_undef(b, 1, 32), uav_op, access,
      state->uav_base + uav_index_zero_based);

   if (guarded) {
      nir_pop_if(b, NULL);
   }

   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_ssbo_atomic(nir_builder * const b,
                                             nir_intrinsic_instr * const intrin,
                                             struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_ssbo_atomic ||
          intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   /* Robustness threaded via state->robust_buffer_access (see above). */
   nir_def * coord =
      nir_udiv_imm(b,
                   terakan_nir_buffer_uav_coord(
                      b, intrin->src[1].ssa, uav_index_zero_based, uav_array_index,
                      state->robust_buffer_access,
                      (access & ACCESS_INCLUDE_HELPERS) != 0, state),
                   4);

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[2].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_ssbo_atomic_swap
                                      ? intrin->src[3].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   /* Write guard: MEM_RAT atomics are NOT bounds-checked by Evergreen
    * hardware.  Wrap the atomic in IF/ENDIF (or IF/ELSE for returning
    * atomics that need zero on the OOB path). */
   bool const guarded = state->robust_buffer_access;
   if (guarded) {
      nir_def *in_bounds = terakan_nir_emit_write_guard(
         b, intrin->src[1].ssa, 4 /* atomic is always 4 bytes */,
         uav_index_zero_based, uav_array_index, state);
      nir_push_if(b, in_bounds);
   }

   if (result_used) {
      /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
      nir_def *atomic_result = nir_u2uN(
            b,
            terakan_nir_build_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               uav_op, access, uav_id_base,
               (b->shader->info.stage == MESA_SHADER_FRAGMENT
                   ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                   : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
               uav_index_zero_based),
            intrin->def.bit_size);
      if (guarded) {
         /* OOB path: return zero for the atomic result. */
         nir_pop_if(b, NULL);
         nir_def *zero = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
         nir_def *result = nir_if_phi(b, atomic_result, zero);
         nir_def_rewrite_uses(&intrin->def, result);
      } else {
         nir_def_rewrite_uses(&intrin->def, atomic_result);
      }
   } else {
      terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, value, compare_value, uav_op,
                                       access, uav_id_base);
      if (guarded) {
         nir_pop_if(b, NULL);
      }
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_load(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_load);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   mesa_shader_stage const stage = b->shader->info.stage;

   enum gl_access_qualifier access = nir_intrinsic_access(intrin);
   if (state->uavs_for_mutable_resources_needed != NULL) {
      /* Need write-read coherence within an invocation. */
      /* TODO(Triang3l): Detect whether write-read coherence is needed more precisely. */
      access &= ~ACCESS_CAN_REORDER;
   } else {
      access |= ACCESS_CAN_REORDER;
   }

   uint8_t const resource_index_base = binding.set->first_shader_resources[stage] +
                                       binding.set_binding->first_shader_resources[stage];

   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      /* Vertex fetches are coherent with UAVs, do a vertex fetch unconditionally. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(b,
                  nir_load_buffer_resource_r600(
                     b, intrin->def.num_components, 32,
                     binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
                     nir_channel(b, intrin->src[1].ssa, 0), .access = access,
                     .id_base = resource_index_base),
                  intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   /* Texture fetches are not coherent with UAVs, if write-read coherence is needed, load using a
    * NOP_RTN UAV operation.
    */

   bool const image_is_array = nir_intrinsic_image_array(intrin);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based =
      terakan_nir_get_binding_uav(&binding, true, state, stage, &apply_uav_array_index);

   if (uav_index_zero_based == UINT_MAX) {
      /* UAV not needed or not available at this stage, load via the texture cache. */
      BITSET_SET_RANGE(state->resources_needed,
                       resource_index_base + binding.array_index_range_first,
                       resource_index_base + binding.array_index_range_last);
      nir_def_rewrite_uses(
         &intrin->def,
         nir_u2uN(
            b,
            nir_load_texture_resource_r600(
               b, intrin->def.num_components, 32,
               binding.array_index != NULL ? binding.array_index : nir_imm_zero(b, 1, 32),
               nir_vec4(
                  b, nir_channel(b, intrin->src[1].ssa, 0),
                  image_dim != GLSL_SAMPLER_DIM_1D ? nir_channel(b, intrin->src[1].ssa, 1)
                                                   : nir_imm_zero(b, 1, 32),
                  image_dim == GLSL_SAMPLER_DIM_3D || image_is_array
                     ? nir_channel(b, intrin->src[1].ssa, image_dim == GLSL_SAMPLER_DIM_1D ? 1 : 2)
                     : nir_imm_zero(b, 1, 32),
                  nir_imm_zero(b, 1, 32)),
               .access = access, .id_base = resource_index_base),
            intrin->def.bit_size));
      nir_instr_remove(&intrin->instr);
      return;
   }

   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);
   nir_def * const immed_index =
      terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                   struct terakan_physical_device const, vk)
                                         ->chip_info);
   nir_def * const undef = nir_undef(b, 1, 32);
   /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
   nir_def_rewrite_uses(
      &intrin->def,
      nir_u2uN(
         b,
         terakan_nir_build_uav_returning_instr_r600(
            b, intrin->def.num_components, 32, uav_array_index,
            terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim, image_is_array), undef,
            undef, immed_index, V_RAT_INST_NOP_RTN, access,
            state->uav_base + uav_index_zero_based,
            (b->shader->info.stage == MESA_SHADER_FRAGMENT
                ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
            uav_index_zero_based),
         intrin->def.bit_size));
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_store(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_store);

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, false, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin);

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin));
   }

   nir_def * const undef = nir_undef(b, 1, 32);

   /* TODO(Triang3l): Proper bit size conversion depending on the source type? */
   terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, nir_u2u32(b, intrin->src[3].ssa),
                                    undef, V_RAT_INST_STORE_TYPED, access,
                                    state->uav_base + uav_index_zero_based);
   nir_instr_remove(&intrin->instr);
}

static void
terakan_nir_lower_bindings_instr_image_deref_atomic(
   nir_builder * const b, nir_intrinsic_instr * const intrin,
   struct terakan_nir_lower_bindings_state * const state)
{
   assert(intrin->intrinsic == nir_intrinsic_image_deref_atomic ||
          intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap);

   if (unlikely(intrin->def.bit_size != 32)) {
      assert(!"Only 32-bit storage image and storage texel buffer atomic operations are supported");
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum glsl_sampler_dim const image_dim = nir_intrinsic_image_dim(intrin);

   struct terakan_nir_binding binding;
   if (unlikely(!terakan_nir_get_binding(intrin->src[0],
                                         terakan_nir_image_descriptor_type(image_dim),
                                         state->layout, b->shader, &binding))) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   b->cursor = nir_before_instr(&intrin->instr);

   bool const result_used = !list_is_empty(&intrin->def.uses);

   bool apply_uav_array_index;
   unsigned const uav_index_zero_based = terakan_nir_get_binding_uav(
      &binding, result_used, state, b->shader->info.stage, &apply_uav_array_index);
   if (unlikely(uav_index_zero_based == UINT_MAX)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }
   nir_def * const uav_array_index =
      apply_uav_array_index ? binding.array_index : nir_imm_zero(b, 1, 32);

   unsigned const uav_op = terakan_nir_atomic_uav_op(nir_intrinsic_atomic_op(intrin), result_used);
   if (unlikely(uav_op == 0)) {
      terakan_nir_lower_bindings_instr_to_null(&intrin->instr);
      return;
   }

   enum gl_access_qualifier const access = nir_intrinsic_access(intrin) & ~ACCESS_CAN_REORDER;

   nir_def * coord;
   if (image_dim == GLSL_SAMPLER_DIM_BUF) {
      coord = terakan_nir_buffer_uav_coord(b, nir_channel(b, intrin->src[1].ssa, 0),
                                           uav_index_zero_based, uav_array_index, true,
                                           (access & ACCESS_INCLUDE_HELPERS) != 0, state);
   } else {
      coord = terakan_nir_image_uav_coord(b, intrin->src[1].ssa, image_dim,
                                          nir_intrinsic_image_array(intrin));
   }

   /* For INC/DEC, the hardware instruction accepts the maximum possible value. */
   unsigned const uav_op_non_rtn = uav_op & 0x1F;
   nir_def * const value =
      uav_op_non_rtn == V_RAT_INST_INC_UINT || uav_op_non_rtn == V_RAT_INST_DEC_UINT
         ? nir_imm_int(b, (int)UINT32_MAX)
         : intrin->src[3].ssa;

   nir_def * const compare_value = intrin->intrinsic == nir_intrinsic_image_deref_atomic_swap
                                      ? intrin->src[4].ssa
                                      : nir_undef(b, 1, 32);

   unsigned const uav_id_base = state->uav_base + uav_index_zero_based;

   /* Write guard: MEM_RAT atomics are NOT bounds-checked by Evergreen
    * hardware.  Wrap the atomic in IF/ENDIF (or IF/ELSE for returning
    * atomics that need zero on the OOB path). */
   bool const guarded = state->robust_buffer_access;
   if (guarded) {
      nir_def *in_bounds = terakan_nir_emit_write_guard(
         b, intrin->src[1].ssa, 4 /* atomic is always 4 bytes */,
         uav_index_zero_based, uav_array_index, state);
      nir_push_if(b, in_bounds);
   }

   if (result_used) {
      /* TODO(Triang3l): Proper bit size conversion depending on the destination type? */
      nir_def *atomic_result = nir_u2uN(
            b,
            terakan_nir_build_uav_returning_instr_r600(
               b, intrin->def.num_components, 32, uav_array_index, coord, value, compare_value,
               terakan_nir_uav_immed_index(b, &container_of(state->layout->vk.base.device->physical,
                                                            struct terakan_physical_device const, vk)
                                                  ->chip_info),
               uav_op, access, uav_id_base,
               (b->shader->info.stage == MESA_SHADER_FRAGMENT
                   ? TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_PIXEL
                   : TERAKAN_RESOURCE_RANGE_UAV_IMMEDIATE_BASE_COMPUTE) +
               uav_index_zero_based),
            intrin->def.bit_size);
      if (guarded) {
         /* OOB path: return zero for the atomic result. */
         nir_pop_if(b, NULL);
         nir_def *zero = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
         nir_def *result = nir_if_phi(b, atomic_result, zero);
         nir_def_rewrite_uses(&intrin->def, result);
      } else {
         nir_def_rewrite_uses(&intrin->def, atomic_result);
      }
   } else {
      terakan_nir_build_uav_instr_r600(b, uav_array_index, coord, value, compare_value, uav_op,
                                       access, uav_id_base);
      if (guarded) {
         nir_pop_if(b, NULL);
      }
      nir_def_rewrite_uses(&intrin->def,
                           nir_undef(b, intrin->def.num_components, intrin->def.bit_size));
   }
   nir_instr_remove(&intrin->instr);
}

bool
terakan_nir_lower_bindings_instr(nir_builder * const b, nir_instr * const instr,
                                 void * const cb_data)
{
   struct terakan_nir_lower_bindings_state * const state =
      (struct terakan_nir_lower_bindings_state *)cb_data;

   if (instr->type == nir_instr_type_tex) {
      return terakan_nir_lower_bindings_instr_tex(b, nir_instr_as_tex(instr), state);
   }

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_num_workgroups:
         if (b->shader->info.stage == MESA_SHADER_COMPUTE) {
            terakan_nir_lower_bindings_instr_load_num_workgroups(b, intrin, state);
            return true;
         }
         return false;
      case nir_intrinsic_load_ubo:
         terakan_nir_lower_bindings_instr_load_ubo(b, intrin, state);
         return true;
      case nir_intrinsic_load_push_constant:
         terakan_nir_lower_bindings_instr_load_push_constant(b, intrin, state);
         return true;
      case nir_intrinsic_load_ssbo:
         terakan_nir_lower_bindings_instr_load_ssbo(b, intrin, state);
         return true;
      case nir_intrinsic_store_ssbo:
         terakan_nir_lower_bindings_instr_store_ssbo(b, intrin, state);
         return true;
      case nir_intrinsic_ssbo_atomic:
      case nir_intrinsic_ssbo_atomic_swap:
         terakan_nir_lower_bindings_instr_ssbo_atomic(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_load:
         terakan_nir_lower_bindings_instr_image_deref_load(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_store:
         terakan_nir_lower_bindings_instr_image_deref_store(b, intrin, state);
         return true;
      case nir_intrinsic_image_deref_atomic:
      case nir_intrinsic_image_deref_atomic_swap:
         terakan_nir_lower_bindings_instr_image_deref_atomic(b, intrin, state);
         return true;
      default:
         break;
      }
   }

   return false;
}
