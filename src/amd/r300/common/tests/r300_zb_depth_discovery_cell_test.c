/* SPDX-License-Identifier: MIT */

/* Asserts carry this suite's verdicts, and a release profile compiles
 * them out through NDEBUG, so the definition is removed here before any
 * header reaches the preprocessor. */
#undef NDEBUG

#include "r300_zb_depth_discovery_cell.h"

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The three arms the campaign emits from one emitter: the discovery
 * draw, and the two controls whose whole difference is the comparison
 * and the write enable. */
struct arm {
   const char *name;
   uint32_t depth_function;
   bool depth_write;
};

static const struct arm arms[] = {
   { "discovery", R300_ZS_ALWAYS, true },
   { "writes-disabled", R300_ZS_ALWAYS, false },
   { "never", R300_ZS_NEVER, true },
};

static void
test_emits_and_checks_its_own_state(void)
{
   for (size_t i = 0; i < sizeof(arms) / sizeof(*arms); i++) {
      struct r300_zb_depth_discovery_ib ib;
      assert(r300_zb_depth_discovery_reference_emit(
                &r300_zb_depth_discovery_z24_linear, arms[i].depth_function,
                arms[i].depth_write, &ib) == 0);
      assert(ib.ib != NULL && ib.ib_size_dwords > 0u);
      assert(r300_zb_depth_discovery_validate_reloc_sites(&ib) == 0);

      struct r300_zb_depth_discovery_state state;
      assert(r300_zb_depth_discovery_read_state(ib.ib, ib.ib_size_dwords,
                                                &state) == 0);

      /* Exactly one ZB_FORMAT write, so no later write can move the
       * format the parser and the hardware act on. */
      assert(state.zb_format_writes == 1u);
      assert(state.zb_format ==
             r300_zb_depth_discovery_z24_linear.surface->depth_format);

      /* The executing compression state is this stream's ZB_BW_CNTL,
       * written after the contract's entry for the same register.  Zero
       * is HiZ, fast fill, read and write compression, and ZB_CB_CLEAR
       * all clear. */
      assert(state.zb_bw_cntl == 0u);
      /* Plane equations stay 4x4 while compression is disabled. */
      assert(state.gb_z_peq_config == 0u);
      assert(state.sc_screendoor == 0x00ffffffu);

      assert((state.zb_cntl & R300_Z_ENABLE) != 0u);
      assert(((state.zb_cntl & R300_Z_WRITE_ENABLE) != 0u) ==
             arms[i].depth_write);
      assert(((state.zb_zstencilcntl >> R300_Z_FUNC_SHIFT) & R300_ZS_MASK) ==
             arms[i].depth_function);

      /* The scissor confines to the declared pixel and the clip
       * rectangle stays wider. */
      uint32_t expected = 0;
      assert(r300_first_draw_scissor_word(
                r300_zb_depth_discovery_z24_linear.pixel_x,
                r300_zb_depth_discovery_z24_linear.pixel_y, &expected) == 0);
      assert(state.sc_scissors_tl == expected);
      assert(state.sc_scissors_br == expected);
      assert(state.sc_cliprect_tl == (1440u | (1440u << 13)));

      r300_zb_depth_discovery_release(&ib);
   }
}

/* The poison-model checker over the cell's own contract.  Arming the
 * depth comparison is the cell's one departure from it: ZB_ZSTENCILCNTL
 * carries the comparison and the contract resolves it to the disabled
 * zero, so an ALWAYS arm departs there and a NEVER arm -- whose encoding
 * is zero -- reproduces the contract word and departs nowhere.  The
 * depth resource words carry the REFERENCE_ARTIFACT disposition and
 * never reach the contract, and ZB_BW_CNTL's HIZ_DISABLE and
 * FAST_FILL_DISABLE are zero-valued encodings, so the cell's write
 * reproduces the contract's disabled word.  Every remaining clause holds
 * its contract value at the draw boundary and at the end of the stream,
 * the one-pixel scissor included: the contract owns that scissor, so the
 * confinement the checker verifies is the confinement the stream emits.
 */
