/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
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

#include "terakan_shader.h"

#include "nir/terakan_nir.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_descriptor.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"

#include "amd/terascale/common/terascale_wddm.h"
#include "compiler/glsl_types.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "gallium/drivers/r600/sfn/sfn_nir_lower_tex.h"
#include "spirv/nir_spirv.h"
#include "util/bitscan.h"
#include "util/macros.h"
#include "vk_nir.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

struct terakan_shader_ring const terakan_shader_rings[TERAKAN_SHADER_RING_INDEX_COUNT] = {
   [TERAKAN_SHADER_RING_INDEX_LSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_LSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E10_SQ_LSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028830_SQ_LSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_HSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_HSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008E18_SQ_HSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028834_SQ_HSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_ESTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_ESTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C50_SQ_ESTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028908_SQ_ESTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_GSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_GSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C58_SQ_GSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_02890C_SQ_GSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_VSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_VSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C60_SQ_VSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028910_SQ_VSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
   [TERAKAN_SHADER_RING_INDEX_PSTMP] =
      {
         .base_wddm_patch_ids = TERASCALE_WDDM_PATCH_IDS_SQ_PSTMP_RING_BASE,
         .base_size_config_reg_offset = TERAKAN_CONFIG_REG_OFFSET(R_008C68_SQ_PSTMP_RING_BASE),
         .item_size_context_reg_offset =
            TERAKAN_CONTEXT_REG_OFFSET(R_028914_SQ_PSTMP_RING_ITEMSIZE),
         .stages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         .sx_surface_sync_mask = (uint32_t)1 << 4,
      },
};

/* From gl_nir_linker.c. */
static void
terakan_nir_shared_type_info(struct glsl_type const * const type, unsigned * const size,
                             unsigned * const align)
{
   assert(glsl_type_is_vector_or_scalar(type));
   uint32_t const comp_size = glsl_type_is_boolean(type) ? 4 : glsl_get_bit_size(type) / 8;
   unsigned const length = glsl_get_vector_elements(type);
   *size = comp_size * length;
   *align = comp_size * (length == 3 ? 4 : length);
}

/* Forward declaration for tranche option 2 -- the wide-phi defs rewriter is
 * defined far below but called from terakan_shader_spirv_to_nir when the
 * EARLIER env is set. */
static bool
terakan_segment_wide_phi_defs(nir_shader * nir, unsigned segment_size,
                              unsigned min_value_count,
                              char const *log_label);

nir_shader *
terakan_shader_spirv_to_nir(struct terakan_device * const device, size_t const spirv_size_bytes,
                            uint32_t const * const spirv, mesa_shader_stage const stage,
                            char const * const entrypoint,
                            VkSpecializationInfo const * const specialization_info)
{
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   static struct spirv_to_nir_options const spirv_options = {
      .environment = NIR_SPIRV_VULKAN,

      /* TODO(Triang3l): Possibly the subgroup size is properly
       * exposed.
       */

      /* TODO(Triang3l): Capabilities when supported and tested. */

      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .shared_addr_format = nir_address_format_32bit_offset,

      .min_ubo_alignment = TERAKAN_KCACHE_HW_LINE_BYTES,
      .min_ssbo_alignment = sizeof(uint32_t),
   };

   nir_shader * nir =
      vk_spirv_to_nir(&device->vk, spirv, spirv_size_bytes, stage, entrypoint,
                      specialization_info, &spirv_options,
                      stage == MESA_SHADER_FRAGMENT ? &physical_device->nir_options_fs
                                                    : &physical_device->nir_options_non_fs,
                      false, NULL);

   if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
      fprintf(stderr, "TERAKAN_NIR_SPIRV: --- post vk_spirv_to_nir (%s) ---\n",
              mesa_shader_stage_name(nir->info.stage));
      nir_print_shader(nir, stderr);
   }

