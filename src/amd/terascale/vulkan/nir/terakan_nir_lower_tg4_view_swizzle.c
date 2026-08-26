/*
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_nir_lower_tg4_view_swizzle
 *
 * Per AMD Evergreen-Family Instruction Set Architecture, Chapter 6
 * (Texture Cache Clauses): the TEX_WORD1 DST_SEL_[X,Y,Z,W] fields
 * specify which fetched element each result lane reads.  For SAMPLE
 * the "fetched elements" are the four channels R/G/B/A of one texel
 * (so DST_SEL correctly implements VkComponentMapping when baked
 * into the descriptor's SQ_TEX_RESOURCE_WORD4).  For FETCH4 /
 * GATHER4 (opcode 0x15, ISA §9.4) the four fetched elements are
 * four spatial corner samples of one channel, and the descriptor's
 * DST_SEL then permutes spatial samples as if they were channels --
 * which is wrong for any non-identity VkComponentMapping.
 *
 * This pass detects `nir_texop_tg4` instructions after
 * `terakan_nir_lower_bindings` has resolved `tex->texture_index` to
 * an absolute physical sampler resource slot. The pipeline-layout map
 * translates that resource identity to a compact per-stage metadata
 * index, and the pass loads the runtime VkComponentMapping from KCACHE
 * bank 14 (populated by
 * `terakan_pipeline_layout.c::terakan_CmdBindDescriptorSets` from
 * `image_view->component_swizzle_packed`), and remaps the gather
 * component argument so that the actual FETCH4 reads the swizzled
 * channel.  For VK_COMPONENT_SWIZZLE_ZERO / _ONE the result is
 * replaced with a constant vec4.
 *
 * Cost: per affected gather, 4 x FETCH4 + 5 x bcsel + 1 KCACHE load
 * + a few ALU extractions.  The 4x FETCH4 cost is unavoidable
 * because the per-VkImageView swizzle is not known at pipeline
 * compile time; CTS texture-gather tests are rare in hot paths so
 * this is acceptable for correctness.
 *
 * KCACHE layout (terakan_robustness_metadata.c::terakan_robustness_metadata_apply):
 *   bank 14, stage-selected line, dwords 40..51 = view_swizzles[12]
 *   Two bindings packed per dword (low 16 bits = even slot,
 *   high 16 bits = odd slot), each binding 16 bits = 4 channels
 *   x 4-bit channel target.  Channel target values match
 *   VK_COMPONENT_SWIZZLE_R/G/B/A/ZERO/ONE (0..5).
 */

#include "terakan_nir.h"
#include "terakan_descriptor.h"
#include "terakan_tg4_metadata.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#include "util/bitset.h"

/* Channel-target enum values stored in the packed swizzle nibble.
 * Match VK_COMPONENT_SWIZZLE_* in the AMD-side encoding so the
 * downstream comparison can use literal small ints. */
#define TERAKAN_TG4_TARGET_R    0u
#define TERAKAN_TG4_TARGET_G    1u
#define TERAKAN_TG4_TARGET_B    2u
#define TERAKAN_TG4_TARGET_A    3u
#define TERAKAN_TG4_TARGET_ZERO 4u
#define TERAKAN_TG4_TARGET_ONE  5u

/* State threaded into the pass binds absolute resource IDs to the compact
 * sampled-image metadata namespace for the shader stage. */
struct terakan_tg4_view_swizzle_state {
   struct terakan_tg4_metadata_map const * metadata_map;
};

/* Clone `tex` with `op == nir_texop_tg4` rewritten to gather channel
 * `new_component` instead of `tex->component`.  Returns the new tex
 * instr's def.  Uses `nir_instr_clone` so source rewrites and SSA
 * book-keeping happen via the canonical NIR clone path.
 *
 * The clone's `texture_index` is shifted by
 * TERAKAN_TG4_GATHER_SLOT_OFFSET to route the FETCH4 through the
 * gather-safe descriptor (identity DST_SEL) bound at the parallel
 * HW slot by `terakan_pipeline_layout.c`.  This is what makes the
 * AMD IL `gather4_comp_sel` HW contract satisfiable: the gather-safe
 * descriptor never carries SEL_0 / SEL_1 in DST_SEL, so the HW lane
 * permutation produces the spatial corner samples cleanly, and the
 * application's VkComponentMapping (including ZERO / ONE) is
 * reconstructed via the bcsel cascade below. */
