/*
 * SPDX-License-Identifier: MIT
 *
 * Structure, register parity, refusal, composition, and digest checks
 * over the fetched R2VB producer and its composed route.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_pm4_compose.h"
#include "r300_r2vb_fetch_pass.h"
#include "r300_r2vb_fetched_producer.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"
#include "tests/r300_fetched_route_digests.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Publication tail: four one-dword register writes. */
#define TAIL_DWORDS 8u

static uint32_t
float_bits(float f)
{
   uint32_t bits;
   memcpy(&bits, &f, sizeof(bits));
   return bits;
}

/* Last-written value per register over a PM4 stream, plus the PACKET3
 * opcodes seen.  The register space is 64 KiB of dword-addressed
 * registers, so a 16K-entry table covers every PACKET0 target.
 */
struct register_map {
   uint32_t value[0x4000];
   bool written[0x4000];
   uint32_t opcode_seen[64];
   uint32_t opcode_count;
};

static void
walk(const uint32_t *words, uint32_t count, struct register_map *map)
{
   memset(map, 0, sizeof(*map));
   uint32_t i = 0;
   while (i < count) {
      const uint32_t header = words[i];
      const uint32_t type = header >> 30;
      const uint32_t payload = ((header >> 16) & 0x3fffu) + 1u;
      assert(type == 0 || type == 3);
      assert(i + 1u + payload <= count);
      if (type == 0) {
         const uint32_t reg = (header & 0xffffu);
         const bool one_reg = (header & (1u << 15)) != 0;
         for (uint32_t k = 0; k < payload; k++) {
            const uint32_t index = one_reg ? reg : reg + k;
            assert(index < 0x4000);
            map->value[index] = words[i + 1u + k];
            map->written[index] = true;
         }
      } else {
         const uint32_t opcode = header & 0xff00u;
         bool seen = false;
         for (uint32_t k = 0; k < map->opcode_count; k++)
            seen |= map->opcode_seen[k] == opcode;
         if (!seen) {
            assert(map->opcode_count < ARRAY_SIZE(map->opcode_seen));
            map->opcode_seen[map->opcode_count++] = opcode;
         }
      }
      i += 1u + payload;
   }
   assert(i == count);
}

static bool
opcode_present(const struct register_map *map, uint32_t opcode)
{
   for (uint32_t k = 0; k < map->opcode_count; k++) {
      if (map->opcode_seen[k] == opcode)
         return true;
   }
   return false;
}

static uint32_t
expected_source_swizzle(int format_id)
{
   const struct r300_vertex_format_semantics *fmt =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   assert(fmt != NULL);
   return ((uint32_t)fmt->select[2] << R300_SWIZZLE_SELECT_X_SHIFT) |
          ((uint32_t)fmt->select[1] << R300_SWIZZLE_SELECT_Y_SHIFT) |
          ((uint32_t)fmt->select[0] << R300_SWIZZLE_SELECT_Z_SHIFT) |
          ((uint32_t)fmt->select[3] << R300_SWIZZLE_SELECT_W_SHIFT) |
          (0xfu << R300_WRITE_ENA_SHIFT);
}