   /* Tranche option 2: env-gated EARLIER call to the wide-phi segmenter, fired
    * immediately after vk_spirv_to_nir before any other pass observes the
    * chain.  Separate env from the existing post-link variant so A/B testing
    * can isolate where the rewrite needs to fire. */
   {
      unsigned earlier_segment_size = 0;
      char const * const earlier_env_value =
         getenv("TERAKAN_EXPERIMENTAL_EARLIER_WIDE_PHI_SEGMENT");
      if (earlier_env_value != NULL && earlier_env_value[0] != '\0') {
         char *end = NULL;
         unsigned long parsed = strtoul(earlier_env_value, &end, 0);
         if (end != earlier_env_value && end[0] == '\0' && parsed >= 2 &&
             parsed < UINT_MAX) {
            earlier_segment_size = (unsigned)parsed;
         } else {
            earlier_segment_size = 64;
         }
      }
      if (earlier_segment_size != 0) {
         unsigned const earlier_min_cases =
            earlier_segment_size < UINT_MAX ? earlier_segment_size + 1 : UINT_MAX;
         if (terakan_segment_wide_phi_defs(
                nir, earlier_segment_size, earlier_min_cases,
                "TERAKAN_EXPERIMENTAL_EARLIER_WIDE_PHI_SEGMENT")) {
            bool cleanup_progress;
            do {
               cleanup_progress = false;
               NIR_PASS(cleanup_progress, nir, nir_opt_copy_prop);
               NIR_PASS(cleanup_progress, nir, nir_opt_dce);
               NIR_PASS(cleanup_progress, nir, nir_opt_remove_phis);
               NIR_PASS(cleanup_progress, nir, nir_opt_dead_cf);
            } while (cleanup_progress);
         }
         if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
            fprintf(stderr,
                    "TERAKAN_NIR_SPIRV: --- post TERAKAN_EXPERIMENTAL_EARLIER_WIDE_PHI_SEGMENT=%u (%s) ---\n",
                    earlier_segment_size, mesa_shader_stage_name(nir->info.stage));
            nir_print_shader(nir, stderr);
         }
      }
   }

   /* SFN expects certain fragment shader system values to be accessed via load_input rather than
    * the system value load intrinsics, make sure that's the case before nir_lower_system_values is
    * done that would otherwise generate system value load intrinsics.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct nir_lower_sysvals_to_varyings_options const lower_sysvals_to_varyings_options = {
         .frag_coord = true,
         .front_face = true,
         .point_coord = true,
      };
      NIR_PASS(_, nir, nir_lower_sysvals_to_varyings, &lower_sysvals_to_varyings_options);
   }

   /* Assign meanings and indices to variables in cases that don't depend on the actual executable
    * code once all variables are set up (including via nir_lower_sysvals_to_varyings).
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      if (nir->info.stage == MESA_SHADER_VERTEX) {
         nir_foreach_shader_in_variable (var, nir) {
            assert(var->data.location >= VERT_ATTRIB_GENERIC0);
            var->data.driver_location = var->data.location - VERT_ATTRIB_GENERIC0;
         }
      } else {
         nir_assign_io_var_locations(nir, nir_var_shader_in);
      }
      /* Fragment shader outputs are compacted in the end, not assigning locations here. */
      if (nir->info.stage != MESA_SHADER_FRAGMENT) {
         nir_assign_io_var_locations(nir, nir_var_shader_out);
      }
   }

   /* Make sure output writes are done only once, so they can be treated as exports, and also make
    * fragment inputs interpolated once.
    */

   if (nir->info.stage != MESA_SHADER_COMPUTE) {
      nir_lower_io_vars_to_temporaries(nir, nir_shader_get_entrypoint(nir), nir_var_shader_out);
   }

   /* Lower interface and binding derefs. */

   NIR_PASS(_, nir, nir_lower_system_values);
   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      nir_lower_compute_system_values_options const lower_compute_system_values_options = {
         .global_id_is_32bit = true,
      };
      NIR_PASS(_, nir, nir_lower_compute_system_values, &lower_compute_system_values_options);
   }

   if (nir->info.stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               terakan_nir_shared_type_info);
   }

   assert(spirv_options.ubo_addr_format == nir_address_format_32bit_index_offset);
   assert(spirv_options.ssbo_addr_format == nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);

   assert(spirv_options.push_const_addr_format == nir_address_format_32bit_offset);
   assert(spirv_options.shared_addr_format == nir_address_format_32bit_offset);
   NIR_PASS(
      _, nir, nir_lower_explicit_io,
      nir_var_mem_push_const | (nir->info.stage == MESA_SHADER_COMPUTE ? nir_var_mem_shared : 0),
      nir_address_format_32bit_offset);

   /* Lower basic instructions that won't be generated by other lowerings. */

   NIR_PASS(_, nir, terakan_nir_lower_sin_cos);

   /* Lower BASIC subgroup primitives before the debug NIR dump.
    *
    * Run nir_lower_subgroups with the same hardware model used by r600
    * Gallium in sfn_nir.cpp. Wave size is 64 on Evergreen/Bobcat; ballot is
    * stored as two 32-bit components.
    *
    * The enabled lowerings cover the VK_SUBGROUP_FEATURE_BASIC_BIT
    * surface advertised by Terakan:
    *   - lower_elect emits gl_SubgroupInvocationID == 0
    *   - lower_vote_trivial folds single-lane vote to source
    *   - lower_relative_shuffle maps XOR/up/down shuffles to absolute
    *   - lower_quad_broadcast_dynamic handles dynamic quad broadcast
    *   - lower_inverse_ballot maps inverse ballot to standard ballot
    *
    * Higher subgroup tiers require LDS emulation. BASIC primitives lower
    * to single-lane or barrier-equivalent sequences without LDS, so the
    * BASIC bit advertisement matches the generated code.
    *
    * subgroupBarrier lowers to a standard compute barrier; wave64 is the
    * natural lockstep granule on Evergreen SIMD, and the compute barrier
    * path handles the cross-wave case.
    */
   {
      static const nir_lower_subgroups_options terakan_subgroups_options = {
         /* Explicit subgroup_size lets NIR fold load_subgroup_id,
          * load_subgroup_size, and load_num_subgroups to constants for the
          * common Bobcat case where the workgroup fits in one wave. */
         .subgroup_size = 64,
         .ballot_bit_size = 32,
         .ballot_components = 2,  /* 2 x uint32 = 64-bit ballot for wave64 */
         .lower_vote_trivial = true,
         .lower_relative_shuffle = true,
         .lower_quad_broadcast_dynamic = true,
         .lower_elect = true,
         .lower_inverse_ballot = true,
         .lower_subgroup_masks = true,
         /* Decompose vote_any / vote_all / vote_*_eq into ballot + ALU.
          * build_vote() in nir_lower_subgroups emits a single
          * nir_ballot followed by integer ALU on the uvec2 result; the
          * terakan LDS pass below expands the ballot into a single
          * LDS_ATOMIC_OR_RET + one workgroup barrier (the
          * single-barrier shape introduced by this commit, replacing
          * the earlier three-barrier form that exhausted the SFN
          * VLIW5 scheduler's GROUP_BARRIER slot pinning).  No
          * shuffle / reduce / read_invocation lowering is enabled
          * yet -- those expand to repeated read_invocation calls that
          * would re-introduce the GROUP_BARRIER pressure the
          * single-barrier ballot was designed to avoid. */
         .lower_vote = true,
         .lower_vote_feq = true,
         .lower_vote_ieq = true,
         .lower_vote_bool_eq = true,
         /* Reduce / boolean_reduce decompose subgroupAdd / Min / Max /
          * And / Or / Xor / AllEqual / etc. into nir_intrinsic_reduce
          * with a nir_op operator argument; the LDS pass then expands
          * each reduce into a 6-level butterfly LDS exchange (see
          * emit_reduce_via_butterfly in terakan_nir_lower_subgroup_lds).
          * Bounded GROUP_BARRIER cost (6 barriers per reduce
          * regardless of input), so the SFN VLIW5 scheduler packs
          * them without slot-pinning crisis. */
         .lower_reduce = true,
         .lower_boolean_reduce = true,
      };
      NIR_PASS(_, nir, nir_lower_subgroups, &terakan_subgroups_options);
      /* Expand the foundational ballot + read_first_invocation
       * primitives that nir_lower_subgroups left behind into LDS
       * sequences (per-call-site allocation, single LDS_ATOMIC_OR_RET,
       * single workgroup barrier).  Compute / kernel stages only. */
      NIR_PASS(_, nir, terakan_nir_lower_subgroup_lds);
   }
   if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
      fprintf(stderr, "TERAKAN_NIR_SPIRV: --- post explicit_io/lowerings (%s) ---\n",
              mesa_shader_stage_name(nir->info.stage));
      nir_print_shader(nir, stderr);
   }

   return nir;
}

/* Memory-access size/align callback for nir_lower_mem_access_bit_sizes.
 *
 * Evergreen MEM_RAT_STORE_TYPED writes one dword per element on the
 * cached path; STORE_RAW writes one dword for cached targets, up to
 * two for CACHELESS.  Sub-32-bit SSBO/global stores therefore must be
 * widened to 32-bit, so this callback returns bit_size=32 for the
 * modes given to the pass.
 *
 * UBO/push-const are not in the pass modes here: UBO goes through
 * KCACHE and push-const lowering is separate.
 *
 * Shift method is `scalar` because the cached MEM_RAT_CMPXCHG_INT
 * path silently no-ops on the validated PALM/SUMO Evergreen
 * TeraScale-2 target.  ADD/XCHG/INC/DEC at the same address succeed,
 * but CMPXCHG with a matching compare leaves memory unchanged across
 * payload-lane permutations.  Do not generalize that CMPXCHG failure
 * to every r600-family GPU without chip-specific probes.
 *
 * For dword-aligned sub-32-bit stores the upstream pass widens via a
 * generated atomic_swap (cmpxchg-shape) RMW; the downstream
 * terakan_nir_lower_cmpxchg_to_speculative_xchg lowering rewrites
 * those to load+bcsel+atomic_xchg, which uses the working XCHG path.
 * Single-thread test patterns are correct; cross-thread sub-dword
 * racing SSBO writes remain a documented silicon gap.
 */
static nir_mem_access_size_align
terakan_nir_mem_access_size_align(nir_intrinsic_op intrin, uint8_t bytes,
                                  uint8_t bit_size, uint32_t align_mul,
                                  uint32_t align_offset, bool offset_is_const,
                                  enum gl_access_qualifier access,
                                  const void *cb_data)
{
   (void)intrin;
   (void)align_mul;
   (void)align_offset;
   (void)offset_is_const;
   (void)access;
   (void)cb_data;

   /* Widen all sub-32-bit accesses to 32-bit.  Cap at 32-bit because the
    * MEM_RAT_STORE_TYPED data path is one dword per element. */
   if (bit_size < 32) {
      bit_size = 32;
   } else if (bit_size > 32) {
      bit_size = 32;
   }

   /* num_components: as many as we can fit in `bytes`, capped at 4
    * (the vec4 channel limit of MEM_RAT_STORE_TYPED). */
   uint8_t num_components = MAX2(1, MIN2(bytes / (bit_size / 8), 4));

   return (nir_mem_access_size_align){
      .num_components = num_components,
      .bit_size = bit_size,
      .align = bit_size / 8,
      .shift = nir_mem_access_shift_method_scalar,
   };
}