static nir_def *
clone_tg4_with_component(nir_builder * const b, nir_tex_instr * const tex,
                         unsigned const new_component)
{
   nir_instr * const cloned_instr = nir_instr_clone(b->shader, &tex->instr);
   nir_tex_instr * const clone = nir_instr_as_tex(cloned_instr);
   clone->component = new_component;
   /* Shift only texture_index, not sampler_index: the gather-safe
    * sibling is a parallel SQ_TEX_RESOURCE descriptor (different
    * RESOURCE_ID).  The sampler is unchanged -- it carries no
    * gather-specific state and the same SAMPLER_ID is reused. */
   clone->texture_index += TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET;
   nir_builder_instr_insert(b, &clone->instr);
   return &clone->def;
}

static bool
terakan_nir_lower_tg4_view_swizzle_instr(nir_builder * const b,
                                         nir_instr * const instr,
                                         void * data)
{
   struct terakan_tg4_view_swizzle_state * const state =
      (struct terakan_tg4_view_swizzle_state *)data;

   if (instr->type != nir_instr_type_tex) {
      return false;
   }

   nir_tex_instr * const tex = nir_instr_as_tex(instr);
   if (tex->op != nir_texop_tg4) {
      return false;
   }

   unsigned metadata_index;
   if (state == NULL || state->metadata_map == NULL ||
       !terakan_tg4_metadata_index(state->metadata_map, tex->texture_index,
                                   &metadata_index)) {
      return false;
   }

   /* Guard tex->component to the four real channels; tg4 never
    * legally takes ZERO/ONE as the comp_arg from GLSL. */
   if (tex->component > 3u) {
      return false;
   }

   b->cursor = nir_before_instr(&tex->instr);

   /* Load the per-binding swizzle word from KCACHE bank 14.  The
    * region starts at dword 40 inside the bank, two bindings per
    * dword.  texture_index is the base descriptor slot; a runtime
    * nir_tex_src_texture_offset source, if present, selects the array
    * element within that base.
    *
    * Per AMD Evergreen-Family ISA Chapter 6: the `nir_load_kcache_r600`
    * intrinsic's `id_base` is the bank selector and `base` is the
    * vec4-element offset within the bank; the src[0] operand is the
    * runtime bank offset (held at zero here since the bank is
    * statically known).
    */
   nir_def * descriptor_slot = nir_imm_int(b, metadata_index);
   int const texture_offset_src_index =
      nir_tex_instr_src_index(tex, nir_tex_src_texture_offset);
   if (texture_offset_src_index >= 0) {
      descriptor_slot = nir_iadd(b, descriptor_slot,
                                 tex->src[texture_offset_src_index].src.ssa);
   }

   nir_def * const slot_dword_dyn = nir_ushr_imm(b, descriptor_slot, 1u);
   nir_def * swizzle_dword = nir_imm_int(b, 0x32103210u);
   for (uint32_t slot_dword = 0; slot_dword < 12u; ++slot_dword) {
      uint32_t const kcache_dword = 40u + slot_dword;
      nir_def * const candidate = nir_load_kcache_r600(
         b, 1, 32, nir_imm_zero(b, 1, 32),
         .access    = ACCESS_CAN_REORDER,
         .id_base   = TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
         .base      = kcache_dword >> 2u,
         .component = kcache_dword & 3u);
      swizzle_dword = nir_bcsel(b, nir_ieq_imm(b, slot_dword_dyn, slot_dword),
                                candidate, swizzle_dword);
   }

   /* Extract the 16 bits for this binding from the dword (low half
    * for even slots, high half for odd slots).  Slots beyond the
    * metadata table use identity swizzle from the default dword. */
   nir_def * const swizzle_word = nir_bcsel(
      b, nir_ine_imm(b, nir_iand_imm(b, descriptor_slot, 1u), 0),
      nir_ushr_imm(b, swizzle_dword, 16u), swizzle_dword);

   /* Extract the 4-bit channel target for the gather's comp_arg.
    *
    *   target = (swizzle_word >> (tex->component * 4)) & 0xf
    */
   nir_def * const channel_target =
      nir_iand_imm(b, nir_ushr_imm(b, swizzle_word, tex->component * 4u), 0xfu);

   /* Generate four candidate gather4 calls, one per real channel.
    *
    * Per AMD Evergreen-Family ISA §9.4, opcode 0x15 (GATHER4)
    * "fetches unfiltered texels from a bilinear sample and packs
    * them into xyzw".  Each candidate reads spatial samples of one
    * channel; the runtime bcsel picks the right one. */
   nir_def * const gather_r = clone_tg4_with_component(b, tex, 0u);
   nir_def * const gather_g = clone_tg4_with_component(b, tex, 1u);
   nir_def * const gather_b = clone_tg4_with_component(b, tex, 2u);
   nir_def * const gather_a = clone_tg4_with_component(b, tex, 3u);

   /* Use a precomputed-result vec4 for the ZERO and ONE constant
    * cases.  The vec4's components match the dest_type so dEQP's
    * integer / float comparisons see the right per-format value.
    *
    * For tg4 the result is always 4 components (4 corner samples),
    * and bit_size is 32 in practice for the texture_gather CTS
    * surface (32-bit dest_type for both float and integer formats).
    * Build the constant vector with explicit per-component values to
    * avoid any ambiguity in nir_replicate / nir_imm_zero handling
    * for the tex def shape.
    */
   nir_def * gather_zero;
   nir_def * gather_one;
   switch (nir_alu_type_get_base_type(tex->dest_type)) {
   case nir_type_int:
   case nir_type_uint:
      gather_zero = nir_imm_ivec4(b, 0, 0, 0, 0);
      gather_one  = nir_imm_ivec4(b, 1, 1, 1, 1);
      if (tex->def.bit_size != 32) {
         gather_zero = nir_imm_intN_t(b, 0, tex->def.bit_size);
         gather_zero = nir_replicate(b, gather_zero, tex->def.num_components);
         gather_one  = nir_imm_intN_t(b, 1, tex->def.bit_size);
         gather_one  = nir_replicate(b, gather_one, tex->def.num_components);
      }
      break;
   case nir_type_float:
   default:
      gather_zero = nir_imm_vec4(b, 0.0f, 0.0f, 0.0f, 0.0f);
      gather_one  = nir_imm_vec4(b, 1.0f, 1.0f, 1.0f, 1.0f);
      if (tex->def.bit_size != 32) {
         gather_zero = nir_imm_floatN_t(b, 0.0, tex->def.bit_size);
         gather_zero = nir_replicate(b, gather_zero, tex->def.num_components);
         gather_one  = nir_imm_floatN_t(b, 1.0, tex->def.bit_size);
         gather_one  = nir_replicate(b, gather_one, tex->def.num_components);
      }
      break;
   }

   /* bcsel cascade: select among {R, G, B, A, ZERO, ONE} based on
    * channel_target.  Order the cascade to keep the channel cases
    * on the cheap path (small int comparisons fold into single
    * cycles on Evergreen ALU). */
   nir_def * sel_r_or_g = nir_bcsel(b,
                                    nir_ieq_imm(b, channel_target,
                                                TERAKAN_TG4_TARGET_R),
                                    gather_r, gather_g);
   nir_def * sel_b_or_a = nir_bcsel(b,
                                    nir_ieq_imm(b, channel_target,
                                                TERAKAN_TG4_TARGET_B),
                                    gather_b, gather_a);
   nir_def * sel_rg_or_ba = nir_bcsel(b,
                                      nir_ult_imm(b, channel_target,
                                                  TERAKAN_TG4_TARGET_B),
                                      sel_r_or_g, sel_b_or_a);
   nir_def * or_zero = nir_bcsel(b,
                                 nir_ieq_imm(b, channel_target,
                                             TERAKAN_TG4_TARGET_ZERO),
                                 gather_zero, sel_rg_or_ba);
   nir_def * result = nir_bcsel(b,
                                nir_ieq_imm(b, channel_target,
                                            TERAKAN_TG4_TARGET_ONE),
                                gather_one, or_zero);

   nir_def_rewrite_uses(&tex->def, result);

   nir_instr_remove(&tex->instr);
   return true;
}

bool
terakan_nir_lower_tg4_view_swizzle(nir_shader * const shader,
                                   BITSET_WORD * const resources_needed,
                                   struct terakan_tg4_metadata_map const * const metadata_map)
{
   struct terakan_tg4_view_swizzle_state state = {
      .metadata_map = metadata_map,
   };
   bool const progress = nir_shader_instructions_pass(
      shader, terakan_nir_lower_tg4_view_swizzle_instr,
      nir_metadata_control_flow, &state);

   if (progress && resources_needed != NULL && metadata_map != NULL) {
      for (unsigned resource_index = 0;
           resource_index < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE;
           ++resource_index) {
         unsigned ignored_metadata_index;
         if (BITSET_TEST(resources_needed, resource_index) &&
             terakan_tg4_metadata_index(metadata_map, resource_index,
                                        &ignored_metadata_index)) {
            unsigned const gather_index =
               resource_index + TERAKAN_GATHER_DESCRIPTOR_SLOT_OFFSET;
            if (gather_index < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE)
               BITSET_SET(resources_needed, gather_index);
         }
      }
   }

   return progress;
}