static void
test_fetch_state(void)
{
   struct r300_r2vb_fetch_state st;

   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_F32_4,
                                                 &st) == 0);
   assert(st.fetch_dwords == 8);
   assert((st.vap_prog_stream_cntl[0] & 0xffffu) == R300_DATA_TYPE_FLOAT_4);
   assert((st.vap_prog_stream_cntl[0] >> 16) ==
          (R300_DATA_TYPE_FLOAT_4 | (6u << R300_DST_VEC_LOC_SHIFT) |
           R300_LAST_VEC));
   assert((st.vap_prog_stream_cntl_ext[0] & 0xffffu) == R300_VAP_SWIZZLE_XYZW);
   /* (z, y, x, w): Z in lane 0, Y in lane 1, X in lane 2, W in lane 3. */
   const uint32_t zyxw =
      (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_X_SHIFT) |
      (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
      (R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_Z_SHIFT) |
      (R300_SWIZZLE_SELECT_W << R300_SWIZZLE_SELECT_W_SHIFT) |
      (0xfu << R300_WRITE_ENA_SHIFT);
   assert((st.vap_prog_stream_cntl_ext[0] >> 16) == zyxw);
   assert((st.vap_prog_stream_cntl_ext[0] >> 16) ==
          expected_source_swizzle(R300_VERTEX_FORMAT_F32_4));
   for (unsigned i = 1; i < 8; i++) {
      assert(st.vap_prog_stream_cntl[i] == 0);
      assert(st.vap_prog_stream_cntl_ext[i] == 0);
   }
   assert(st.vap_vtx_state_cntl == 0x5555);
   assert(st.vap_vsm_vtx_assm == (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0));
   assert(st.vap_out_vtx_fmt[0] == R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);
   assert(st.vap_out_vtx_fmt[1] == R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS);
   assert(st.gb_enable == 0);
   assert(st.rs_count == (R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN));
   assert(st.rs_inst_count == 0);

   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_F32_3,
                                                 &st) == 0);
   assert(st.fetch_dwords == 7);
   assert((st.vap_prog_stream_cntl[0] >> 16) ==
          (R300_DATA_TYPE_FLOAT_3 | (6u << R300_DST_VEC_LOC_SHIFT) |
           R300_LAST_VEC));
   /* (z, y, x, 1). */
   const uint32_t zyx1 =
      (R300_SWIZZLE_SELECT_Z << R300_SWIZZLE_SELECT_X_SHIFT) |
      (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
      (R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_Z_SHIFT) |
      (R300_SWIZZLE_SELECT_FP_ONE << R300_SWIZZLE_SELECT_W_SHIFT) |
      (0xfu << R300_WRITE_ENA_SHIFT);
   assert((st.vap_prog_stream_cntl_ext[0] >> 16) == zyx1);

   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_F32_2,
                                                 &st) == 0);
   assert(st.fetch_dwords == 6);
   assert((st.vap_prog_stream_cntl[0] >> 16) ==
          (R300_DATA_TYPE_FLOAT_2 | (6u << R300_DST_VEC_LOC_SHIFT) |
           R300_LAST_VEC));
   /* (0, y, x, 1). */
   const uint32_t zero_yx1 =
      (R300_SWIZZLE_SELECT_FP_ZERO << R300_SWIZZLE_SELECT_X_SHIFT) |
      (R300_SWIZZLE_SELECT_Y << R300_SWIZZLE_SELECT_Y_SHIFT) |
      (R300_SWIZZLE_SELECT_X << R300_SWIZZLE_SELECT_Z_SHIFT) |
      (R300_SWIZZLE_SELECT_FP_ONE << R300_SWIZZLE_SELECT_W_SHIFT) |
      (0xfu << R300_WRITE_ENA_SHIFT);
   assert((st.vap_prog_stream_cntl_ext[0] >> 16) == zero_yx1);

   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_F32_1,
                                                 &st) == -EINVAL);
   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_INVALID,
                                                 &st) == -EINVAL);
   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_COUNT,
                                                 &st) == -EINVAL);
   assert(r300_r2vb_fetched_producer_fetch_state(R300_VERTEX_FORMAT_F32_4,
                                                 NULL) == -EINVAL);
}

static void
test_slot_positions(void)
{
   uint32_t words[12];
   assert(r300_r2vb_fetched_producer_slot_positions(3, words, 12) == 0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(words[v * 4 + 0] == float_bits((float)v + 0.5f));
      assert(words[v * 4 + 1] == float_bits(0.5f));
      assert(words[v * 4 + 2] == 0);
      assert(words[v * 4 + 3] == float_bits(1.0f));
   }
   assert(r300_r2vb_fetched_producer_slot_positions(3, words, 11) == -ENOSPC);
   assert(r300_r2vb_fetched_producer_slot_positions(0, words, 12) == -EINVAL);
   assert(r300_r2vb_fetched_producer_slot_positions(3, NULL, 12) == -EINVAL);
}