/* `data` points to `nir_variable_mode robust_modes`. */
static bool
terakan_nir_should_vectorize_load_store(unsigned const align_mul, unsigned const align_offset,
                                        unsigned const bit_size, unsigned const num_components,
                                        int64_t hole_size, nir_intrinsic_instr * const low,
                                        nir_intrinsic_instr * const high, void * const data)
{
   /* Don't vectorize kcache loads — vectorizing breaks bank locality and
    * wastes ALU clause capacity.  Also reject mixed kcache + resource loads
    * (the constant address of `high` may exceed the kcache buffer window). */
   if (low->intrinsic == nir_intrinsic_load_kcache_r600 ||
       high->intrinsic == nir_intrinsic_load_kcache_r600 ||
       low->intrinsic != high->intrinsic) {
      return false;
   }

   if (low->intrinsic == nir_intrinsic_store_ssbo) {
      /* Storage buffer UAVs always use TERASCALE_FORMAT_INDEX_32 or STORE_BYTE / STORE_SHORT. */
      return false;
   }

   /* Vectorization of buffer resource fetches. */

   if (num_components > 4 || hole_size != 0) {
      return false;
   }

   unsigned const vector_bytes = bit_size / 8u * num_components;

   if (bit_size < 32) {
      /* According to testing on Barts, 8_8_8 and 16_16_16 buffer fetches return completely invalid
       * values.
       */
      if (num_components == 3) {
         return false;
      }

      /* According to testing on Barts, the hardware implicitly rounds the address down to the
       * alignment requirement, which is min(bytes per element, 4), matching the alignment
       * restriction described in "4.4.6 Element Alignment" of the Direct3D 11.3 Functional
       * Specification.
       */
      if (nir_combined_align(align_mul, align_offset) < MIN2(vector_bytes, 4u)) {
         return false;
      }
   }

   /* Width-promoted alignment guard.
    *
    * The standard NIR vectorizer can also produce a "wider type" merge,
    * promoting e.g. two sub-32-bit element loads at byte offsets 6 and 8
    * into a single bit_size=32 load at offset 6.  The address alignment
    * of the original sub-32-bit elements (2-byte for u16, 1-byte for u8)
    * is what bounds the safe VFETCH width on r600/Evergreen, not the
    * post-merge bit_size.
    *
    * Detect width-promotion by comparing the input intrinsic's element
    * bit_size against the callback's proposed bit_size.  When the
    * promotion crosses the 4-byte VFETCH-alignment boundary
    * (combined_align < min(vector_bytes, 4)), refuse the merge so the
    * loads stay narrow and stay aligned.
    *
    * For straight same-bit_size merges (e.g., two u32 -> uvec2), align_mul
    * may legitimately be 0 ("no hint") -- the original alignment check
    * stays gated by `bit_size < 32` so it does not refuse legitimate
    * 32-bit merges when align hints are absent.
    *
    * Cite: D3D 11.3 section 4.4.6 Element Alignment; Barts/Cypress/Palm
    * vfetch-alignment behavior (same source as the preceding gate).
    */
   {
      unsigned const low_elem_bits = low->def.bit_size;
      if (low_elem_bits < bit_size && low_elem_bits < 32u) {
         if (nir_combined_align(align_mul, align_offset) < MIN2(vector_bytes, 4u)) {
            return false;
         }
      }
   }

   /* According to testing on Barts, for elements larger than 4 bytes, bounds checking only
    * considers the first 4 bytes, and if they are within the buffer size, the entire element is
    * fetched. This may result in data outside the memory range bound to the buffer being loaded,
    * which is not allowed with robustBufferAccess, so vectorize beyond 4 bytes only if it can be
    * assumed that out-of-bounds access must not happen.
    */
   if (vector_bytes > 4) {
      nir_variable_mode const robust_modes = *(nir_variable_mode const *)data;
      switch (low->intrinsic) {
      case nir_intrinsic_load_ubo:
         if (robust_modes & nir_var_mem_ubo) {
            return false;
         }
         break;
      case nir_intrinsic_load_ssbo:
         if (robust_modes & nir_var_mem_ssbo) {
            return false;
         }
         break;
      default:
         break;
      }
   }

   return true;
}

static unsigned
terakan_lower_bit_size_callback(const nir_instr *instr, void *UNUSED data)
{
   if (instr->type != nir_instr_type_alu)
      return 0;

   const nir_alu_instr *alu = nir_instr_as_alu(instr);

   /* Promote ALU ops that truly operate on sub-32-bit sources.
    * Keep 32->8/16 conversion ops out of nir_lower_bit_size so they
    * continue through explicit backend conversion handlers. */
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      unsigned src_bit_size = nir_src_bit_size(alu->src[i].src);
      if (src_bit_size > 1 && src_bit_size < 32)
         return 32;
   }

   return 0;
}

static bool
terakan_get_experimental_wide_phi_select_limit(unsigned * const limit_out)
{
   char const * const limit_string =
      getenv("TERAKAN_EXPERIMENTAL_WIDE_PHI_SELECT_LIMIT");
   if (limit_string == NULL || limit_string[0] == '\0')
      return false;

   errno = 0;
   char *limit_end = NULL;
   unsigned long const parsed_limit = strtoul(limit_string, &limit_end, 0);
   if (errno != 0 || limit_end == limit_string || limit_end[0] != '\0') {
      *limit_out = UINT_MAX - 1;
      return true;
   }

   *limit_out = parsed_limit >= UINT_MAX ? UINT_MAX - 1 : (unsigned)parsed_limit;
   return true;
}

static bool
terakan_get_experimental_segment_wide_phi(unsigned * const segment_size_out)
{
   char const * const segment_size_string =
      getenv("TERAKAN_EXPERIMENTAL_SEGMENT_WIDE_PHI");
   if (segment_size_string == NULL || segment_size_string[0] == '\0')
      return false;

   errno = 0;
   char *segment_size_end = NULL;
   unsigned long const parsed_segment_size =
      strtoul(segment_size_string, &segment_size_end, 0);
   if (errno != 0 || segment_size_end == segment_size_string ||
       segment_size_end[0] != '\0' || parsed_segment_size < 2) {
      *segment_size_out = 64;
      return true;
   }

   *segment_size_out = parsed_segment_size >= UINT_MAX
                          ? UINT_MAX - 1
                          : (unsigned)parsed_segment_size;
   return true;
}

static bool
terakan_get_experimental_early_wide_phi_segment(unsigned * const segment_size_out)
{
   char const * const segment_size_string =
      getenv("TERAKAN_EXPERIMENTAL_EARLY_WIDE_PHI_SEGMENT");
   if (segment_size_string == NULL || segment_size_string[0] == '\0')
      return false;

   errno = 0;
   char *segment_size_end = NULL;
   unsigned long const parsed_segment_size =
      strtoul(segment_size_string, &segment_size_end, 0);
   if (errno != 0 || segment_size_end == segment_size_string ||
       segment_size_end[0] != '\0' || parsed_segment_size < 2) {
      *segment_size_out = 64;
      return true;
   }

   *segment_size_out = parsed_segment_size >= UINT_MAX
                          ? UINT_MAX - 1
                          : (unsigned)parsed_segment_size;
   return true;
}

static bool
terakan_wide_phi_auto_segment_disabled(void)
{
   return getenv("TERAKAN_DISABLE_WIDE_PHI_AUTO_SEGMENT") != NULL;
}

static bool
terakan_def_is_scalar_load_const(nir_def * const def, unsigned const bit_size,
                                 nir_const_value * const value_out)
{
   if (def == NULL || def->num_components != 1 || def->bit_size != bit_size)
      return false;

   nir_instr * const instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_load_const)
      return false;

   nir_load_const_instr const * const load_const = nir_instr_as_load_const(instr);
   if (load_const->def.num_components != 1 || load_const->def.bit_size != bit_size)
      return false;

   *value_out = load_const->value[0];
   return true;
}