static void
test_contract_departure(void)
{
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_discovery_reference_contract(
             &r300_zb_depth_discovery_z24_linear, &contract) == 0);

   for (size_t a = 0; a < sizeof(arms) / sizeof(*arms); a++) {
      struct r300_zb_depth_discovery_ib ib;
      assert(r300_zb_depth_discovery_reference_emit(
                &r300_zb_depth_discovery_z24_linear, arms[a].depth_function,
                arms[a].depth_write, &ib) == 0);

      const uint32_t expected_departures =
         arms[a].depth_function == R300_ZS_NEVER ? 0u : 1u;
      struct r300_first_draw_check_report report;
      const uint32_t poisons[] = { 0x00000000u, 0xffffffffu, 0xdeadbeefu };
      for (size_t i = 0; i < sizeof(poisons) / sizeof(*poisons); i++) {
         const uint32_t unsatisfied = r300_first_draw_state_check(
            &contract, ib.ib, ib.ib_size_dwords, poisons[i], &report);
         assert(unsatisfied == expected_departures);
         for (uint32_t k = 0; k < report.unsatisfied_count; k++)
            assert(contract.entries[report.unsatisfied[k]].reg ==
                   R300_ZB_ZSTENCILCNTL);
      }
      r300_zb_depth_discovery_release(&ib);
   }
}

/* Building the reference cell from each scenario, so a tiled rung emits
 * rather than being refused by a capability the discovery contract does
 * not require. */
static void
test_every_scenario_emits(void)
{
   const struct r300_zb_depth_discovery_scenario *scenarios[] = {
      &r300_zb_depth_discovery_z24_linear,
      &r300_zb_depth_discovery_z24_linear_seed_5a,
      &r300_zb_depth_discovery_z24_linear_seed_a5,
      &r300_zb_depth_discovery_z24_microtiled,
      &r300_zb_depth_discovery_z24_macrotiled,
   };
   uint32_t linear_stream[R300_ZB_DISCOVERY_MAX_DWORDS];
   uint32_t linear_dwords = 0;

   for (size_t i = 0; i < sizeof(scenarios) / sizeof(*scenarios); i++) {
      struct r300_zb_depth_discovery_ib ib;
      assert(r300_zb_depth_discovery_reference_emit(scenarios[i],
                                                    R300_ZS_ALWAYS, true,
                                                    &ib) == 0);
      struct r300_zb_depth_discovery_params params = {
         .scenario = scenarios[i],
         .depth_function = R300_ZS_ALWAYS,
         .depth_write = true,
      };
      assert(r300_zb_depth_discovery_check_state(&params, ib.ib,
                                                 ib.ib_size_dwords) == 0);

      /* The three linear scenarios differ in their host fill alone, so
       * their streams are byte-identical and no IB digest separates the
       * two stencil seeds from each other or from the unseeded run.
       * That is why the retained artifact carries an initial-image
       * digest of its own. */
      if (i == 0) {
         linear_dwords = ib.ib_size_dwords;
         memcpy(linear_stream, ib.ib, linear_dwords * sizeof(uint32_t));
      } else if (i < 3) {
         assert(ib.ib_size_dwords == linear_dwords);
         assert(memcmp(linear_stream, ib.ib,
                       linear_dwords * sizeof(uint32_t)) == 0);
      } else {
         /* A tiled rung differs from the linear one: same dword count,
          * differing DEPTHPITCH tile bits. */
         assert(ib.ib_size_dwords == linear_dwords);
         assert(memcmp(linear_stream, ib.ib,
                       linear_dwords * sizeof(uint32_t)) != 0);
      }
      r300_zb_depth_discovery_release(&ib);
   }
}

/* Known-bads for the state checker.  Each mutates one dword of a stream
 * the checker admitted, so a checker that stopped reading a register is
 * caught by the arm that names it. */
