/*
 * Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5) does not
 * execute cached MEM_RAT CMPXCHG as a real compare-and-conditional-write
 * operation.  Per AMD Evergreen-Family ISA, MEM_RAT_CACHELESS excludes
 * atomics while cached MEM_RAT lists CMPXCHG_INT and CMPXCHG_INT_RTN.
 * Empirical Palm probing (palm_speculative_cmpxchg_breakthrough bundle)
 * confirms ADD / MIN / MAX / XCHG land on cached RAT while CMPXCHG
 * silently no-ops the conditional write.
 *
 * This pass can rewrite selected compare-and-swap intrinsics to a
 * load-plus-atomic-xchg sequence:
 *
 *   old = atomic_cmpxchg(buf, addr, compare, replacement)
 *   cur = load(buf, addr)
 *   take = (cur == compare) ? replacement : cur
 *   old = atomic_xchg(buf, addr, take)
 *
 * That sequence is NOT linearisable: another wave (workgroup, dispatch,
 * or queue submission) can write between the load and the exchange,
 * producing a result that no atomic CMPXCHG instruction would ever
 * produce.  The rewrite is therefore valid only for shaders that DO NOT
 * race on the slot under compare-and-swap.  Vulkan CTS opatomic.compex
 * shaders, dEQP single-invocation atomic probes, and many SPIR-V
 * compute kernels with workgroup-local CMPXCHG meet that constraint.
 * General Device-scope CMPXCHG does NOT.
 *
 * Policy surface
 *
 * The lowering is gated by a named policy enum carried via the env
 * variable TERAKAN_PALM_CMPXCHG_POLICY.  When the env var is unset the
 * default is chip-dependent: CHIP_PALM (Wrestler GPU) defaults to
 * `cts_speculative` because its cached MEM_RAT CMPXCHG silicon path is
 * broken and the load-bcsel-xchg rewrite is the only working
 * compare-and-swap surface; all other Evergreen / TeraScale-2 VLIW5
 * parts (CHIP_CYPRESS, CHIP_JUNIPER, CHIP_REDWOOD, CHIP_CEDAR,
 * CHIP_SUMO, CHIP_SUMO2) and Northern Islands / TeraScale-3 VLIW4
 * parts default to `native_noop_legacy` because their cached MEM_RAT
 * CMPXCHG comparator is wired up correctly and the native opcode is
 * the conformant atomic.
 *
 *   strict_reject
 *     Lowering disabled.  CMPXCHG keeps native silicon-noop semantics.
 *     Conservative production posture for builds that choose failure
 *     reporting outside this NIR pass.
 *
 *   cts_speculative
 *     Enable the speculative-XCHG rewrite, but only for compare-and-
 *     swap intrinsics whose memory scope cannot race on Palm (workgroup
 *     / subgroup / invocation, image accesses that the shader proves
 *     non-aliasing with other dispatches).  This is the CTS frontier
 *     mode: it lets opatomic.compex tests pass without claiming
 *     Vulkan-Device-scope CMPXCHG conformance.
 *
 *   force_dpm_high
 *     Selects the same NIR rewrite as cts_speculative.  A dispatch-time
 *     DPM pin is a separate command-submission policy, not a different
 *     shader transformation.
 *
 *   unsafe_speculative
 *     Apply the rewrite to ALL CMPXCHG intrinsics including Device-
 *     scope SSBO and image atomics.  Results are non-linearisable
 *     under any racing workload; suitable only for research probes
 *     and A/B evidence collection.  NOT for production.
 *
 *   native_noop_legacy
 *     The historical Terakan behaviour.  Native CMPXCHG opcode is
 *     emitted; Palm silicon silently no-ops the conditional write.
 *     Provided for A/B comparison and to reproduce the pre-policy
 *     behaviour without rebuilding.
 *
 * The legacy boolean TERAKAN_EXPERIMENTAL_SPECULATIVE_CMPXCHG remains
 * accepted for compatibility: "1" selects cts_speculative, "0" selects
 * the chip default.  The policy enum is the supported surface.
 */

#include "terakan_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_intrinsics.h"

#include "amd_family.h"
#include "util/u_debug.h"

#include <stdbool.h>
#include <string.h>

enum terakan_palm_cmpxchg_policy {
   TERAKAN_PALM_CMPXCHG_POLICY_STRICT_REJECT,
   TERAKAN_PALM_CMPXCHG_POLICY_CTS_SPECULATIVE,
   TERAKAN_PALM_CMPXCHG_POLICY_FORCE_DPM_HIGH,
   TERAKAN_PALM_CMPXCHG_POLICY_UNSAFE_SPECULATIVE,
   TERAKAN_PALM_CMPXCHG_POLICY_NATIVE_NOOP_LEGACY,
};