static bool
terakan_collect_wide_phi_const_chain(nir_def *def, nir_const_value * const values,
                                     unsigned const max_values,
                                     unsigned * const value_count_out,
                                     unsigned * const bit_size_out)
{
   unsigned value_count = 0;
   unsigned bit_size = 0;

   while (def != NULL) {
      nir_instr * const instr = nir_def_instr(def);
      if (instr->type != nir_instr_type_phi)
         break;

      nir_phi_instr * const phi = nir_instr_as_phi(instr);
      if (phi->def.num_components != 1)
         return false;

      if (bit_size == 0)
         bit_size = phi->def.bit_size;
      else if (phi->def.bit_size != bit_size)
         return false;

      nir_phi_src *phi_sources[2] = { NULL, NULL };
      unsigned phi_source_count = 0;
      nir_foreach_phi_src(phi_source, phi) {
         if (phi_source_count >= ARRAY_SIZE(phi_sources))
            return false;
         phi_sources[phi_source_count++] = phi_source;
      }
      if (phi_source_count != ARRAY_SIZE(phi_sources))
         return false;

      if (value_count >= max_values)
         return false;

      nir_const_value const_value;
      nir_def *previous_def = NULL;
      bool found_const = false;
      for (unsigned phi_source_index = 0; phi_source_index < ARRAY_SIZE(phi_sources);
           ++phi_source_index) {
         nir_def * const source_def = phi_sources[phi_source_index]->src.ssa;
         nir_const_value candidate_const_value;
         if (!found_const &&
             terakan_def_is_scalar_load_const(source_def, bit_size, &candidate_const_value)) {
            const_value = candidate_const_value;
            found_const = true;
         } else if (previous_def == NULL) {
            previous_def = source_def;
         } else {
            return false;
         }
      }

      if (!found_const || previous_def == NULL)
         return false;

      values[value_count++] = const_value;
      def = previous_def;
   }

   if (value_count < 2 || bit_size == 0)
      return false;

   for (unsigned i = 0; i < value_count / 2; ++i) {
      nir_const_value const value_tmp = values[i];
      values[i] = values[value_count - 1 - i];
      values[value_count - 1 - i] = value_tmp;
   }

   *value_count_out = value_count;
   *bit_size_out = bit_size;
   return true;
}

static nir_def *
terakan_find_wide_phi_selector(nir_function_impl * const impl, unsigned const case_count)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;

         nir_alu_instr * const alu = nir_instr_as_alu(instr);
         if (alu->op != nir_op_umod || alu->def.num_components != 1 ||
             alu->def.bit_size != 32)
            continue;

         nir_const_value const * const divisor = nir_src_as_const_value(alu->src[1].src);
         if (divisor != NULL && divisor->u32 == case_count)
            return &alu->def;
      }
   }

   return NULL;
}

static bool
terakan_debug_wide_phi_shape_enabled(void)
{
   return getenv("TERAKAN_DEBUG_WIDE_PHI_SHAPE") != NULL;
}

static void
terakan_debug_wide_phi_shape(nir_shader const * const nir,
                             nir_function_impl * const impl,
                             char const * const label)
{
   if (!terakan_debug_wide_phi_shape_enabled())
      return;

   unsigned total_phi_count = 0;
   unsigned max_phi_source_count = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_phi)
            continue;

         ++total_phi_count;
         unsigned phi_source_count = 0;
         nir_phi_instr * const phi = nir_instr_as_phi(instr);
         nir_foreach_phi_src(phi_source, phi) {
            (void)phi_source;
            ++phi_source_count;
         }
         max_phi_source_count = MAX2(max_phi_source_count, phi_source_count);
      }
   }

   enum { max_cases = 2048 };
   unsigned uav_value_phi_chain_count = 0;
   unsigned widest_uav_value_phi_chain = 0;
   unsigned widest_uav_value_bit_size = 0;
   bool widest_selector_found = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_uav_instr_r600 ||
             intrin->num_components != 1 || intrin->src[2].ssa == NULL ||
             intrin->src[2].ssa->num_components != 1)
            continue;

         nir_const_value values[max_cases];
         unsigned value_count = 0;
         unsigned bit_size = 0;
         if (!terakan_collect_wide_phi_const_chain(intrin->src[2].ssa, values,
                                                   max_cases, &value_count,
                                                   &bit_size))
            continue;

         ++uav_value_phi_chain_count;
         if (value_count > widest_uav_value_phi_chain) {
            widest_uav_value_phi_chain = value_count;
            widest_uav_value_bit_size = bit_size;
            widest_selector_found = terakan_find_wide_phi_selector(impl, value_count) != NULL;
         }
      }
   }

   fprintf(stderr,
           "TERAKAN_WIDE_PHI_SHAPE label=%s stage=%s total_phi=%u "
           "max_phi_sources=%u uav_phi_chains=%u widest_uav_phi_chain=%u "
           "widest_bit_size=%u selector_found=%u\n",
           label, mesa_shader_stage_name(nir->info.stage), total_phi_count,
           max_phi_source_count, uav_value_phi_chain_count,
           widest_uav_value_phi_chain, widest_uav_value_bit_size,
           widest_selector_found ? 1 : 0);
}

/* Tranche option (post-empirical): the original implementation built the
 * inner per-segment select as a linear cascade of `if (selector == k) ...
 * else previous_result` iterations.  That makes the `selector` SSA def live
 * across all 64 (or segment_size) iterations within a segment -- effectively
 * an O(segment_size)-deep nest.  This caused most of the residual pressure.
 *
 * Replace with a balanced binary tree over the segment's case values, mirroring
 * the outer `terakan_build_segment_tree_const_select`.  Now every value lookup
 * is O(log2(segment_size)) deep, and the selector's live range is bounded by
 * the tree depth, not the segment width. */
static nir_def *
terakan_build_segment_local_balanced_tree(nir_builder * const b,
                                          nir_def * const selector,
                                          nir_const_value const * const values,
                                          unsigned const bit_size,
                                          unsigned const first_value,
                                          unsigned const value_count_in_segment)
{
   if (value_count_in_segment == 0)
      return nir_undef(b, 1, bit_size);

   if (value_count_in_segment == 1) {
      nir_if * const case_if =
         nir_push_if(b, nir_ieq_imm(b, selector, first_value));
      nir_def * const case_value =
         nir_build_imm(b, 1, bit_size, &values[first_value]);
      nir_pop_if(b, case_if);
      return nir_if_phi(b, case_value, nir_undef(b, 1, bit_size));
   }

   unsigned const left_count = value_count_in_segment / 2;
   unsigned const right_count = value_count_in_segment - left_count;
   unsigned const split_value = first_value + left_count;

   nir_if * const split_if =
      nir_push_if(b, nir_ult_imm(b, selector, split_value));
   nir_def * const left_result = terakan_build_segment_local_balanced_tree(
      b, selector, values, bit_size, first_value, left_count);

   nir_push_else(b, split_if);
   nir_def * const right_result = terakan_build_segment_local_balanced_tree(
      b, selector, values, bit_size, split_value, right_count);

   nir_pop_if(b, split_if);
   return nir_if_phi(b, left_result, right_result);
}

static nir_def *
terakan_build_segment_local_const_select(nir_builder * const b, nir_def * const selector,
                                         nir_const_value const * const values,
                                         unsigned const value_count,
                                         unsigned const segment_size,
                                         unsigned const bit_size,
                                         unsigned const segment_index)
{
   unsigned const segment_first = segment_index * segment_size;
   unsigned const segment_last = MIN2(value_count, segment_first + segment_size);
   unsigned const segment_value_count = segment_last - segment_first;
   return terakan_build_segment_local_balanced_tree(
      b, selector, values, bit_size, segment_first, segment_value_count);
}

static nir_def *
terakan_build_segment_tree_const_select(nir_builder * const b, nir_def * const selector,
                                        nir_def * const segment_selector,
                                        nir_const_value const * const values,
                                        unsigned const value_count,
                                        unsigned const segment_size,
                                        unsigned const bit_size,
                                        unsigned const first_segment,
                                        unsigned const segment_count)
{
   if (segment_count == 1) {
      return terakan_build_segment_local_const_select(b, selector, values, value_count,
                                                      segment_size, bit_size,
                                                      first_segment);
   }

   unsigned const left_count = segment_count / 2;
   unsigned const right_count = segment_count - left_count;
   unsigned const split_segment = first_segment + left_count;
   nir_if * const segment_if =
      nir_push_if(b, nir_ult_imm(b, segment_selector, split_segment));
   nir_def * const left_result =
      terakan_build_segment_tree_const_select(b, selector, segment_selector, values,
                                              value_count, segment_size, bit_size,
                                              first_segment, left_count);

   nir_push_else(b, segment_if);
   nir_def * const right_result =
      terakan_build_segment_tree_const_select(b, selector, segment_selector, values,
                                              value_count, segment_size, bit_size,
                                              split_segment, right_count);

   nir_pop_if(b, segment_if);
   return nir_if_phi(b, left_result, right_result);
}