/* The fetched reference pass carries the immediate reference pass's
 * register contract: every register the immediate pass writes, the
 * fetched pass writes with the same value, except the fetch tuple the
 * width changes (PROG_STREAM_CNTL_0, PROG_STREAM_CNTL_EXT_0, VTX_SIZE);
 * and the fetched pass writes no register the immediate pass leaves
 * alone.  The draw grammar is the one other difference: LOAD_VBPNTR plus
 * DRAW_VBUF_2 in place of DRAW_IMMD_2.
 */
static void
test_register_parity(int format_id)
{
   struct r300_r2vb_fetched_producer_ib fetched;
   assert(r300_r2vb_fetched_producer_reference_emit(format_id, &fetched) == 0);
   assert(r300_r2vb_fetched_producer_validate_reloc_sites(&fetched) == 0);
   struct r300_r2vb_producer_ib immediate;
   assert(r300_r2vb_producer_reference_emit(&immediate) == 0);

   static struct register_map fetched_map, immediate_map;
   walk(fetched.ib, fetched.ib_size_dwords, &fetched_map);
   walk(immediate.ib, immediate.ib_size_dwords, &immediate_map);

   const uint32_t psc0 = R300_VAP_PROG_STREAM_CNTL_0 >> 2;
   const uint32_t psc_ext0 = R300_VAP_PROG_STREAM_CNTL_EXT_0 >> 2;
   const uint32_t vtx_size = R300_VAP_VTX_SIZE >> 2;
   unsigned compared = 0;
   for (uint32_t reg = 0; reg < 0x4000; reg++) {
      /* The fetched body establishes the whole RS_IP and RS_INST tables,
       * zero tail included, where the immediate pass writes entry 0 of
       * each and leaves the rest inherited; with RS_INST_COUNT = 0 the
       * tail is inert, so the fetched pass may write it as zero and the
       * immediate pass may leave it alone.
       */
      const bool rs_tail = (reg > (R300_RS_IP_0 >> 2) &&
                            reg <= (R300_RS_IP_0 >> 2) + 7) ||
                           (reg > (R300_RS_INST_0 >> 2) &&
                            reg <= (R300_RS_INST_0 >> 2) + 7);
      if (rs_tail) {
         assert(!immediate_map.written[reg]);
         assert(fetched_map.written[reg] && fetched_map.value[reg] == 0);
         continue;
      }
      if (fetched_map.written[reg] != immediate_map.written[reg])
         fprintf(stderr, "register 0x%04x: fetched %d immediate %d\n",
                 reg << 2, fetched_map.written[reg],
                 immediate_map.written[reg]);
      assert(fetched_map.written[reg] == immediate_map.written[reg]);
      if (!fetched_map.written[reg])
         continue;
      compared++;
      if (reg == psc0 || reg == psc_ext0 || reg == vtx_size)
         continue;
      assert(fetched_map.value[reg] == immediate_map.value[reg]);
   }
   /* The contract prefix, prologue, VAP tuple, RS routing, US block,
    * and tail span well over a hundred distinct registers.
    */
   assert(compared > 100);

   /* The fetch tuple itself: the immediate tuple with the width's data
    * type, the reversed source swizzle, and 4 + fetch dwords.
    */
   const struct r300_vertex_format_semantics *fmt =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);
   assert((immediate_map.value[psc0] & 0xffffu) ==
          (fetched_map.value[psc0] & 0xffffu));
   assert((fetched_map.value[psc0] >> 16) ==
          ((uint32_t)fmt->data_type | (6u << R300_DST_VEC_LOC_SHIFT) |
           R300_LAST_VEC));
   assert((fetched_map.value[psc_ext0] & 0xffffu) ==
          (immediate_map.value[psc_ext0] & 0xffffu));
   assert((fetched_map.value[psc_ext0] >> 16) ==
          expected_source_swizzle(format_id));
   assert(fetched_map.value[vtx_size] == 4u + fmt->hardware_fetch_dwords);
   assert(immediate_map.value[vtx_size] == 8u);

   assert(opcode_present(&immediate_map, R300_PACKET3_3D_DRAW_IMMD_2));
   assert(!opcode_present(&immediate_map, R300_PACKET3_3D_LOAD_VBPNTR));
   assert(!opcode_present(&fetched_map, R300_PACKET3_3D_DRAW_IMMD_2));
   assert(opcode_present(&fetched_map, R300_PACKET3_3D_LOAD_VBPNTR));
   assert(opcode_present(&fetched_map, R300_PACKET3_3D_DRAW_VBUF_2));

   /* Layout: contract + prologue + US block, then the fixed fetched body,
    * then the tail; the carrier site precedes the body and the array
    * sites sit inside it.
    */
   assert(fetched.fetch_body_start + R300_R2VB_FETCH_PASS_DWORDS +
             TAIL_DWORDS ==
          fetched.ib_size_dwords);
   assert(fetched.reloc_site_count == 3);
   assert(fetched.reloc_sites[0].ib_index < fetched.fetch_body_start);
   assert(fetched.reloc_sites[1].ib_index > fetched.fetch_body_start);
   assert(fetched.reloc_sites[2].ib_index ==
          fetched.reloc_sites[1].ib_index + 2);
   assert(fetched.ib[fetched.reloc_sites[0].ib_index] == 0);
   assert(fetched.ib[fetched.reloc_sites[1].ib_index] == 4);
   assert(fetched.ib[fetched.reloc_sites[2].ib_index] == 8);

   /* The bytes ahead of the fetched body equal the immediate pass ahead
    * of its VAP tuple: the shared prologue emitter, verbatim.  The
    * immediate pass writes VAP_PROG_STREAM_CNTL_0 right after
    * VAP_VTE_CNTL, so its first PSC header locates that boundary; the
    * fetched pass emits the US block before its body, so the US block is
    * located as the dwords between the prologue end and the body start.
    */
   uint32_t immediate_psc_header = 0;
   for (uint32_t i = 0; i + 8 < immediate.ib_size_dwords; i++) {
      if (immediate.ib[i] == CP_PACKET0(R300_VAP_PROG_STREAM_CNTL_0, 7)) {
         immediate_psc_header = i;
         break;
      }
   }
   assert(immediate_psc_header != 0);
   assert(memcmp(fetched.ib, immediate.ib,
                 immediate_psc_header * sizeof(uint32_t)) == 0);

   /* The fetched body equals a direct fetch-pass emission of the same
    * descriptor, so the producer inherits the fetch-pass contract
    * dword for dword.
    */
   struct r300_r2vb_fetch_state state;
   assert(r300_r2vb_fetched_producer_fetch_state(format_id, &state) == 0);
   const struct r300_r2vb_fetch_pass_params fetch = {
      .state = &state,
      .stream = {
         { .role = R300_R2VB_BO_SLOT, .size_dwords = 4, .stride_dwords = 4,
           .offset_bytes = 0, .bo_size_bytes = 4096,
           .relocation_payload = 4 },
         { .role = R300_R2VB_BO_MODEL,
           .size_dwords = fmt->hardware_fetch_dwords,
           .stride_dwords = fmt->semantic_record_bytes / 4,
           .offset_bytes = 0, .bo_size_bytes = 4096,
           .relocation_payload = 8 },
      },
      .vertex_count = R300_R2VB_PRODUCER_REFERENCE_COUNT,
      .vf_prim = R300_VAP_VF_CNTL__PRIM_POINTS,
   };
   uint32_t body[R300_R2VB_FETCH_PASS_DWORDS];
   struct r300_pm4_builder b;
   struct r300_r2vb_fetch_pass_relocs body_relocs;
   r300_pm4_builder_init(&b, body, R300_R2VB_FETCH_PASS_DWORDS);
   assert(r300_r2vb_fetch_pass_emit(&b, &fetch, &body_relocs) == 0);
   uint32_t body_dwords = 0;
   assert(r300_pm4_builder_finish(&b, &body_dwords) == 0);
   assert(body_dwords == R300_R2VB_FETCH_PASS_DWORDS);
   assert(memcmp(&fetched.ib[fetched.fetch_body_start], body,
                 sizeof(body)) == 0);

   /* The tail equals the immediate pass's tail. */
   assert(memcmp(&fetched.ib[fetched.ib_size_dwords - TAIL_DWORDS],
                 &immediate.ib[immediate.ib_size_dwords - TAIL_DWORDS],
                 TAIL_DWORDS * sizeof(uint32_t)) == 0);

   /* Scanning the stream finds exactly the three recorded sites. */
   uint32_t indices[4], payloads[4];
   assert(r300_pm4_scan_reloc_sites(fetched.ib, fetched.ib_size_dwords,
                                    indices, payloads, 4) == 3);
   for (unsigned i = 0; i < 3; i++) {
      assert(indices[i] == fetched.reloc_sites[i].ib_index);
      assert(payloads[i] == i * 4);
   }
   assert(r300_pm4_scan_reloc_sites(fetched.ib, fetched.ib_size_dwords,
                                    indices, payloads, 2) == -ENOSPC);

   r300_r2vb_producer_pass_release(&immediate);
   r300_r2vb_fetched_producer_release(&fetched);
}