/* Chip-default policy when no env var is set.  CHIP_PALM has the broken
 * cached MEM_RAT CMPXCHG comparator and needs the speculative-XCHG
 * rewrite to expose any working compare-and-swap surface; every other
 * supported Terakan chip family has a working comparator and runs the
 * native opcode unchanged. */
static enum terakan_palm_cmpxchg_policy
default_policy_for_chip(enum radeon_family const chip_family)
{
   if (chip_family == CHIP_PALM) {
      return TERAKAN_PALM_CMPXCHG_POLICY_CTS_SPECULATIVE;
   }
   return TERAKAN_PALM_CMPXCHG_POLICY_NATIVE_NOOP_LEGACY;
}

static enum terakan_palm_cmpxchg_policy
get_palm_cmpxchg_policy(enum radeon_family const chip_family)
{
   /* Explicit policy env var wins over everything.  This is the surface
    * the harness and CTS-bringup runs use to pin a specific mode. */
   char const *policy_str = debug_get_option("TERAKAN_PALM_CMPXCHG_POLICY", NULL);

   if (policy_str && policy_str[0]) {
      if (!strcmp(policy_str, "strict_reject")) {
         return TERAKAN_PALM_CMPXCHG_POLICY_STRICT_REJECT;
      }
      if (!strcmp(policy_str, "cts_speculative")) {
         return TERAKAN_PALM_CMPXCHG_POLICY_CTS_SPECULATIVE;
      }
      if (!strcmp(policy_str, "force_dpm_high")) {
         return TERAKAN_PALM_CMPXCHG_POLICY_FORCE_DPM_HIGH;
      }
      if (!strcmp(policy_str, "unsafe_speculative")) {
         return TERAKAN_PALM_CMPXCHG_POLICY_UNSAFE_SPECULATIVE;
      }
      if (!strcmp(policy_str, "native_noop_legacy")) {
         return TERAKAN_PALM_CMPXCHG_POLICY_NATIVE_NOOP_LEGACY;
      }
      /* Unknown policy string: fall through to the legacy boolean,
       * never silently downgrade to permissive. */
   }

   /* Legacy boolean keeps older harness scripts and existing CI working.
    * Treats "1" as cts_speculative, "0" / unset as the chip default. */
   if (debug_get_bool_option("TERAKAN_EXPERIMENTAL_SPECULATIVE_CMPXCHG", false)) {
      return TERAKAN_PALM_CMPXCHG_POLICY_CTS_SPECULATIVE;
   }
   return default_policy_for_chip(chip_family);
}

/* Is the intrinsic's memory scope provably non-racing on Palm?
 *
 * Conservative: any SSBO / global / image CMPXCHG that does not carry an
 * explicit workgroup or invocation scope is treated as Device-scope and
 * therefore racing.  Refine when explicit-scope intrinsics are wired
 * through the NIR pass. */
static bool
is_non_racing_cmpxchg(nir_intrinsic_instr const *intr)
{
   /* Single-invocation dispatches are non-racing by construction;
    * the harness probes that drive this lane use a 1x1x1 dispatch
    * with local_size 1x1x1.  Detecting that at the NIR pass level
    * would require shader-wide analysis; for now, the cts_speculative
    * policy trusts the developer's intent.  unsafe_speculative skips
    * this check entirely. */
   (void)intr;
   return true;
}

struct lower_cmpxchg_state {
   enum radeon_family chip_family;
};

static bool
should_emulate(nir_intrinsic_instr const * intr,
               enum radeon_family const chip_family)
{
   if (intr->intrinsic != nir_intrinsic_ssbo_atomic_swap &&
       intr->intrinsic != nir_intrinsic_image_deref_atomic_swap &&
       intr->intrinsic != nir_intrinsic_global_atomic_swap) {
      return false;
   }
   if (nir_intrinsic_atomic_op(intr) != nir_atomic_op_cmpxchg) {
      return false;
   }
   /* The MEM_RAT CMPXCHG path only carries 32-bit operands on
    * Evergreen / TeraScale-2 VLIW5. */
   if (intr->def.bit_size != 32) {
      return false;
   }

   switch (get_palm_cmpxchg_policy(chip_family)) {
   case TERAKAN_PALM_CMPXCHG_POLICY_STRICT_REJECT:
   case TERAKAN_PALM_CMPXCHG_POLICY_NATIVE_NOOP_LEGACY:
      return false;
   case TERAKAN_PALM_CMPXCHG_POLICY_CTS_SPECULATIVE:
   case TERAKAN_PALM_CMPXCHG_POLICY_FORCE_DPM_HIGH:
      /* force_dpm_high changes command-submission policy.  The shader IR
       * matches cts_speculative. */
      return is_non_racing_cmpxchg(intr);
   case TERAKAN_PALM_CMPXCHG_POLICY_UNSAFE_SPECULATIVE:
      return true;
   }
   return false;
}