static nir_def *
terakan_build_segmented_const_select(nir_builder * const b, nir_def * const selector,
                                     nir_const_value const * const values,
                                     unsigned const value_count,
                                     unsigned const segment_size,
                                     unsigned const bit_size)
{
   unsigned const segment_count = (value_count + segment_size - 1) / segment_size;
   /* Tranche option 1: when segment_size is a power of 2, the segment selector
    * is a right-shift of the case-selector instead of a udiv -- one ALU op
    * fewer and one fewer live range to colour. */
   nir_def *segment_selector;
   if (util_is_power_of_two_nonzero(segment_size)) {
      segment_selector = nir_ushr_imm(b, selector, util_logbase2(segment_size));
   } else {
      segment_selector = nir_udiv_imm(b, selector, segment_size);
   }
   return terakan_build_segment_tree_const_select(b, selector, segment_selector, values,
                                                  value_count, segment_size, bit_size, 0,
                                                  segment_count);
}

static bool
terakan_segment_wide_phi_impl(nir_function_impl * const impl, unsigned const segment_size)
{
   enum { max_cases = 2048 };
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr * const intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_uav_instr_r600 ||
             intrin->num_components != 1 || intrin->src[2].ssa == NULL ||
             intrin->src[2].ssa->num_components != 1)
            continue;

         nir_const_value values[max_cases];
         unsigned value_count = 0;
         unsigned bit_size = 0;
         if (!terakan_collect_wide_phi_const_chain(intrin->src[2].ssa, values, max_cases,
                                                   &value_count, &bit_size) ||
             value_count <= segment_size)
            continue;

         nir_def * const selector = terakan_find_wide_phi_selector(impl, value_count);
         if (selector == NULL)
            continue;

         nir_builder builder = nir_builder_at(nir_before_instr(&intrin->instr));
         nir_def * const segmented_value =
            terakan_build_segmented_const_select(&builder, selector, values, value_count,
                                                 segment_size, bit_size);
         if (segmented_value == NULL)
            continue;

         nir_src_rewrite(&intrin->src[2], segmented_value);
         progress = true;

         fprintf(stderr,
                 "TERAKAN_EXPERIMENTAL_SEGMENT_WIDE_PHI: segmented %u-case uav value with segment size %u\n",
                 value_count, segment_size);
      }
   }

   return nir_progress(progress, impl, nir_metadata_none);
}

static bool
terakan_segment_wide_phi_defs_impl(nir_function_impl * const impl,
                                   unsigned const segment_size,
                                   unsigned const min_value_count,
                                   char const * const log_label)
{
   enum { max_cases = 2048, max_chains = 16 };
   nir_const_value values[max_cases];
   nir_def *already_rewritten[max_chains] = { NULL };
   unsigned already_rewritten_count = 0;
   bool progress = false;

   /* Tranche option 5: process all qualifying root phis, not just the last one
    * found.  `nir_def_rewrite_uses` only redirects consumers -- the original
    * phi chain remains in the IR and would be detected again on a fresh sweep.
    * Track rewritten root defs and skip them on subsequent passes. */
   for (;;) {
      nir_phi_instr *root_phi = NULL;
      unsigned value_count = 0;
      unsigned bit_size = 0;

      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_phi)
               continue;

            nir_phi_instr * const phi = nir_instr_as_phi(instr);

            bool already_done = false;
            for (unsigned i = 0; i < already_rewritten_count; ++i) {
               if (already_rewritten[i] == &phi->def) {
                  already_done = true;
                  break;
               }
            }
            if (already_done)
               continue;

            unsigned candidate_value_count = 0;
            unsigned candidate_bit_size = 0;
            if (!terakan_collect_wide_phi_const_chain(&phi->def, values, max_cases,
                                                      &candidate_value_count,
                                                      &candidate_bit_size) ||
                candidate_value_count < min_value_count ||
                candidate_value_count <= segment_size)
               continue;

            if (terakan_find_wide_phi_selector(impl, candidate_value_count) == NULL)
               continue;

            root_phi = phi;
            value_count = candidate_value_count;
            bit_size = candidate_bit_size;
            goto found;
         }
      }
      break;

   found:;
      nir_def * const selector = terakan_find_wide_phi_selector(impl, value_count);
      if (selector == NULL)
         break;

      nir_builder builder = nir_builder_at(nir_after_phis(root_phi->instr.block));
      nir_def * const segmented_value =
         terakan_build_segmented_const_select(&builder, selector, values, value_count,
                                              segment_size, bit_size);
      if (segmented_value == NULL)
         break;

      nir_def_rewrite_uses(&root_phi->def, segmented_value);
      if (already_rewritten_count < max_chains)
         already_rewritten[already_rewritten_count++] = &root_phi->def;
      progress = true;

      fprintf(stderr,
              "%s: segmented %u-case phi value with segment size %u min cases %u\n",
              log_label, value_count, segment_size, min_value_count);

      /* Safety: a multi-chain shader should not exceed `max_chains` rewrites.
       * Stop unconditionally if we hit the cap to avoid any pathological loop. */
      if (already_rewritten_count >= max_chains)
         break;
   }

   if (!progress)
      return false;

   return nir_progress(true, impl, nir_metadata_none);
}

static bool
terakan_segment_wide_phi_defs(nir_shader * const nir, unsigned const segment_size,
                              unsigned const min_value_count,
                              char const * const log_label)
{
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      progress |= terakan_segment_wide_phi_defs_impl(impl, segment_size,
                                                     min_value_count, log_label);
   }
   return progress;
}

static bool
terakan_segment_wide_phi(nir_shader * const nir, unsigned const segment_size)
{
   bool progress = false;
   nir_foreach_function_impl(impl, nir) {
      progress |= terakan_segment_wide_phi_impl(impl, segment_size);
   }
   return progress;
}