static void
test_refusals(void)
{
   struct r300_r2vb_producer_layout layout;
   assert(r300_r2vb_producer_layout_single_row(3, &layout) == 0);
   struct r300_fragment_binary fs;
   assert(r300_r2vb_producer_reference_fs(&fs) == 0);

   const struct r300_r2vb_fetched_producer_params good = {
      .layout = layout,
      .fragment_binary = &fs,
      .source = { .format_id = R300_VERTEX_FORMAT_F32_4, .offset_bytes = 0,
                  .stride_bytes = 16, .bo_size_bytes = 4096 },
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = 4096,
   };
   struct r300_r2vb_fetched_producer_ib ib;
   assert(r300_r2vb_fetched_producer_emit(&good, &ib) == 0);
   assert(r300_r2vb_fetched_producer_validate_reloc_sites(&ib) == 0);
   const uint32_t good_dwords = ib.ib_size_dwords;
   r300_r2vb_fetched_producer_release(&ib);

   struct r300_r2vb_fetched_producer_params bad;

   bad = good;
   bad.source.stride_bytes = 12;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.source.stride_bytes = 18;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.source.offset_bytes = 2;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.slot_offset_bytes = 6;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.source.format_id = R300_VERTEX_FORMAT_F32_1;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.fragment_binary = NULL;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   bad = good;
   bad.layout.count = 0;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -EINVAL);
   /* Three 16-byte records from offset 16 reach byte 64 of a 60-byte BO. */
   bad = good;
   bad.source.offset_bytes = 16;
   bad.source.bo_size_bytes = 60;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -ERANGE);
   bad.source.bo_size_bytes = 64;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == 0);
   r300_r2vb_fetched_producer_release(&ib);
   bad = good;
   bad.slot_bo_size_bytes = 47;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == -ERANGE);
   /* A wider stride over a larger BO is a legal application binding. */
   bad = good;
   bad.source.stride_bytes = 32;
   bad.source.offset_bytes = 256;
   assert(r300_r2vb_fetched_producer_emit(&bad, &ib) == 0);
   assert(ib.ib_size_dwords == good_dwords);
   r300_r2vb_fetched_producer_release(&ib);

   /* Caller storage short of the stream refuses without a stream. */
   uint32_t *words = calloc(good_dwords, sizeof(uint32_t));
   assert(words != NULL);
   assert(r300_r2vb_fetched_producer_emit_into(&good, words, good_dwords - 1,
                                               &ib) == -ENOSPC);
   assert(ib.ib == NULL && ib.ib_size_dwords == 0);
   assert(r300_r2vb_fetched_producer_emit_into(&good, words, good_dwords,
                                               &ib) == 0);
   assert(ib.ib_size_dwords == good_dwords && !ib.owns_ib);
   free(words);

   r300_fragment_binary_finish(&fs);
}