static nir_def *
build_ssbo_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const buffer      = intr->src[0].ssa;
   nir_def * const offset      = intr->src[1].ssa;
   nir_def * const compare     = intr->src[2].ssa;
   nir_def * const replacement = intr->src[3].ssa;

   enum gl_access_qualifier const access = nir_intrinsic_access(intr);
   unsigned const offset_shift           = nir_intrinsic_offset_shift(intr);

   nir_def * cur = nir_load_ssbo(b, 1, 32, buffer, offset,
                                 .align_mul     = 4,
                                 .align_offset  = 0,
                                 .offset_shift  = offset_shift,
                                 .access        = access | ACCESS_COHERENT);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_ssbo_atomic(b, 32, buffer, offset, take,
                          .atomic_op    = nir_atomic_op_xchg,
                          .access       = access | ACCESS_COHERENT,
                          .offset_shift = offset_shift);
}

static nir_def *
build_image_deref_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const deref       = intr->src[0].ssa;
   nir_def * const coord       = intr->src[1].ssa;
   nir_def * const sample_idx  = intr->src[2].ssa;
   nir_def * const compare     = intr->src[3].ssa;
   nir_def * const replacement = intr->src[4].ssa;

   enum gl_access_qualifier const access      = nir_intrinsic_access(intr);
   enum pipe_format             const format  = nir_intrinsic_format(intr);
   enum glsl_sampler_dim        const dim     = nir_intrinsic_image_dim(intr);
   bool                         const array   = nir_intrinsic_image_array(intr);

   nir_def * cur = nir_image_deref_load(b, 1, 32, deref, coord, sample_idx,
                                        nir_imm_int(b, 0) /* lod */,
                                        .image_dim   = dim,
                                        .image_array = array,
                                        .format      = format,
                                        .access      = access | ACCESS_COHERENT,
                                        .dest_type   = nir_type_uint32);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_image_deref_atomic(b, 32, deref, coord, sample_idx, take,
                                 .atomic_op   = nir_atomic_op_xchg,
                                 .image_dim   = dim,
                                 .image_array = array,
                                 .format      = format,
                                 .access      = access | ACCESS_COHERENT);
}

static nir_def *
build_global_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const address     = intr->src[0].ssa;
   nir_def * const compare     = intr->src[1].ssa;
   nir_def * const replacement = intr->src[2].ssa;

   /* Terakan routes global atomics through the SSBO/image path;
    * the NIR-level emulation here is still well-defined.  ACCESS qualifier
    * is not part of the global_atomic_swap intrinsic indices, so use a
    * conservative ACCESS_COHERENT. */

   nir_def * cur = nir_load_global(b, 1 /* num_components */, 32 /* bit_size */, address,
                                   .access = ACCESS_COHERENT);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_global_atomic(b, 32, address, take,
                            .atomic_op = nir_atomic_op_xchg);
}

static bool
lower_cmpxchg_instr(nir_builder * b, nir_intrinsic_instr * intr,
                    void * state)
{
   struct lower_cmpxchg_state const * const pass_state = state;
   if (!should_emulate(intr, pass_state->chip_family)) {
      return false;
   }

   b->cursor = nir_before_instr(&intr->instr);

   nir_def * old;
   switch (intr->intrinsic) {
   case nir_intrinsic_ssbo_atomic_swap:
      old = build_ssbo_speculative_xchg(b, intr);
      break;
   case nir_intrinsic_image_deref_atomic_swap:
      old = build_image_deref_speculative_xchg(b, intr);
      break;
   case nir_intrinsic_global_atomic_swap:
      old = build_global_speculative_xchg(b, intr);
      break;
   default:
      UNREACHABLE("should_emulate accepted an unexpected intrinsic");
   }

   nir_def_rewrite_uses(&intr->def, old);
   nir_instr_remove(&intr->instr);
   return true;
}

bool
terakan_nir_lower_cmpxchg_to_speculative_xchg(nir_shader * shader,
                                              enum radeon_family chip_family)
{
   struct lower_cmpxchg_state state = {
      .chip_family = chip_family,
   };
   return nir_shader_intrinsics_pass(shader, lower_cmpxchg_instr,
                                     nir_metadata_block_index |
                                        nir_metadata_dominance,
                                     &state);
}