void
terakan_shader_lower_and_optimize_post_link(
   nir_shader * const nir, struct terakan_pipeline_layout const * const pipeline_layout,
   BITSET_WORD * const resources_needed, uint32_t * const samplers_needed,
   BITSET_WORD * const uavs_for_mutable_resources_needed,
   uint32_t * const driver_push_constants_used,
   uint16_t * const kcache_needed,
   uint8_t * const fragment_data_uncompacted_locations_out,
   bool const robust_buffer_access)
{
   bool progress;

   /* Finally eliminate all dead code that may have effect on lowerings below and on analysis within
    * SFN, so that the demands of the shader can be estimated as accurately as possible.
    *
    * SFN also needs SSA, local variables need to be lowered to SSA, and the stores left after the
    * lowering need to be cleaned up, at some point.
    * Do that as part of the DCE loop, so that DCE works accurately through variable access, and can
    * provide feedback to dead variable removal.
    * Note that while the shader has functions inlined as part of SPIR-V to NIR conversion,
    * nir_var_shader_temp may be generated by passes like nir_lower_io_vars_to_temporaries.
    * They must be lowered to nir_var_function_temp for this cleanup to work.
    */

   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_remove_dead_variables, nir_var_function_temp, NULL);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);

   unsigned early_segment_wide_phi_size = 0;
   unsigned early_segment_wide_phi_min_cases = 0;
   char const *early_segment_wide_phi_label = NULL;
   if (terakan_get_experimental_early_wide_phi_segment(&early_segment_wide_phi_size)) {
      early_segment_wide_phi_min_cases =
         early_segment_wide_phi_size < UINT_MAX ? early_segment_wide_phi_size + 1 : UINT_MAX;
      early_segment_wide_phi_label = "TERAKAN_EXPERIMENTAL_EARLY_WIDE_PHI_SEGMENT";
   } else if (!terakan_wide_phi_auto_segment_disabled()) {
      /* The 512-way Vulkan 1.0 opPhi.wide shape exceeds normal and physical
       * SFN register budgets, while the balanced early segment rewrite lowers
       * its measured pressure from 133 to 22.  Keep the default predicate
       * narrow: only constant selector-backed phi chains at the failing fan-in
       * boundary are transformed. */
      early_segment_wide_phi_size = 64;
      early_segment_wide_phi_min_cases = 512;
      early_segment_wide_phi_label = "TERAKAN_WIDE_PHI_AUTO_SEGMENT";
   }

   if (early_segment_wide_phi_size != 0) {
      if (terakan_segment_wide_phi_defs(nir, early_segment_wide_phi_size,
                                        early_segment_wide_phi_min_cases,
                                        early_segment_wide_phi_label)) {
         bool cleanup_progress;
         do {
            cleanup_progress = false;
            NIR_PASS(cleanup_progress, nir, nir_opt_copy_prop);
            NIR_PASS(cleanup_progress, nir, nir_opt_dce);
            NIR_PASS(cleanup_progress, nir, nir_opt_remove_phis);
            NIR_PASS(cleanup_progress, nir, nir_opt_dead_cf);
         } while (cleanup_progress);
      }

      if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
         fprintf(stderr,
                 "TERAKAN_NIR_SPIRV: --- post %s=%u min_cases=%u (%s) ---\n",
                 early_segment_wide_phi_label, early_segment_wide_phi_size,
                 early_segment_wide_phi_min_cases, mesa_shader_stage_name(nir->info.stage));
         nir_print_shader(nir, stderr);
      }
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* For fragment data location compaction. */
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in, NULL);
   }

   /* Compact fragment shader output locations.
    * See the description of terakan_nir_compact_fragment_data_locations.
    */

   uint8_t fragment_data_uncompacted_locations = 0b0;
   NIR_PASS(_, nir, terakan_nir_compact_fragment_data_locations,
            &fragment_data_uncompacted_locations);
   if (fragment_data_uncompacted_locations_out != NULL) {
      *fragment_data_uncompacted_locations_out = fragment_data_uncompacted_locations;
   }

   /* Assign gl_frag_result values to variables after the fragment data location compaction has
    * remapped the locations to the hardware values.
    */

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_assign_io_var_locations(nir, nir_var_shader_out);
   }

   /* Lower texture operations. */

   NIR_PASS(_, nir, r600_nir_lower_cube_to_2darray);

   /* Vectorize loads that will be lowered to typed buffer load (vertex fetch) instructions, but
    * first lower all loads to scalar to make sure hardware constraints for vectorizing are taken
    * into account. Also, lower SSBO stores to scalar because the hardware instruction stores one
    * element.
    */

   nir_load_store_vectorize_options load_store_vectorize_options = {
      .callback = terakan_nir_should_vectorize_load_store,
      .modes = nir_var_mem_ubo | nir_var_mem_push_const | nir_var_mem_ssbo,
   };
   NIR_PASS(_, nir, nir_lower_io_to_scalar, load_store_vectorize_options.modes, NULL, NULL);

   /* Widen sub-32-bit SSBO/global accesses to 32-bit.
    *
    * MEM_RAT_STORE_TYPED writes one dword per element on the cached
    * path, so sub-32-bit stores must be widened to 32-bit-aligned RMW.
    * UBO/push-const are not in the modes here; they are handled
    * separately via KCACHE.
    *
    * may_lower_unaligned_stores_to_atomics=true: the pass generates
    * atomic_swap (cmpxchg-shape) for partial-dword stores.  The cached
    * MEM_RAT_CMPXCHG_INT path is silicon-broken on the validated
    * PALM/SUMO Evergreen TeraScale-2 target, so the downstream
    * cmpxchg-to-speculative-xchg lowering rewrites those generated
    * cmpxchgs to load+bcsel+atomic_xchg.  Single-thread test patterns
    * are correct under this chain; cross-thread sub-dword racing
    * remains a documented gap.
    */
   nir_lower_mem_access_bit_sizes_options mem_access_options = {
      .callback = terakan_nir_mem_access_size_align,
      .modes = nir_var_mem_ssbo | nir_var_mem_global,
      .may_lower_unaligned_stores_to_atomics = true,
   };
   NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &mem_access_options);
   /* Use the effective per-stage robustness flag computed by the pipeline
    * compiler (device feature OR VK_EXT_pipeline_robustness per-stage state).
    * See terakan_nir_buffer_uav_coord for the hardware rationale. */
   if (robust_buffer_access) {
      load_store_vectorize_options.robust_modes |= nir_var_mem_ubo | nir_var_mem_ssbo;
   }
   load_store_vectorize_options.cb_data = &load_store_vectorize_options.robust_modes;
   NIR_PASS(_, nir, nir_opt_load_store_vectorize, &load_store_vectorize_options);

   /* Speculative-XCHG emulation for atomic compare-and-swap.
    *
    * Evergreen ISA exposes CMPXCHG_INT (RAT_INST=4) and CMPXCHG_INT_RTN
    * (RAT_INST=36) on the MEM_RAT cached path, but PALM/Wrestler silicon
    * silently no-ops them (the L2 cache lacks the compare-and-conditional-
    * write comparator).  Rewrite supported CAS intrinsics to a load +
    * bcsel + atomic_xchg sequence using only PALM-confirmed-working
    * hardware.  Must run BEFORE terakan_nir_lower_bindings so the lowered
    * xchg flows through the standard atomic UAV path. */
   NIR_PASS(_, nir, terakan_nir_lower_cmpxchg_to_speculative_xchg);

   /* Lower bindings according to the pipeline layout.
    * In fragment shaders, this is done after compacting the fragment data output locations as UAVs
    * must be placed above color attachments.
    * robust_buffer_access is threaded here so that the UAV coord clamp in
    * terakan_nir_buffer_uav_coord honours per-pipeline robustness state. */
   NIR_PASS(_, nir, terakan_nir_lower_bindings, pipeline_layout, resources_needed, samplers_needed,
            nir->info.stage == MESA_SHADER_FRAGMENT
               ? util_bitcount(fragment_data_uncompacted_locations)
               : 0,
            uavs_for_mutable_resources_needed, driver_push_constants_used,
            kcache_needed, robust_buffer_access);

   /* Texture-gather (FETCH4 / GATHER4) per-binding VkComponentMapping
    * rewrite.  AMD Evergreen-Family ISA Chapter 6: SQ_TEX_RESOURCE_WORD4
    * DST_SEL permutes the four FETCH result lanes -- correct for SAMPLE
    * but wrong for FETCH4 where the four lanes are spatial corner
    * samples.  This pass runs after binding lowering so
    * `tex->texture_index` is the physical resource slot (the index
    * used for the runtime-swizzle KCACHE lookup in bank 14).  Marks
    * KCACHE bank 14 as needed iff any tg4 was rewritten. */
   {
      bool any_tg4 = false;
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type == nir_instr_type_tex &&
                   nir_instr_as_tex(instr)->op == nir_texop_tg4) {
                  any_tg4 = true;
                  break;
               }
            }
            if (any_tg4) break;
         }
         if (any_tg4) break;
      }
      if (getenv("TERAKAN_TG4_VS_TRACE") != NULL) {
         FILE *tf = fopen("/tmp/terakan_tg4_trace.log", "a");
         if (tf) {
            fprintf(tf, "stage=%s any_tg4=%d\n",
                    mesa_shader_stage_name(nir->info.stage),
                    any_tg4 ? 1 : 0);
            fflush(tf);
            fclose(tf);
         }
      }
      if (any_tg4) {
         *kcache_needed |=
            (uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA;
         NIR_PASS(_, nir, terakan_nir_lower_tg4_view_swizzle);
      }
   }

   if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
      fprintf(stderr, "TERAKAN_NIR_SPIRV: --- post terakan_nir_lower_bindings (%s) ---\n",
              mesa_shader_stage_name(nir->info.stage));
      nir_print_shader(nir, stderr);
   }

   if (terakan_debug_wide_phi_shape_enabled()) {
      nir_foreach_function_impl(impl, nir) {
         terakan_debug_wide_phi_shape(nir, impl, "post_bindings");
      }
   }

   unsigned wide_phi_select_limit = 0;
   if (terakan_get_experimental_wide_phi_select_limit(&wide_phi_select_limit)) {
      bool wide_phi_select_progress;
      nir_opt_peephole_select_options wide_phi_select_options = {
         .limit = wide_phi_select_limit,
         .indirect_load_ok = false,
         .expensive_alu_ok = true,
      };

      do {
         wide_phi_select_progress = false;
         NIR_PASS(wide_phi_select_progress, nir, nir_opt_peephole_select,
                  &wide_phi_select_options);
         if (wide_phi_select_progress) {
            NIR_PASS(_, nir, nir_opt_copy_prop);
            NIR_PASS(_, nir, nir_opt_dce);
            NIR_PASS(_, nir, nir_opt_dead_cf);
         }
      } while (wide_phi_select_progress);

      if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
         fprintf(stderr,
                 "TERAKAN_NIR_SPIRV: --- post TERAKAN_EXPERIMENTAL_WIDE_PHI_SELECT_LIMIT=%u (%s) ---\n",
                 wide_phi_select_limit, mesa_shader_stage_name(nir->info.stage));
         nir_print_shader(nir, stderr);
      }
   }

   unsigned segment_wide_phi_size = 0;
   if (terakan_get_experimental_segment_wide_phi(&segment_wide_phi_size)) {
      if (terakan_segment_wide_phi(nir, segment_wide_phi_size)) {
         bool cleanup_progress;
         do {
            cleanup_progress = false;
            NIR_PASS(cleanup_progress, nir, nir_opt_copy_prop);
            NIR_PASS(cleanup_progress, nir, nir_opt_dce);
            NIR_PASS(cleanup_progress, nir, nir_opt_remove_phis);
            NIR_PASS(cleanup_progress, nir, nir_opt_if,
                     nir_opt_if_optimize_phi_true_false);
            NIR_PASS(cleanup_progress, nir, nir_opt_dead_cf);
         } while (cleanup_progress);
      }

      if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
         fprintf(stderr,
                 "TERAKAN_NIR_SPIRV: --- post TERAKAN_EXPERIMENTAL_SEGMENT_WIDE_PHI=%u (%s) ---\n",
                 segment_wide_phi_size, mesa_shader_stage_name(nir->info.stage));
         nir_print_shader(nir, stderr);
      }
   }

   if (terakan_debug_wide_phi_shape_enabled()) {
      nir_foreach_function_impl(impl, nir) {
         terakan_debug_wide_phi_shape(nir, impl, "pre_scalar_lowering");
      }
   }

   /* Perform lowerings on the level of basic building blocks after the interface has been set up.
    */

   /* TODO(Triang3l): Invoke nir_lower_fragcoord_wtrans when r600_lower_and_optimize_nir is removed.
    */

   assert(nir->options->lower_to_scalar);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, nir->options->lower_to_scalar_filter, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);

   if (terakan_debug_wide_phi_shape_enabled()) {
      nir_foreach_function_impl(impl, nir) {
         terakan_debug_wide_phi_shape(nir, impl, "post_lower_phis_to_scalar");
      }
   }

   /* Everything lowered by nir_lower_alu is supported natively as of this writing. */

   NIR_PASS(_, nir, nir_lower_pack);
   NIR_PASS(_, nir, nir_lower_bit_size, terakan_lower_bit_size_callback, NULL);

   nir_lower_idiv_options lower_idiv_options = {};
   NIR_PASS(_, nir, nir_lower_idiv, &lower_idiv_options);

   /* Lower flrp before optimization so algebraic / CSE can clean up the
    * expansion.  This matches RADV's pipeline ordering. */
   if (!nir->info.flrp_lowered) {
      assert(nir->options->lower_flrp16 && nir->options->lower_flrp32 &&
             nir->options->lower_flrp64);
      NIR_PASS(_, nir, nir_lower_flrp, 16 | 32 | 64, false);
      nir->info.flrp_lowered = true;
   }

   /* -------------------------------------------------------------------
    * Lightweight pre-SFN cleanup.
    *
    * Only run copy propagation, algebraic (mandatory lowerings), constant
    * folding, and DCE here.  The heavy iterative convergence loop runs
    * AFTER r600_lower_and_optimize_nir (in terakan_shader_impl_compile)
    * so it can see the full IR including SFN's IF/ENDIF write guards,
    * KCACHE bank-14 loads, and bounds-check ALU.
    *
    * VLIW5 constraint: no scalarization passes here (or in the post-SFN
    * loop).  TeraScale physically thrives on vec4 ops; scalarizing strips
    * the vector dependency graph that SFN's C4 Bundle Packer needs to
    * fill X,Y,Z,W slots.  Phase 1 already ran nir_lower_alu_to_scalar
    * with the architecture-specific filter; do not add more on top.
    * ------------------------------------------------------------------- */
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_intrinsics);
   NIR_PASS(_, nir, nir_opt_algebraic);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);

   if (getenv("TERAKAN_DEBUG_NIR_SPIRV") != NULL) {
      fprintf(stderr, "TERAKAN_NIR_SPIRV: --- post terakan_shader_lower_and_optimize_post_link (%s) ---\n",
              mesa_shader_stage_name(nir->info.stage));
      nir_print_shader(nir, stderr);
   }
}
/* =====================================================================
 * UINT24 peephole — convert imul → umul24 / iadd(umul24, c) → umad24.
 *
 * ISA basis: Evergreen_ISA.pdf §2.5 — MUL_UINT24 performs an unsigned
 * 24×24→32 multiply using the FP24 mantissa datapath.  It is single-
 * cycle and executes on ANY vector ALU slot, whereas MULLO_INT is
 * multi-cycle and restricted to the trans-only slot.  MULADD_UINT24
 * fuses a 24-bit multiply with a 32-bit add in one op3 slot.
 *
 * The optimisation is valid when both multiply operands are provably
 * in [0, 2^24).  The addend in umad24 has no range restriction.
 *
 * Running this at the NIR level (before SFN) enables the algebraic
 * optimizer to further combine umul24 + iadd → umad24, and lets DCE
 * clean up any dead intermediate instructions.
 * =================================================================== */