static void
test_route_compose(int format_id)
{
   struct r300_r2vb_fetched_route_ib route;
   assert(r300_r2vb_fetched_route_reference_compose(format_id, &route) == 0);

   struct r300_r2vb_fetched_producer_ib producer;
   assert(r300_r2vb_fetched_producer_reference_emit(format_id, &producer) ==
          0);
   struct r300_tcl_bypass_triangle_ib consumer;
   assert(r300_tcl_bypass_triangle_extent_emit(R300_TRIANGLE_TARGET_WIDTH,
                                               R300_TRIANGLE_TARGET_HEIGHT,
                                               &consumer) == 0);

   assert(route.ib_size_dwords ==
          producer.ib_size_dwords + consumer.ib_size_dwords);
   assert(route.consumer_start_dwords == producer.ib_size_dwords);
   assert(route.composition.fragment_count == 2);
   assert(route.composition.fragment_start[0] == 0);
   assert(route.composition.reloc_count == 5);

   /* Stream order: producer carrier write, slot read, source read,
    * consumer color write, consumer carrier read (the TCL-bypass cell
    * retargets the color buffer before binding its vertex array);
    * payloads are the role map's chunk index times four.
    */
   static const enum r300_r2vb_bo_role roles[5] = {
      R300_R2VB_BO_CARRIER, R300_R2VB_BO_SLOT, R300_R2VB_BO_MODEL,
      R300_R2VB_BO_COLOR, R300_R2VB_BO_CARRIER,
   };
   static const uint32_t payloads[5] = { 0, 8, 12, 4, 0 };
   static const uint32_t writes[5] = { 2, 0, 0, 2, 0 };
   static const uint32_t reads[5] = { 0, 2, 2, 0, 2 };
   for (unsigned i = 0; i < 5; i++) {
      const struct r300_pm4_composed_reloc *r = &route.composition.relocs[i];
      assert(r->role == roles[i]);
      assert(route.ib[r->ib_index] == payloads[i]);
      assert(route.ib[r->ib_index - 1] == CP_PACKET3(R300_PM4_PACKET3_NOP, 0));
      assert(r->write_domain == writes[i]);
      assert(r->read_domains == reads[i]);
      if (i > 0)
         assert(route.composition.relocs[i - 1].ib_index < r->ib_index);
   }
   assert(route.composition.relocs[2].ib_index < route.consumer_start_dwords);
   assert(route.composition.relocs[3].ib_index >= route.consumer_start_dwords);

   /* Outside the rewritten payloads, the halves are the component
    * emissions verbatim.
    */
   uint32_t *expected = malloc(route.ib_size_dwords * sizeof(uint32_t));
   assert(expected != NULL);
   memcpy(expected, producer.ib, producer.ib_size_dwords * sizeof(uint32_t));
   memcpy(expected + producer.ib_size_dwords, consumer.ib,
          consumer.ib_size_dwords * sizeof(uint32_t));
   for (unsigned i = 0; i < 5; i++)
      expected[route.composition.relocs[i].ib_index] = payloads[i];
   assert(memcmp(route.ib, expected, route.ib_size_dwords * sizeof(uint32_t)) ==
          0);
   free(expected);

   uint32_t indices[8], scanned[8];
   assert(r300_pm4_scan_reloc_sites(route.ib, route.ib_size_dwords, indices,
                                    scanned, 8) == 5);
   for (unsigned i = 0; i < 5; i++) {
      assert(indices[i] == route.composition.relocs[i].ib_index);
      assert(scanned[i] == payloads[i]);
   }

   /* Known-bad: an unbound role refuses with -ENOENT before any write. */
   struct r300_r2vb_fetched_route_params params = {
      .source = { .format_id = format_id, .offset_bytes = 0,
                  .stride_bytes = r300_vertex_format_semantics(
                                     (enum r300_vertex_format_id)format_id)
                                     ->semantic_record_bytes,
                  .bo_size_bytes = 4096 },
      .slot_offset_bytes = 0,
      .slot_bo_size_bytes = 4096,
      .consumer_words = consumer.ib,
      .consumer_dwords = consumer.ib_size_dwords,
      .consumer_carrier_site =
         consumer.reloc_sites[R300_TRIANGLE_SLOT_VERTEX].ib_index,
      .consumer_color_site =
         consumer.reloc_sites[R300_TRIANGLE_SLOT_COLOR].ib_index,
      .roles = { .chunk_index = { [R300_R2VB_BO_CARRIER] = 0,
                                  [R300_R2VB_BO_COLOR] = 1,
                                  [R300_R2VB_BO_SLOT] = -1,
                                  [R300_R2VB_BO_MODEL] = 3 } },
   };
   struct r300_r2vb_fetched_route_ib refused;
   assert(r300_r2vb_fetched_route_compose(&params, &refused) == -ENOENT);
   assert(refused.ib == NULL);

   /* Known-bad: a consumer site that is not a relocation payload refuses
    * with -EINVAL.
    */
   params.roles.chunk_index[R300_R2VB_BO_SLOT] = 2;
   params.consumer_color_site =
      consumer.reloc_sites[R300_TRIANGLE_SLOT_COLOR].ib_index + 1;
   assert(r300_r2vb_fetched_route_compose(&params, &refused) == -EINVAL);

   /* Known-bad: a second writer of the carrier role -- the consumer's
    * carrier site misdeclared as a write -- refuses with -EEXIST through
    * the composer, the one-writer-per-role rule.
    */
   {
      const struct r300_pm4_reloc_site producer_sites[3] = {
         { producer.reloc_sites[0].ib_index, R300_R2VB_BO_CARRIER, 0, 2 },
         { producer.reloc_sites[1].ib_index, R300_R2VB_BO_SLOT, 2, 0 },
         { producer.reloc_sites[2].ib_index, R300_R2VB_BO_MODEL, 2, 0 },
      };
      const struct r300_pm4_reloc_site consumer_sites[2] = {
         { consumer.reloc_sites[R300_TRIANGLE_SLOT_COLOR].ib_index,
           R300_R2VB_BO_COLOR, 0, 2 },
         { consumer.reloc_sites[R300_TRIANGLE_SLOT_VERTEX].ib_index,
           R300_R2VB_BO_CARRIER, 2, 2 },
      };
      const struct r300_pm4_fragment fragments[2] = {
         { producer.ib, producer.ib_size_dwords, producer_sites, 3 },
         { consumer.ib, consumer.ib_size_dwords, consumer_sites, 2 },
      };
      const struct r300_pm4_role_map map = {
         .chunk_index = { [R300_R2VB_BO_CARRIER] = 0, [R300_R2VB_BO_COLOR] = 1,
                          [R300_R2VB_BO_SLOT] = 2, [R300_R2VB_BO_MODEL] = 3 },
      };
      uint32_t *words = malloc(route.ib_size_dwords * sizeof(uint32_t));
      assert(words != NULL);
      struct r300_pm4_builder b;
      struct r300_pm4_composition composition;
      r300_pm4_builder_init(&b, words, route.ib_size_dwords);
      assert(r300_pm4_compose(&b, fragments, 2, &map, &composition) ==
             -EEXIST);
      assert(b.count == 0);
      free(words);
   }

   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_fetched_producer_release(&producer);
   r300_r2vb_fetched_route_release(&route);
}