/* The surface origin the stream programs, against an expectation stated
 * here rather than taken from the emitter.
 *
 * ZB_DEPTHOFFSET carries a byte offset inside the buffer object and the
 * kernel adds the object's address, so the register is what decides
 * where the device's surface begins.  The host initializes storage at
 * the layout's base and the observation reads it there; a stream
 * programming a different origin binds the device somewhere else.  For
 * the campaign's linear scenario the two candidate origins are 2048 and
 * 0 bytes, and at the coordinate (37, 21) on a 64-pixel row of four-byte
 * pixels they place the write at
 *
 *   2048 + 21 * 256 + 37 * 4 = 7572 = 0x1d94   the declared surface
 *          21 * 256 + 37 * 4 = 5524 = 0x1594   a surface based at zero
 *
 * Both lie inside the declared storage envelope [2048, 18688), so a
 * write at the wrong origin leaves both guards intact and satisfies the
 * observation's one-changed-slot condition.  A clean observation
 * therefore carries no evidence about the origin, and the binding is
 * checked against these literals instead.
 */
static void
test_surface_origin_binding(void)
{
   const struct r300_zb_depth_discovery_scenario *scenario =
      &r300_zb_depth_discovery_z24_linear;

   /* The two addresses above, computed here from the coordinate and the
    * surface geometry, with no call into the layout helper. */
   const uint32_t row_bytes = 64u * 4u;
   const uint32_t declared_base = 2048u;
   const uint32_t declared_address =
      declared_base + 21u * row_bytes + 37u * 4u;
   const uint32_t zero_based_address = 21u * row_bytes + 37u * 4u;
   assert(declared_address == 0x1d94u);
   assert(zero_based_address == 0x1594u);
   assert(scenario->pixel_x == 37u && scenario->pixel_y == 21u);

   /* Both candidates fall inside the declared envelope, which is why a
    * one-changed-slot observation cannot separate them. */
   struct r300_zb_depth_layout layout;
   assert(r300_zb_depth_discovery_layout(scenario, &layout) == 0);
   assert(layout.base_offset_bytes == declared_base);
   assert(declared_address >= layout.base_offset_bytes &&
          declared_address < layout.base_offset_bytes + layout.storage_bytes);
   assert(zero_based_address >= layout.base_offset_bytes &&
          zero_based_address <
             layout.base_offset_bytes + layout.storage_bytes);

   /* Every scenario programs its own layout's base, and the checker
    * agrees. */
   const struct r300_zb_depth_discovery_scenario *scenarios[] = {
      &r300_zb_depth_discovery_z24_linear,
      &r300_zb_depth_discovery_z24_linear_seed_5a,
      &r300_zb_depth_discovery_z24_linear_seed_a5,
      &r300_zb_depth_discovery_z24_microtiled,
      &r300_zb_depth_discovery_z24_macrotiled,
   };
   for (size_t i = 0; i < sizeof(scenarios) / sizeof(*scenarios); i++) {
      struct r300_zb_depth_discovery_ib ib;
      assert(r300_zb_depth_discovery_reference_emit(scenarios[i],
                                                    R300_ZS_ALWAYS, true,
                                                    &ib) == 0);
      struct r300_zb_depth_discovery_state state;
      assert(r300_zb_depth_discovery_read_state(ib.ib, ib.ib_size_dwords,
                                                &state) == 0);
      struct r300_zb_depth_layout own;
      assert(r300_zb_depth_discovery_layout(scenarios[i], &own) == 0);
      assert(state.zb_depthoffset_writes == 1u);
      assert(state.zb_depthoffset == own.base_offset_bytes);
      assert(state.zb_depthoffset == declared_base);
      /* The macrotiled rung needs its base on a 2048-byte macrotile, and
       * every rung needs the low five bits ZB_DEPTHOFFSET does not
       * encode left clear. */
      assert(state.zb_depthoffset % own.base_alignment_bytes == 0u);
      assert((state.zb_depthoffset & 0x1fu) == 0u);

      /* The pitch word carries the row width, both tile fields, and the
       * endian selector, so a tiled rung differs from the linear one
       * here and not only in an offset. */
      assert(state.zb_depthpitch_writes == 1u);
      assert((state.zb_depthpitch & R300_DEPTHPITCH_MASK) == 64u);
      assert((state.zb_depthpitch & (7u << 16)) ==
             r300_zb_depth_surface_tile_bits(scenarios[i]->surface));
      r300_zb_depth_discovery_release(&ib);
   }
}