/* Conservative range proof: return true when |src| is provably in
 * the unsigned range [0, 2^24).  Only 32-bit scalar/vector ALU defs
 * are analysed; all other forms return false (safe default). */
static bool
terakan_nir_value_fits_24bit(nir_alu_src *alu_src)
{
   nir_def *def = alu_src->src.ssa;
   if (def->bit_size != 32)
      return false;

   /* --- Constant --- */
   nir_const_value *cv = nir_src_as_const_value(alu_src->src);
   if (cv)
      return cv->u32 < (1u << 24);

   /* Remaining checks require the source to be an ALU instruction. */
   nir_instr *parent = nir_def_instr(def);
   if (parent->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *src_alu = nir_instr_as_alu(parent);

   switch (src_alu->op) {
   /* extract_u8 → [0, 255], extract_u16 → [0, 65535] */
   case nir_op_extract_u8:
   case nir_op_extract_u16:
      return true;

   /* Zero-extend from ≤16-bit → always < 2^16 */
   case nir_op_u2u32:
      return src_alu->src[0].src.ssa->bit_size <= 16;

   /* Bitwise AND: result ≤ mask.  If mask is constant < 2^24, the
    * result is provably < 2^24 regardless of the other operand. */
   case nir_op_iand: {
      nir_const_value *mask0 =
         nir_src_as_const_value(src_alu->src[0].src);
      nir_const_value *mask1 =
         nir_src_as_const_value(src_alu->src[1].src);
      if (mask0 && mask0->u32 < (1u << 24))
         return true;
      if (mask1 && mask1->u32 < (1u << 24))
         return true;
      return false;
   }

   /* Logical right shift by constant ≥ 8 on a 32-bit value:
    * result < 2^(32 − shift_amt).  For shift ≥ 8, that's < 2^24. */
   case nir_op_ushr: {
      nir_const_value *shift =
         nir_src_as_const_value(src_alu->src[1].src);
      if (shift && shift->u32 >= 8 && shift->u32 < 32)
         return true;
      return false;
   }

   default:
      return false;
   }
}

/* Instruction callback for nir_shader_instructions_pass: lower a single
 * nir_op_imul to nir_op_umul24 when both sources fit in 24 bits, and
 * fuse iadd(umul24, c) → umad24 when the umul24 has a single use. */
static bool
terakan_nir_opt_uint24_instr(nir_builder *b, nir_instr *instr,
                             UNUSED void *cb_data)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);

   /* --- Phase 1: imul → umul24 when both sources fit 24 bits --- */
   if (alu->op == nir_op_imul &&
       alu->def.bit_size == 32 &&
       terakan_nir_value_fits_24bit(&alu->src[0]) &&
       terakan_nir_value_fits_24bit(&alu->src[1])) {
      alu->op = nir_op_umul24;
      return true;
   }

   /* --- Phase 2: iadd(umul24(a, b), c) → umad24(a, b, c) ---
    * umad24 is a single op3-slot instruction that fuses multiply + add.
    * The addend c has no 24-bit range restriction.
    *
    * Only fuse when the umul24 has exactly one use (this iadd), so that
    * replacing the iadd with umad24 makes the umul24 dead (cleaned up
    * by the subsequent DCE pass). */
   if (alu->op == nir_op_iadd && alu->def.bit_size == 32) {
      for (unsigned i = 0; i < 2; i++) {
         nir_def *mul_def = alu->src[i].src.ssa;
         nir_instr *mul_instr = nir_def_instr(mul_def);
         if (mul_instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr *mul = nir_instr_as_alu(mul_instr);
         if (mul->op != nir_op_umul24)
            continue;
         if (!list_is_singular(&mul_def->uses))
            continue;

         unsigned add_idx = 1 - i;
         b->cursor = nir_before_instr(instr);
         nir_def *mad = nir_build_alu3(b, nir_op_umad24,
                                       mul->src[0].src.ssa,
                                       mul->src[1].src.ssa,
                                       alu->src[add_idx].src.ssa);
         nir_def_rewrite_uses(&alu->def, mad);
         nir_instr_remove(instr);
         return true;
      }
   }

   return false;
}