static void
test_scan_refusals(void)
{
   /* A type-1 header has no PM4 meaning. */
   const uint32_t bad_type[1] = { 0x40000000u };
   assert(r300_pm4_scan_reloc_sites(bad_type, 1, NULL, NULL, 4) == -EINVAL);
   /* A PACKET0 claiming two payload dwords in a one-dword stream. */
   const uint32_t truncated[2] = { CP_PACKET0(R300_VAP_VTX_SIZE, 1), 0 };
   assert(r300_pm4_scan_reloc_sites(truncated, 2, NULL, NULL, 4) == -EINVAL);
   /* A well-formed stream with no relocation. */
   const uint32_t plain[2] = { CP_PACKET0(R300_VAP_VTX_SIZE, 0), 8 };
   assert(r300_pm4_scan_reloc_sites(plain, 2, NULL, NULL, 4) == 0);
   assert(r300_pm4_scan_reloc_sites(NULL, 0, NULL, NULL, 4) == -EINVAL);
}

/* The composed reference routes' identities: the no-submit digests an
 * offline composition and the driver's submit-time composition both
 * produce for the reference geometry.
 */
static void
test_route_digests(void)
{
   static const struct {
      int format_id;
      uint32_t dwords;
      uint32_t consumer_start;
      const char *blake3;
   } pins[] = {
      { R300_VERTEX_FORMAT_F32_4, R300_FETCHED_F32_4_ROUTE_IB_DWORDS,
        R300_FETCHED_F32_4_ROUTE_CONSUMER_START_DWORDS,
        R300_FETCHED_F32_4_ROUTE_IB_BLAKE3 },
      { R300_VERTEX_FORMAT_F32_3, R300_FETCHED_F32_3_ROUTE_IB_DWORDS,
        R300_FETCHED_F32_3_ROUTE_CONSUMER_START_DWORDS,
        R300_FETCHED_F32_3_ROUTE_IB_BLAKE3 },
      { R300_VERTEX_FORMAT_F32_2, R300_FETCHED_F32_2_ROUTE_IB_DWORDS,
        R300_FETCHED_F32_2_ROUTE_CONSUMER_START_DWORDS,
        R300_FETCHED_F32_2_ROUTE_IB_BLAKE3 },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(pins); i++) {
      struct r300_r2vb_fetched_route_ib route;
      assert(r300_r2vb_fetched_route_reference_compose(pins[i].format_id,
                                                       &route) == 0);
      char hex[65];
      r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, hex);
      if (route.ib_size_dwords != pins[i].dwords ||
          route.consumer_start_dwords != pins[i].consumer_start ||
          strcmp(hex, pins[i].blake3) != 0) {
         fprintf(stderr,
                 "fetched route format %d: %u dwords split %u blake3 %s "
                 "(pinned %u / %u / %s)\n",
                 pins[i].format_id, route.ib_size_dwords,
                 route.consumer_start_dwords, hex, pins[i].dwords,
                 pins[i].consumer_start, pins[i].blake3);
         assert(!"fetched route digest drifted from its pin");
      }
      r300_r2vb_fetched_route_release(&route);
   }
   /* The three widths compose distinct streams. */
   assert(strcmp(R300_FETCHED_F32_4_ROUTE_IB_BLAKE3,
                 R300_FETCHED_F32_3_ROUTE_IB_BLAKE3) != 0);
   assert(strcmp(R300_FETCHED_F32_3_ROUTE_IB_BLAKE3,
                 R300_FETCHED_F32_2_ROUTE_IB_BLAKE3) != 0);
}

int
main(void)
{
   test_fetch_state();
   test_slot_positions();
   test_register_parity(R300_VERTEX_FORMAT_F32_4);
   test_register_parity(R300_VERTEX_FORMAT_F32_3);
   test_register_parity(R300_VERTEX_FORMAT_F32_2);
   test_refusals();
   test_route_compose(R300_VERTEX_FORMAT_F32_4);
   test_route_compose(R300_VERTEX_FORMAT_F32_3);
   test_route_compose(R300_VERTEX_FORMAT_F32_2);
   test_scan_refusals();
   test_route_digests();
   printf("r300-r2vb-fetched-producer: OK\n");
   return 0;
}