/* The known-bad the repair exists for: a stream identical to the
 * reference except that the depth base is zero.  It is built through the
 * emitter's parameter rather than by editing a dword, so it is the
 * stream the pre-repair reference emitter produced, and the checker must
 * refuse it.  Comparing the recorder against the reference emitter would
 * not have caught this, because both named the same wrong origin.
 */
static void
test_zero_depth_base_refused(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_discovery_reference_contract(
             &r300_zb_depth_discovery_z24_linear, &contract) == 0);

   const struct r300_zb_depth_discovery_params zero_base = {
      .scenario = &r300_zb_depth_discovery_z24_linear,
      .vertex_offset = 0,
      .color_pitch_format =
         r300_rb3d_colorpitch0_pack_argb8888(R300_ZB_DISCOVERY_PITCH_PIXELS),
      .depth_offset_bytes = 0,
      .depth_function = R300_ZS_ALWAYS,
      .depth_write = true,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   uint32_t words[R300_ZB_DISCOVERY_MAX_DWORDS];
   struct r300_zb_depth_discovery_ib ib;
   assert(r300_zb_depth_discovery_emit_into(&zero_base, words,
                                            sizeof(words) / 4u, &ib) == 0);

   struct r300_zb_depth_discovery_state state;
   assert(r300_zb_depth_discovery_read_state(ib.ib, ib.ib_size_dwords,
                                             &state) == 0);
   assert(state.zb_depthoffset == 0u);
   assert(r300_zb_depth_discovery_check_state(&zero_base, ib.ib,
                                              ib.ib_size_dwords) != 0);

   /* The same parameters at the resolved base are admitted, so the
    * refusal above names the origin and not some other difference. */
   struct r300_zb_depth_discovery_params good_base = zero_base;
   good_base.depth_offset_bytes = 2048u;
   assert(r300_zb_depth_discovery_emit_into(&good_base, words,
                                            sizeof(words) / 4u, &ib) == 0);
   assert(r300_zb_depth_discovery_check_state(&good_base, ib.ib,
                                              ib.ib_size_dwords) == 0);

   /* A base one macrotile past the declared one is inside the
    * allocation and still refused: the check is equality with the
    * layout, not a range test. */
   struct r300_zb_depth_discovery_params shifted = zero_base;
   shifted.depth_offset_bytes = 4096u;
   assert(r300_zb_depth_discovery_emit_into(&shifted, words,
                                            sizeof(words) / 4u, &ib) == 0);
   assert(r300_zb_depth_discovery_check_state(&shifted, ib.ib,
                                              ib.ib_size_dwords) != 0);

   r300_fragment_binary_finish(&fs);
}