static bool
terakan_nir_opt_uint24(nir_shader *nir)
{
   return nir_shader_instructions_pass(
      nir, terakan_nir_opt_uint24_instr,
      nir_metadata_control_flow,
      NULL);
}

/*
 * Filter callback for nir_remove_dead_variables: returns true for VS output
 * variables whose location is in the dead_varyings bitmask.
 */
static bool
terakan_nir_is_dead_vs_output(nir_variable *var, void *data)
{
   uint64_t dead_mask = *(const uint64_t *)data;
   return (dead_mask & BITFIELD64_BIT(var->data.location)) != 0;
}

void
terakan_postprocess_nir(nir_shader *nir,
                        mesa_shader_stage stage,
                        bool remove_point_size,
                        uint64_t fs_inputs_read)
{
   bool progress = false;

   /* --- UINT24 peephole (all stages) ---
    * Convert imul → umul24 for bounded operands, then fuse
    * iadd(umul24, c) → umad24.  This runs for ALL shader stages
    * because integer multiplies appear everywhere (array indexing,
    * struct offsets, loop counters × strides). */
   NIR_PASS(progress, nir, terakan_nir_opt_uint24);

   /* --- VS-specific varying pruning --- */
   if (stage == MESA_SHADER_VERTEX) {
      /* Compute the set of dead VS outputs to prune. */
      uint64_t dead_varyings = 0;

      /* --- Point size removal ---
       * If topology is definitively not POINT_LIST AND VS is the true last
       * vertex stage, the PA ignores gl_PointSize.  Strip it to save one
       * PARAM export + the ALU that computes it.
       * The caller guarantees remove_point_size is only set when safe
       * (static non-point topology, not as_es, not as_ls). */
      if (remove_point_size && (nir->info.outputs_written & VARYING_BIT_PSIZ))
         dead_varyings |= VARYING_BIT_PSIZ;

      /* --- Varying pruning (FS→VS feedback) ---
       * Only prune PARAM exports — POS/CLIP/CULL/LAYER/VIEWPORT/EDGE are
       * consumed by the PA/clipper hardware, not routed through SPI to FS.
       * PSIZ is PA-consumed and handled separately above.
       *
       * ISA basis: SPI_VS_OUT_ID uses semantic ID matching against
       * SPI_PS_INPUT_CNTL.  Dense-packing surviving PARAM exports after
       * pruning is safe — the SFN backend assigns consecutive export_param
       * indices to surviving outputs, and SPI finds them by semantic ID
       * regardless of physical slot.  (Evergreen_3D_Registers_v2 §SPI) */
      uint64_t const hw_consumed_outputs =
         VARYING_BIT_POS | VARYING_BIT_PSIZ |
         VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1 |
         VARYING_BIT_CULL_DIST0 | VARYING_BIT_CULL_DIST1 |
         VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT | VARYING_BIT_EDGE;

      uint64_t const param_outputs =
         nir->info.outputs_written & ~hw_consumed_outputs;
      uint64_t const dead_params = param_outputs & ~fs_inputs_read;
      dead_varyings |= dead_params;

      if (dead_varyings != 0) {
         /* Strip store_output intrinsics targeting dead varyings.
          * This is the actual codegen change — SFN will not see these
          * outputs and will not emit PARAM exports for them. */
         nir_function_impl *impl = nir_shader_get_entrypoint(nir);
         nir_foreach_block(block, impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
               if (intrin->intrinsic != nir_intrinsic_store_output)
                  continue;
               nir_io_semantics sem = nir_intrinsic_io_semantics(intrin);
               if (dead_varyings & BITFIELD64_BIT(sem.location))
                  nir_instr_remove(instr);
            }
         }

         /* Manual nir_instr_remove invalidates all derived metadata
          * (dominance, liveness, block indices).  Explicitly invalidate so
          * subsequent passes (DCE, gather_info) rebuild from scratch rather
          * than operating on stale cached data. */
         impl->valid_metadata = nir_metadata_none;

         /* Remove dead output variables via standard NIR infrastructure.
          * Using nir_remove_dead_variables ensures all bookkeeping
          * (variable lists, counts) is updated correctly. */
         nir_remove_dead_variables_options dead_var_opts = {
            .can_remove_var = terakan_nir_is_dead_vs_output,
            .can_remove_var_data = &dead_varyings,
         };
         NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_out,
                  &dead_var_opts);

         /* Clear outputs_written bits for pruned varyings. */
         nir->info.outputs_written &= ~dead_varyings;

         progress = true;

         /* Debug: verify below after DCE + gather_info. */
         assert((nir->info.outputs_written & dead_varyings) == 0);
      }
   }

   /* --- Final cleanup (all stages) ---
    * DCE removes ALU chains made dead by uint24 fusion or varying
    * pruning.  Re-gather shader info to keep metadata consistent.
    *
    * Note: TeraScale-2 does not support transform feedback (no XFB
    * extension advertised), so pruning based on fs_inputs_read alone
    * is safe.  If XFB support is ever added, pruning must also check
    * xfb_info to avoid stripping feedback-captured outputs. */
   if (progress) {
      nir_function_impl *impl = nir_shader_get_entrypoint(nir);
      NIR_PASS(_, nir, nir_opt_dce);
      nir_shader_gather_info(nir, impl);
   }
}

void
terakan_shader_impl_finish(struct terakan_shader_impl * const shader,
                           VkAllocationCallbacks const * const allocator)
{
   if (shader->shader.arrays != NULL) {
      free(shader->shader.arrays);
   }

   if (shader->static_state.program_bo != NULL) {
      terakan_bo_free(shader->static_state.program_bo, allocator);
      shader->static_state.program_bo = NULL;
   }
}
