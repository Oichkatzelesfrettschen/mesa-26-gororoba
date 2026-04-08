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
 * terakan_nir_lower_bindings.c — Thin orchestrator
 *
 * Entry point for NIR binding lowering.  Calls apply_pipeline_layout
 * (descriptor resolution) then lower_abi (instruction emission) in
 * strict sequence.
 *
 * Phase 0 mechanical file split.  See TERAKAN_SHADER_ABI_CONTRACT.md.
 * The true two-pass phase boundary (where lower_abi stops calling
 * apply_layout helpers directly) is a future Phase 1 concern.
 */

#include "terakan_nir_lower_bindings_internal.h"

bool
terakan_nir_lower_bindings(nir_shader * const shader,
                           struct terakan_pipeline_layout const * const layout,
                           BITSET_WORD * const resources_needed_accum,
                           uint32_t * const samplers_needed_accum, unsigned const uav_base,
                           BITSET_WORD * const uavs_for_mutable_resources_needed_out_opt,
                           uint32_t * const driver_push_constants_used_accum,
                           uint16_t * const kcache_needed_accum_out,
                           bool const robust_buffer_access)
{
   uint16_t kcache_needed_accum = 0;
   bool progress = false;

   /* TODO(Triang3l): Lower 64-bit buffer access. */

   /* Lower load_vulkan_descriptor and vulkan_resource_reindex chains to vulkan_resource_index. */
   progress |=
      nir_shader_lower_instructions(shader, terakan_nir_lower_load_vulkan_descriptor_filter,
                                    terakan_nir_lower_load_vulkan_descriptor_impl, NULL);
   progress |=
      nir_shader_instructions_pass(shader, terakan_nir_lower_vulkan_resource_reindex_instr,
                                   nir_metadata_block_index | nir_metadata_dominance, NULL);

   /* Make data offsets in buffers relative to 0. */
   progress |= nir_shader_lower_instructions(shader, terakan_nir_zero_vulkan_resource_offset_filter,
                                             terakan_nir_zero_vulkan_resource_offset_impl, NULL);

   /* Make sure resource indices and data offsets are known to be constant if they are. */
   bool constant_folding_progress;
   do {
      constant_folding_progress = false;
      bool copy_prop_progress = false;
      NIR_PASS(copy_prop_progress, shader, nir_opt_copy_prop);
      if (copy_prop_progress) {
         constant_folding_progress = true;
         /* Cleanup to prevent the same propagations from happening infinitely. */
         NIR_PASS(constant_folding_progress, shader, nir_opt_dce);
      }
      NIR_PASS(constant_folding_progress, shader, nir_opt_constant_folding);
   } while (constant_folding_progress);

   /* Apply the pipeline layout and perform various lowerings based on it. */

   assert(uav_base <= TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT);

   struct terakan_nir_lower_bindings_state state = {
      .layout = layout,
      .resources_needed = resources_needed_accum,
      .samplers_needed = samplers_needed_accum,
      .uav_base = uav_base,
      .driver_push_constants_used = driver_push_constants_used_accum,
      .kcache_needed = &kcache_needed_accum,
      .robust_buffer_access = robust_buffer_access,
   };

   if (uavs_for_mutable_resources_needed_out_opt != NULL) {
      if (shader->info.stage == MESA_SHADER_FRAGMENT) {
         memset(uavs_for_mutable_resources_needed_out_opt, 0,
                sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_PIXEL));
         if (layout->vk.base.device->enabled_features.fragmentStoresAndAtomics) {
            state.uavs_for_mutable_resources_needed = uavs_for_mutable_resources_needed_out_opt;
         }
      } else {
         memset(
            uavs_for_mutable_resources_needed_out_opt, 0,
            sizeof(BITSET_WORD) * BITSET_WORDS(TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL));
         if (shader->info.stage == MESA_SHADER_COMPUTE) {
            state.uavs_for_mutable_resources_needed = uavs_for_mutable_resources_needed_out_opt;
         }
      }
   }

   terakan_nir_gather_uavs_needed(shader, &state);

   progress |= nir_shader_instructions_pass(shader, terakan_nir_lower_bindings_instr,
                                            nir_metadata_none, &state);

   nir_shader_preserve_all_metadata(shader);
   *kcache_needed_accum_out = kcache_needed_accum;
   return progress;
}