static void
test_state_checker_refusals(void)
{
   struct r300_zb_depth_discovery_ib ib;
   assert(r300_zb_depth_discovery_reference_emit(
             &r300_zb_depth_discovery_z24_linear, R300_ZS_ALWAYS, true,
             &ib) == 0);

   const struct r300_zb_depth_discovery_params good = {
      .scenario = &r300_zb_depth_discovery_z24_linear,
      .depth_function = R300_ZS_ALWAYS,
      .depth_write = true,
   };
   assert(r300_zb_depth_discovery_check_state(&good, ib.ib,
                                              ib.ib_size_dwords) == 0);

   uint32_t stream[R300_ZB_DISCOVERY_MAX_DWORDS];
   const uint32_t dwords = ib.ib_size_dwords;
   assert(dwords + 3u <= R300_ZB_DISCOVERY_MAX_DWORDS);

   /* Finds the payload dword of the single-register PACKET0 write to
    * reg, so a mutation names a register rather than an offset. */
   uint32_t index_of[16];
   uint32_t index_count = 0;
   const uint32_t watched[] = {
      R300_ZB_BW_CNTL,    R300_GB_Z_PEQ_CONFIG, R300_SC_SCREENDOOR,
      R300_SC_SCISSORS_BR, R300_ZB_CNTL,        R300_ZB_ZSTENCILCNTL,
      R300_ZB_FORMAT,     R300_ZB_DEPTHOFFSET,  R300_ZB_DEPTHPITCH,
   };
   for (size_t w = 0; w < sizeof(watched) / sizeof(*watched); w++) {
      uint32_t found = UINT32_MAX;
      for (uint32_t i = 0; i + 1u < dwords; i++)
         if (ib.ib[i] == ((0u << 30) | ((watched[w] >> 2) & 0x1fffu)))
            found = i + 1u;
      assert(found != UINT32_MAX);
      index_of[index_count++] = found;
   }

   /* One mutated payload per watched register, each refused. */
   for (uint32_t m = 0; m < index_count; m++) {
      memcpy(stream, ib.ib, dwords * sizeof(uint32_t));
      stream[index_of[m]] ^= 0x1u;
      assert(r300_zb_depth_discovery_check_state(&good, stream, dwords) != 0);
   }

   /* ZB_CB_CLEAR alone, the bit whose set state makes a partially
    * written microtile's untouched portion unknown. */
   memcpy(stream, ib.ib, dwords * sizeof(uint32_t));
   stream[index_of[0]] |= R300_ZB_CB_CLEAR_CACHE_LINE_WRITE_ONLY;
   assert(r300_zb_depth_discovery_check_state(&good, stream, dwords) != 0);

   /* 8x8 plane equations while compression is disabled. */
   memcpy(stream, ib.ib, dwords * sizeof(uint32_t));
   stream[index_of[1]] = R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_8_8;
   assert(r300_zb_depth_discovery_check_state(&good, stream, dwords) != 0);

   /* The dword index of the draw packet, which is where the state the
    * hardware executes under stops being written. */
   uint32_t draw_index = UINT32_MAX;
   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib.ib[i];
      if (r300_first_draw_is_draw_packet(header)) {
         draw_index = i;
         break;
      }
      if ((header >> 30) == 2u) {
         i += 1u;
         continue;
      }
      i += ((header >> 16) & 0x3fffu) + 2u;
   }
   assert(draw_index != UINT32_MAX);

   /* A second ZB_FORMAT write ahead of the draw.  The one-write grammar
    * is what makes the effective format unambiguous, so a stream
    * carrying two before the draw is refused rather than read as its
    * first or its last. */
   uint32_t extended[R300_ZB_DISCOVERY_MAX_DWORDS + 2u];
   memcpy(extended, ib.ib, draw_index * sizeof(uint32_t));
   extended[draw_index] = (0u << 30) | ((R300_ZB_FORMAT >> 2) & 0x1fffu);
   extended[draw_index + 1u] =
      r300_zb_depth_discovery_z24_linear.surface->depth_format;
   memcpy(extended + draw_index + 2u, ib.ib + draw_index,
          (dwords - draw_index) * sizeof(uint32_t));
   assert(r300_zb_depth_discovery_check_state(&good, extended,
                                              dwords + 2u) != 0);

   /* The same write placed after the draw is admitted, because the draw
    * executed under the state standing when it was reached.  This is the
    * positive half of the draw-boundary rule: without it, a checker that
    * simply refused every second write would pass the arm above for a
    * reason that has nothing to do with when the write lands. */
   memcpy(extended, ib.ib, dwords * sizeof(uint32_t));
   extended[dwords] = (0u << 30) | ((R300_ZB_FORMAT >> 2) & 0x1fffu);
   extended[dwords + 1u] =
      r300_zb_depth_discovery_z24_linear.surface->depth_format;
   assert(r300_zb_depth_discovery_check_state(&good, extended,
                                              dwords + 2u) == 0);

   /* A compression enable set before the draw and cleared after it.  The
    * terminal state is the admitted zero and the draw executed under
    * ZB_CB_CLEAR, which is exactly what a terminal-state reader would
    * miss. */
   memcpy(extended, ib.ib, draw_index * sizeof(uint32_t));
   extended[draw_index] = (0u << 30) | ((R300_ZB_BW_CNTL >> 2) & 0x1fffu);
   extended[draw_index + 1u] = R300_ZB_CB_CLEAR_CACHE_LINE_WRITE_ONLY;
   memcpy(extended + draw_index + 2u, ib.ib + draw_index,
          (dwords - draw_index) * sizeof(uint32_t));
   assert(r300_zb_depth_discovery_check_state(&good, extended,
                                              dwords + 2u) != 0);

   /* A stream that establishes every register and never draws executes
    * none of it. */
   assert(r300_zb_depth_discovery_check_state(&good, ib.ib, draw_index) != 0);

   /* RADEON_ONE_REG_WR holds the base register across every payload
    * dword.  A two-dword repeated run at ZB_BW_CNTL writes that register
    * twice and reaches no other, so a walk that advanced the base would
    * assign the second payload to GB_Z_PEQ_CONFIG's neighbor and leave
    * ZB_BW_CNTL at the first value.  The compression enable is in the
    * second payload, so only a walk that honors the flag sees it. */
   memcpy(extended, ib.ib, draw_index * sizeof(uint32_t));
   extended[draw_index] = (0u << 30) | (1u << 16) | RADEON_ONE_REG_WR |
                          ((R300_ZB_BW_CNTL >> 2) & 0x1fffu);
   extended[draw_index + 1u] = 0u;
   extended[draw_index + 2u] = R300_ZB_CB_CLEAR_CACHE_LINE_WRITE_ONLY;
   memcpy(extended + draw_index + 3u, ib.ib + draw_index,
          (dwords - draw_index) * sizeof(uint32_t));
   assert(r300_zb_depth_discovery_check_state(&good, extended,
                                              dwords + 3u) != 0);

   /* The pitch word's fields are judged apart from its numeric row
    * width.  Each mutation keeps the pitch at 64 pixels and moves one
    * other field, which is the class a width-only comparison would
    * admit: a stream agreeing on pixels per row and disagreeing on how
    * those pixels are arranged in memory. */
   const uint32_t pitch_index = index_of[8];
   assert((ib.ib[pitch_index] & R300_DEPTHPITCH_MASK) == 64u);
   const uint32_t pitch_field_mutations[] = {
      R300_DEPTHMACROTILE_ENABLE,
      R300_DEPTHMICROTILE(1u),
      R300_DEPTHMICROTILE(2u),
      R300_DEPTHENDIAN(1u),
   };
   for (size_t m = 0;
        m < sizeof(pitch_field_mutations) / sizeof(*pitch_field_mutations);
        m++) {
      memcpy(stream, ib.ib, dwords * sizeof(uint32_t));
      stream[pitch_index] |= pitch_field_mutations[m];
      assert((stream[pitch_index] & R300_DEPTHPITCH_MASK) == 64u);
      assert(r300_zb_depth_discovery_check_state(&good, stream, dwords) != 0);
   }

   /* A second write to either resource register ahead of the draw leaves
    * the surface the draw resolves ambiguous, the same way a second
    * ZB_FORMAT write does. */
   const uint32_t resource_registers[] = { R300_ZB_DEPTHOFFSET,
                                           R300_ZB_DEPTHPITCH };
   for (size_t m = 0;
        m < sizeof(resource_registers) / sizeof(*resource_registers); m++) {
      memcpy(extended, ib.ib, draw_index * sizeof(uint32_t));
      extended[draw_index] =
         (0u << 30) | ((resource_registers[m] >> 2) & 0x1fffu);
      extended[draw_index + 1u] =
         resource_registers[m] == R300_ZB_DEPTHOFFSET
            ? 2048u
            : ib.ib[pitch_index];
      memcpy(extended + draw_index + 2u, ib.ib + draw_index,
             (dwords - draw_index) * sizeof(uint32_t));
      /* The repeated value is the admitted one, so the refusal names the
       * second write rather than a wrong payload. */
      assert(r300_zb_depth_discovery_check_state(&good, extended,
                                                 dwords + 2u) != 0);
   }

   /* A type-1 packet has no R300 encoding, and a payload running past
    * the stream is malformed; neither yields a state to judge. */
   struct r300_zb_depth_discovery_state state_out;
   const uint32_t type1 = 1u << 30;
   assert(r300_zb_depth_discovery_read_state(&type1, 1u, &state_out) != 0);
   const uint32_t overrun = (0u << 30) | (7u << 16) |
                            ((R300_ZB_BW_CNTL >> 2) & 0x1fffu);
   assert(r300_zb_depth_discovery_read_state(&overrun, 1u, &state_out) != 0);

   /* The declared arm and the emitted arm disagree. */
   struct r300_zb_depth_discovery_params wrong = good;
   wrong.depth_write = false;
   assert(r300_zb_depth_discovery_check_state(&wrong, ib.ib, dwords) != 0);
   wrong = good;
   wrong.depth_function = R300_ZS_NEVER;
   assert(r300_zb_depth_discovery_check_state(&wrong, ib.ib, dwords) != 0);

   /* The declared pixel and the emitted scissor disagree, which is what
    * separates a scissor read back from the stream from a scissor the
    * experiment declared. */
   wrong = good;
   wrong.scenario = &r300_zb_depth_discovery_z24_microtiled;
   struct r300_zb_depth_discovery_scenario moved =
      r300_zb_depth_discovery_z24_linear;
   moved.pixel_x = 38u;
   wrong.scenario = &moved;
   assert(r300_zb_depth_discovery_check_state(&wrong, ib.ib, dwords) != 0);

   r300_zb_depth_discovery_release(&ib);
}

static void
test_emit_refusals(void)
{
   struct r300_fragment_binary fs;
   assert(r300_tcl_bypass_triangle_reference_fs(&fs) == 0);
   struct r300_first_draw_contract contract;
   assert(r300_zb_depth_discovery_reference_contract(
             &r300_zb_depth_discovery_z24_linear, &contract) == 0);

   const struct r300_zb_depth_discovery_params good = {
      .scenario = &r300_zb_depth_discovery_z24_linear,
      .color_pitch_format =
         r300_rb3d_colorpitch0_pack_argb8888(R300_ZB_DISCOVERY_PITCH_PIXELS),
      .depth_function = R300_ZS_ALWAYS,
      .depth_write = true,
      .fragment_binary = &fs,
      .first_draw_contract = &contract,
   };
   uint32_t words[R300_ZB_DISCOVERY_MAX_DWORDS];
   struct r300_zb_depth_discovery_ib ib;
   assert(r300_zb_depth_discovery_emit_into(&good, words, sizeof(words) / 4u,
                                            &ib) == 0);
   const uint32_t needed = ib.ib_size_dwords;

   struct r300_zb_depth_discovery_params bad;

   assert(r300_zb_depth_discovery_emit_into(NULL, words, needed, &ib) ==
          -EINVAL);
   assert(r300_zb_depth_discovery_emit_into(&good, NULL, needed, &ib) ==
          -EINVAL);

   /* A destination one dword short refuses rather than emitting a short
    * stream. */
   assert(r300_zb_depth_discovery_emit_into(&good, words, needed - 1u, &ib) ==
          -ENOSPC);
   assert(ib.ib == NULL && ib.ib_size_dwords == 0u);

   bad = good;
   bad.first_draw_contract = NULL;
   assert(r300_zb_depth_discovery_emit_into(&bad, words, needed, &ib) != 0);

   bad = good;
   bad.fragment_binary = NULL;
   assert(r300_zb_depth_discovery_emit_into(&bad, words, needed, &ib) != 0);

   bad = good;
   bad.scenario = NULL;
   assert(r300_zb_depth_discovery_emit_into(&bad, words, needed, &ib) != 0);

   bad = good;
   bad.depth_function = R300_ZS_ALWAYS + 1u;
   assert(r300_zb_depth_discovery_emit_into(&bad, words, needed, &ib) != 0);

   /* A scenario whose surface names other geometry: one layout would be
    * emitted and another read. */
   struct r300_zb_depth_surface narrow = *r300_zb_depth_discovery_z24_linear.surface;
   narrow.width = 32u;
   struct r300_zb_depth_discovery_scenario narrow_scenario =
      r300_zb_depth_discovery_z24_linear;
   narrow_scenario.surface = &narrow;
   bad = good;
   bad.scenario = &narrow_scenario;
   assert(r300_zb_depth_discovery_emit_into(&bad, words, needed, &ib) != 0);

   r300_fragment_binary_finish(&fs);
}

int
main(void)
{
   test_emits_and_checks_its_own_state();
   test_contract_departure();
   test_every_scenario_emits();
   test_surface_origin_binding();
   test_zero_depth_base_refused();
   test_state_checker_refusals();
   test_emit_refusals();
   printf("r300_zb_depth_discovery_cell_test: ok\n");
   return 0;
}
