/*
 * SPDX-License-Identifier: MIT
 *
 * The RB2D fill route's admission layer and the mutation matrix over the
 * exact prepared submit plan.
 *
 * The first half calibrates each rule in isolation: the memory contract
 * admits one destination and refuses each way a destination can fail, the
 * frozen-cell predicate admits one cell and refuses each field that breaks
 * it, the identity digest reads every field it names, and the authority
 * admits one declaration and refuses an absent, malformed, or foreign one.
 * Every refusal arm sits beside the admitted case built from the same
 * fixture, so a refusal proves the rule under test rather than a fixture
 * that never passed.
 *
 * The second half builds the attended cell's prepared plan -- a 64 KiB
 * destination, a 4992-byte interval at offset 12, the pattern 0x11223344,
 * the 256-byte carrier -- mutates one bound field at a time, and records
 * which check refuses.  Each mutation names its narrowest owner: a pitch
 * off the 64-byte grid belongs to the span layout, a base offset off the
 * 1 KiB grid to DST_PITCH_OFFSET's own packing, an empty rectangle to the
 * fill plan, a moved relocation site to the site validator, a truncated
 * stream to the emitter's capacity, an extra segment to this route's
 * one-segment contract, and a wrapped address to the memory contract.  The
 * fill pattern has no structural owner -- DP_BRUSH_FRGD_CLR takes any
 * 32-bit value -- so the submission identity is its sole catcher, which is
 * the reason that identity covers more than the stream digest.
 *
 * The verdicts are explicit returns rather than assertions, so a release
 * build that compiles assertions out runs the same checks.
 */

#include "r3v_fill_route.h"

#include "amd/r300/common/r300_rb2d_fill.h"
#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "util/macros.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      if (!(cond)) {                                                         \
         failures++;                                                         \
         fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                              \
      }                                                                      \
   } while (0)

/* The attended cell, as docs/hardware/r3v-native-rb2d-fill-route-cell.md
 * declares it.  The decomposition below is the planner's own; pinning it
 * here binds this route's reference plan to the shape
 * r300_rb2d_linear_span_test pins for the same interval. */
#define CELL_ALLOCATION_BYTES (64u * 1024u)
#define CELL_FILL_OFFSET 12u
#define CELL_FILL_BYTES 4992u
#define CELL_FILL_VALUE 0x11223344u
#define CELL_RECTS 3u
#define CELL_SEGMENTS 1u
#define CELL_IB_DWORDS R300_RB2D_FILL_DWORDS(CELL_RECTS)
#define CELL_KERNEL "6.16.0-fixture"
#define CELL_SRCVERSION "FIXTURESRCVERSION0000000"
#define CELL_DESTINATION_HANDLE 0x77u

/* Everything one submission of this route is, in the form the route builds
 * it: the destination, the carrier, the decomposition, the stream, and the
 * relocation sites.  A mutation edits one field of a copy. */
struct prepared_plan {
   struct r3v_fill_route_memory memory;
   struct r300_rb2d_span span;
   struct r300_rb2d_span_layout layout;
   struct r300_rb2d_fill_plan plans[CELL_SEGMENTS];
   struct r300_rb2d_fill_rect rects[CELL_SEGMENTS *
                                    R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   uint32_t ib[CELL_IB_DWORDS];
   uint32_t ib_dwords;
   struct r3v_fill_route_reloc_site sites[R300_RB2D_FILL_SLOT_COUNT];
   uint32_t site_count;
   uint32_t rect_count;
   uint32_t segment_count;
   uint32_t destination_handle;
   struct r3v_fill_route_cell cell;
};

static void
plan_identity(const struct prepared_plan *p,
              struct r3v_fill_route_identity *id)
{
   *id = (struct r3v_fill_route_identity){
      .allocation_bytes = p->memory.memory_bytes,
      .buffer_bytes = p->memory.buffer_bytes,
      .binding_offset = p->memory.binding_offset,
      .fill_offset = p->memory.fill_offset,
      .fill_bytes = p->memory.fill_bytes,
      .fill_value = p->span.value,
      .pitch_bytes = p->layout.pitch_bytes,
      .format = (uint32_t)p->layout.format,
      .segment_count = p->segment_count,
      .rect_count = p->rect_count,
      .rects = p->rects,
      .ib_dwords = p->ib_dwords,
      .ib = p->ib,
      .relocation_count = p->site_count,
      .reloc_sites = p->sites,
      .read_domains = p->cell.read_domains,
      .write_domain = p->cell.write_domain,
      .destination_handle = p->destination_handle,
      .kernel_release = CELL_KERNEL,
      .module_srcversion = CELL_SRCVERSION,
   };
}

/* Builds the attended cell's prepared plan through the same layers the
 * route runs: the memory contract, the span decomposition, the emitter,
 * and the relocation-site validator. */
static bool
prepare_cell(struct prepared_plan *p)
{
   memset(p, 0, sizeof(*p));
   p->memory = (struct r3v_fill_route_memory){
      .bound = true,
      .buffer_usage = R3V_FILL_ROUTE_USAGE_TRANSFER_DST,
      .memory_property_flags = R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE,
      .write_domain = R3V_FILL_ROUTE_DOMAIN_GTT,
      .buffer_bytes = CELL_ALLOCATION_BYTES,
      .binding_offset = 0,
      .memory_bytes = CELL_ALLOCATION_BYTES,
      .fill_offset = CELL_FILL_OFFSET,
      .fill_bytes = CELL_FILL_BYTES,
   };
   if (r3v_fill_route_memory_check(&p->memory) != R3V_FILL_ROUTE_ADMITTED)
      return false;

   p->span = (struct r300_rb2d_span){
      .byte_offset = p->memory.binding_offset + p->memory.fill_offset,
      .byte_size = p->memory.fill_bytes,
      .value = CELL_FILL_VALUE,
   };
   p->layout = (struct r300_rb2d_span_layout){
      .pitch_bytes = R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .format = R300_RB2D_FORMAT_ARGB8888,
   };

   enum r300_rb2d_span_refusal refusal = R300_RB2D_SPAN_OK;
   p->segment_count = r300_rb2d_linear_span_plan(
      &p->span, &p->layout, p->memory.memory_bytes, p->plans, p->rects,
      CELL_SEGMENTS, &refusal);
   if (p->segment_count != CELL_SEGMENTS)
      return false;
   for (uint32_t s = 0; s < p->segment_count; s++)
      p->rect_count += p->plans[s].rect_count;

   if (!r300_rb2d_linear_span_dwords(p->plans, p->segment_count,
                                     &p->ib_dwords) ||
       p->ib_dwords != CELL_IB_DWORDS)
      return false;

   struct r300_rb2d_fill_ib emitted;
   if (r300_rb2d_fill_emit_into(&p->plans[0], p->ib, p->ib_dwords,
                                &emitted) != 0 ||
       r300_rb2d_fill_validate_reloc_sites(&emitted) != 0)
      return false;
   for (uint32_t r = 0; r < emitted.reloc_site_count; r++) {
      p->sites[r].ib_index = emitted.reloc_sites[r].ib_index;
      p->sites[r].slot = emitted.reloc_sites[r].slot;
   }
   p->site_count = emitted.reloc_site_count;

   p->destination_handle = CELL_DESTINATION_HANDLE;
   p->cell = (struct r3v_fill_route_cell){
      .copy_count = 1,
      .reference_count = 1,
      .copy_is_fill = true,
      .gpu_routed = true,
      .destination_bound = true,
      .reference_names_destination = true,
      .fill_offset = p->memory.fill_offset,
      .fill_bytes = p->memory.fill_bytes,
      .buffer_bytes = p->memory.buffer_bytes,
      .read_domains = 0,
      .write_domain = R3V_FILL_ROUTE_DOMAIN_GTT,
   };
   return r3v_fill_route_cell_frozen(&p->cell);
}

/* The prepared plan is the one the attended cell declares, rectangle for
 * rectangle.  A plan that drifted from the declaration would carry every
 * mutation below against the wrong reference. */
static void
test_prepared_plan_is_the_attended_cell(const struct prepared_plan *p)
{
   static const struct {
      uint32_t x, y, width, height;
   } expected[CELL_RECTS] = {
      { 3, 0, 61, 1 },
      { 0, 1, 64, 18 },
      { 0, 19, 35, 1 },
   };

   CHECK(p->segment_count == CELL_SEGMENTS, "segments %u", p->segment_count);
   CHECK(p->rect_count == CELL_RECTS, "rectangles %u", p->rect_count);
   CHECK(p->site_count == 1, "relocation sites %u", p->site_count);
   CHECK(p->ib_dwords == CELL_IB_DWORDS, "stream dwords %u", p->ib_dwords);
   CHECK(p->plans[0].surface.pitch_bytes ==
            R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
         "carrier pitch %u", p->plans[0].surface.pitch_bytes);
   for (uint32_t r = 0; r < p->rect_count && r < CELL_RECTS; r++) {
      CHECK(p->rects[r].x == expected[r].x && p->rects[r].y == expected[r].y &&
               p->rects[r].width == expected[r].width &&
               p->rects[r].height == expected[r].height &&
               p->rects[r].value == CELL_FILL_VALUE,
            "rectangle %u is (%u,%u,%u,%u,0x%08x)", r, p->rects[r].x,
            p->rects[r].y, p->rects[r].width, p->rects[r].height,
            p->rects[r].value);
   }
}

static void
test_memory_contract(void)
{
   const struct r3v_fill_route_memory admitted = {
      .bound = true,
      .buffer_usage = R3V_FILL_ROUTE_USAGE_TRANSFER_DST,
      .memory_property_flags = R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE,
      .write_domain = R3V_FILL_ROUTE_DOMAIN_GTT,
      .buffer_bytes = 4096,
      .binding_offset = 0,
      .memory_bytes = 8192,
      .fill_offset = 0,
      .fill_bytes = 256,
   };
   /* Calibration: the fixture every arm below mutates is itself admitted,
    * so a refusal names the mutated field rather than the fixture. */
   CHECK(r3v_fill_route_memory_check(&admitted) == R3V_FILL_ROUTE_ADMITTED,
         "the admitted destination is refused as %s",
         r3v_fill_route_refusal_name(
            r3v_fill_route_memory_check(&admitted)));
   CHECK(r3v_fill_route_memory_check(NULL) ==
            R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST,
         "an absent request is admitted");

   struct r3v_fill_route_memory m;

#define ARM(field, value, expected)                                          \
   do {                                                                      \
      m = admitted;                                                          \
      m.field = (value);                                                     \
      const enum r3v_fill_route_refusal got = r3v_fill_route_memory_check(&m); \
      CHECK(got == (expected), #field " = " #value " gives %s, not %s",      \
            r3v_fill_route_refusal_name(got),                                \
            r3v_fill_route_refusal_name(expected));                          \
   } while (0)

   ARM(bound, false, R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND);
   ARM(buffer_usage, 0u, R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST);
   ARM(fill_bytes, 0u, R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY);
   ARM(fill_bytes, 255u, R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT);
   ARM(fill_offset, 2u, R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT);
   ARM(fill_bytes, 8192u, R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER);
   ARM(fill_offset, 4096u, R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER);
   ARM(binding_offset, 8192u, R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY);
   ARM(memory_property_flags, 0u,
       R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE);
   ARM(write_domain, 0x4u, R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN);
#undef ARM

   /* The address envelope needs an allocation past 32 bits to reach, so it
    * carries its own fixture rather than one field of the one above. */
   m = admitted;
   m.memory_bytes = R300_RB2D_ADDRESS_SPACE_BYTES + 8192;
   m.binding_offset = R300_RB2D_ADDRESS_SPACE_BYTES - 128;
   m.buffer_bytes = 4096;
   m.fill_offset = 0;
   m.fill_bytes = 256;
   CHECK(r3v_fill_route_memory_check(&m) ==
            R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE,
         "a far edge past 2^32 is admitted as %s",
         r3v_fill_route_refusal_name(r3v_fill_route_memory_check(&m)));
   /* The last byte the envelope reaches is admitted, so the bound is the
    * field's own width rather than one byte inside it. */
   m.binding_offset = R300_RB2D_ADDRESS_SPACE_BYTES - 256;
   CHECK(r3v_fill_route_memory_check(&m) == R3V_FILL_ROUTE_ADMITTED,
         "a far edge at exactly 2^32 is refused as %s",
         r3v_fill_route_refusal_name(r3v_fill_route_memory_check(&m)));
}

static void
test_cell_predicate(void)
{
   const struct r3v_fill_route_cell frozen = {
      .copy_count = 1,
      .reference_count = 1,
      .copy_is_fill = true,
      .gpu_routed = true,
      .destination_bound = true,
      .reference_names_destination = true,
      .fill_offset = 12,
      .fill_bytes = 4992,
      .buffer_bytes = CELL_ALLOCATION_BYTES,
      .read_domains = 0,
      .write_domain = R3V_FILL_ROUTE_DOMAIN_GTT,
   };
   CHECK(r3v_fill_route_cell_frozen(&frozen), "the frozen cell is unfrozen");
   CHECK(!r3v_fill_route_cell_frozen(NULL), "an absent cell is frozen");

   struct r3v_fill_route_cell c;
#define ARM(field, value)                                                    \
   do {                                                                      \
      c = frozen;                                                            \
      c.field = (value);                                                     \
      CHECK(!r3v_fill_route_cell_frozen(&c),                                 \
            #field " = " #value " still reports the cell frozen");           \
   } while (0)

   ARM(copy_count, 2u);
   ARM(reference_count, 2u);
   ARM(copy_is_fill, false);
   ARM(gpu_routed, false);
   ARM(destination_bound, false);
   ARM(reference_names_destination, false);
   ARM(fill_bytes, 0u);
   ARM(fill_bytes, 4990u);
   ARM(fill_offset, 13u);
   ARM(fill_offset, CELL_ALLOCATION_BYTES);
   /* The buffer object's role: the 2D engine writes the destination and the
    * stream reads nothing, so a read domain and a write domain outside GTT
    * are both a different role. */
   ARM(read_domains, R3V_FILL_ROUTE_DOMAIN_GTT);
   ARM(write_domain, 0u);
   ARM(write_domain, 0x4u);
#undef ARM
}

static void
test_identity_and_authority(const struct prepared_plan *reference)
{
   struct r3v_fill_route_identity id;
   plan_identity(reference, &id);

   char digest[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   const char *reason = NULL;
   CHECK(r3v_fill_route_identity_digest(&id, digest, &reason),
         "the reference identity has no digest: %s",
         reason != NULL ? reason : "unnamed");
   CHECK(strlen(digest) == R3V_FILL_ROUTE_DIGEST_HEX_SIZE - 1,
         "the digest is %zu characters", strlen(digest));

   /* Every field the digest names must be readable, so an absent array
    * behind a nonzero count leaves the identity undefined rather than
    * hashing as a zero. */
   struct r3v_fill_route_identity broken;
   char scratch[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
#define ABSENT(field, value)                                                 \
   do {                                                                      \
      broken = id;                                                           \
      broken.field = (value);                                                \
      CHECK(!r3v_fill_route_identity_digest(&broken, scratch, &reason),      \
            #field " = " #value " still produced a digest");                 \
   } while (0)
   ABSENT(ib, NULL);
   ABSENT(ib_dwords, 0u);
   ABSENT(rects, NULL);
   ABSENT(rect_count, 0u);
   ABSENT(segment_count, 0u);
   ABSENT(reloc_sites, NULL);
   ABSENT(relocation_count, 0u);
   ABSENT(destination_handle, 0u);
   ABSENT(kernel_release, NULL);
   ABSENT(module_srcversion, NULL);
#undef ABSENT
   CHECK(!r3v_fill_route_identity_digest(NULL, scratch, &reason),
         "an absent identity produced a digest");

   /* The authority admits the declaration that names this submission and
    * refuses every other value, malformed ones included. */
   char actual[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   CHECK(r3v_fill_route_authority_check(&id, digest, actual, &reason) ==
            R3V_FILL_ROUTE_ADMITTED,
         "the matching declaration is refused: %s",
         reason != NULL ? reason : "unnamed");
   CHECK(strcmp(actual, digest) == 0,
         "the authority reports a different identity than the digest");

   static const char *const rejected[] = {
      NULL,
      "",
      "1",
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde",
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0",
      "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
   };
   for (unsigned i = 0; i < ARRAY_SIZE(rejected); i++) {
      CHECK(r3v_fill_route_authority_check(&id, rejected[i], actual,
                                           &reason) ==
               R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED,
            "declaration %u is not reported undeclared", i);
   }
   /* A well-formed digest naming another submission is a mismatch rather
    * than an absent declaration, so the two refusals stay distinct. */
   char foreign[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   memcpy(foreign, digest, sizeof(foreign));
   foreign[0] = foreign[0] == 'a' ? 'b' : 'a';
   CHECK(r3v_fill_route_authority_check(&id, foreign, actual, &reason) ==
            R3V_FILL_ROUTE_REFUSE_AUTHORITY_MISMATCH,
         "a foreign digest is not reported as a mismatch");
}

/* One mutation of the prepared plan and what refused it. */
struct mutation_result {
   const char *name;
   const char *owner;
   bool structurally_refused;
   bool identity_moved;
};

static bool
identity_differs(const struct prepared_plan *reference,
                 const struct r3v_fill_route_identity *mutated)
{
   struct r3v_fill_route_identity base;
   plan_identity(reference, &base);
   char a[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   char b[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
   if (!r3v_fill_route_identity_digest(&base, a, NULL))
      return false;
   if (!r3v_fill_route_identity_digest(mutated, b, NULL))
      return true;
   return strcmp(a, b) != 0;
}

static void
report(struct mutation_result r)
{
   printf("  %-24s owner=%-22s structural=%-3s identity=%s\n", r.name,
          r.owner, r.structurally_refused ? "yes" : "no",
          r.identity_moved ? "moved" : "same");
   CHECK(r.structurally_refused || r.identity_moved,
         "mutation %s is refused by nothing", r.name);
}

static void
test_mutation_matrix(const struct prepared_plan *reference)
{
   struct prepared_plan p;
   struct r3v_fill_route_identity id;
   enum r300_rb2d_span_refusal span_refusal;

   printf("mutation matrix over the attended cell's prepared plan:\n");

   /* Wrong pitch: a carrier off DST_PITCH_OFFSET's 64-byte grid has no
    * representation in the word, so the span layout refuses it. */
   p = *reference;
   p.layout.pitch_bytes = 200;
   const enum r300_rb2d_span_refusal pitch_refusal =
      r300_rb2d_span_layout_check(&p.layout);
   plan_identity(&p, &id);
   CHECK(pitch_refusal == R300_RB2D_SPAN_REFUSE_LAYOUT_PITCH_GRID,
         "an off-grid pitch gives %s", r300_rb2d_span_refusal_name(pitch_refusal));
   report((struct mutation_result){ "wrong pitch", "span layout",
                                    pitch_refusal != R300_RB2D_SPAN_OK,
                                    identity_differs(reference, &id) });

   /* Wrong base offset: the offset field counts 1 KiB units, so a surface
    * base off that grid names a different surface. */
   p = *reference;
   p.plans[0].surface.base_offset_bytes += 512;
   const enum r300_rb2d_fill_refusal base_refusal =
      r300_rb2d_fill_plan_check(&p.plans[0]);
   CHECK(base_refusal == R300_RB2D_FILL_REFUSE_OFFSET_GRID,
         "an off-grid base gives %s",
         r300_rb2d_fill_refusal_name(base_refusal));
   report((struct mutation_result){ "wrong base offset", "fill plan",
                                    base_refusal != R300_RB2D_FILL_OK, false });

   /* Wrong rectangle extent: DST_WIDTH_HEIGHT both launches the fill and
    * carries its extent, so a zero extent names no work. */
   p = *reference;
   p.rects[1].width = 0;
   p.plans[0].rects = p.rects;
   const enum r300_rb2d_fill_refusal rect_refusal =
      r300_rb2d_fill_plan_check(&p.plans[0]);
   plan_identity(&p, &id);
   CHECK(rect_refusal == R300_RB2D_FILL_REFUSE_RECT_EMPTY,
         "an empty rectangle gives %s",
         r300_rb2d_fill_refusal_name(rect_refusal));
   report((struct mutation_result){ "wrong rect extent", "fill plan",
                                    rect_refusal != R300_RB2D_FILL_OK,
                                    identity_differs(reference, &id) });

   /* A rectangle reaching past the surface is the other extent fault, and
    * it lands on its own arm rather than the empty one. */
   p = *reference;
   p.rects[1].width = 4096;
   p.plans[0].rects = p.rects;
   const enum r300_rb2d_fill_refusal wide_refusal =
      r300_rb2d_fill_plan_check(&p.plans[0]);
   CHECK(wide_refusal == R300_RB2D_FILL_REFUSE_RECT_OUTSIDE,
         "an oversize rectangle gives %s",
         r300_rb2d_fill_refusal_name(wide_refusal));

   /* Wrong fill value: DP_BRUSH_FRGD_CLR takes any 32-bit pattern, so no
    * structural check owns this and the submission identity is the sole
    * catcher.  That is the whole reason the identity covers more than the
    * stream digest. */
   p = *reference;
   p.span.value = ~CELL_FILL_VALUE;
   plan_identity(&p, &id);
   report((struct mutation_result){ "wrong fill value",
                                    "submission identity", false,
                                    identity_differs(reference, &id) });

   /* Wrong relocation site: the validator holds one site, inside the
    * stream, with the PACKET3 NOP header one dword before its payload. */
   p = *reference;
   struct r300_rb2d_fill_ib moved = {
      .ib = p.ib,
      .ib_size_dwords = p.ib_dwords,
      .reloc_sites = { { p.sites[0].ib_index + 2, p.sites[0].slot } },
      .reloc_site_count = 1,
   };
   const int site_refusal = r300_rb2d_fill_validate_reloc_sites(&moved);
   p.sites[0].ib_index += 2;
   plan_identity(&p, &id);
   CHECK(site_refusal != 0, "a moved relocation site validates");
   report((struct mutation_result){ "wrong relocation site",
                                    "site validator", site_refusal != 0,
                                    identity_differs(reference, &id) });

   /* Wrong destination buffer object: two allocations of one size, bound
    * at one offset and filled over one range, agree in every other field,
    * so the object's own name is what separates them. */
   p = *reference;
   p.destination_handle = CELL_DESTINATION_HANDLE + 1u;
   plan_identity(&p, &id);
   report((struct mutation_result){ "wrong destination BO",
                                    "submission identity", false,
                                    identity_differs(reference, &id) });

   /* Wrong buffer-object role: the destination is written and never read,
    * so a read domain names a different role for the same allocation. */
   p = *reference;
   p.cell.read_domains = R3V_FILL_ROUTE_DOMAIN_GTT;
   plan_identity(&p, &id);
   report((struct mutation_result){ "wrong BO role", "cell predicate",
                                    !r3v_fill_route_cell_frozen(&p.cell),
                                    identity_differs(reference, &id) });

   /* Wrong write domain: the memory contract names the domain the route
    * writes under, and the cell predicate holds the installed reference to
    * the same value. */
   p = *reference;
   p.memory.write_domain = 0x4u;
   p.cell.write_domain = 0x4u;
   plan_identity(&p, &id);
   const enum r3v_fill_route_refusal domain_refusal =
      r3v_fill_route_memory_check(&p.memory);
   CHECK(domain_refusal == R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN,
         "a foreign write domain gives %s",
         r3v_fill_route_refusal_name(domain_refusal));
   report((struct mutation_result){ "wrong write domain",
                                    "memory contract", true,
                                    identity_differs(reference, &id) });

   /* Truncated stream: the emitter refuses before reporting a half-written
    * stream, and the identity over a shorter stream is a different one. */
   p = *reference;
   struct r300_rb2d_fill_ib truncated;
   const int emit_refusal = r300_rb2d_fill_emit_into(
      &p.plans[0], p.ib, p.ib_dwords - 1, &truncated);
   p.ib_dwords -= 1;
   plan_identity(&p, &id);
   CHECK(emit_refusal == -ENOSPC, "a truncated stream gives %d",
         emit_refusal);
   report((struct mutation_result){ "truncated stream", "emitter capacity",
                                    emit_refusal != 0,
                                    identity_differs(reference, &id) });

   /* One extra segment: this route's contract carries one, so a range
    * needing two is one the carrier cannot name in a single stream. */
   const uint64_t segment_window = (uint64_t)R300_RB2D_SAFE_EXCLUSIVE_END *
                                   R300_RB2D_SPAN_PITCH_DIRECT_WRITE;
   struct r300_rb2d_span two = {
      .byte_offset = CELL_FILL_OFFSET,
      .byte_size = segment_window + R300_RB2D_SPAN_PITCH_DIRECT_WRITE,
      .value = CELL_FILL_VALUE,
   };
   span_refusal = R300_RB2D_SPAN_OK;
   const uint32_t segments = r300_rb2d_linear_span_segments(
      &two, &reference->layout, segment_window * 4, &span_refusal);
   CHECK(segments > CELL_SEGMENTS,
         "a range past one segment window decomposes into %u segments (%s)",
         segments, r300_rb2d_span_refusal_name(span_refusal));
   report((struct mutation_result){ "one extra segment",
                                    "one-segment contract",
                                    segments > CELL_SEGMENTS, false });

   /* A 32-bit address wrap: the far edge measured from the relocated base
    * leaves what DST_PITCH_OFFSET addresses.  The memory contract names it
    * first on this route's path; the span holds the same bound for a
    * caller that reaches the decomposition directly. */
   p = *reference;
   p.memory.memory_bytes =
      R300_RB2D_ADDRESS_SPACE_BYTES + CELL_ALLOCATION_BYTES;
   p.memory.binding_offset = R300_RB2D_ADDRESS_SPACE_BYTES - 1024;
   p.memory.buffer_bytes = CELL_ALLOCATION_BYTES;
   const enum r3v_fill_route_refusal wrap_refusal =
      r3v_fill_route_memory_check(&p.memory);
   CHECK(wrap_refusal == R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE,
         "a wrapped address gives %s",
         r3v_fill_route_refusal_name(wrap_refusal));
   struct r300_rb2d_span wrapped = {
      .byte_offset = R300_RB2D_ADDRESS_SPACE_BYTES - 1024 + CELL_FILL_OFFSET,
      .byte_size = CELL_FILL_BYTES,
      .value = CELL_FILL_VALUE,
   };
   span_refusal = R300_RB2D_SPAN_OK;
   (void)r300_rb2d_linear_span_segments(&wrapped, &reference->layout,
                                        R300_RB2D_ADDRESS_SPACE_BYTES +
                                           CELL_ALLOCATION_BYTES,
                                        &span_refusal);
   CHECK(span_refusal == R300_RB2D_SPAN_REFUSE_ADDRESS_WIDTH,
         "the span's own address bound gives %s",
         r300_rb2d_span_refusal_name(span_refusal));
   plan_identity(&p, &id);
   report((struct mutation_result){ "32-bit address wrap",
                                    "memory contract", true,
                                    identity_differs(reference, &id) });
}

int
main(void)
{
   struct prepared_plan reference;
   if (!prepare_cell(&reference)) {
      fprintf(stderr,
              "FAIL: the attended cell's prepared plan does not build\n");
      return 1;
   }

   test_prepared_plan_is_the_attended_cell(&reference);
   test_memory_contract();
   test_cell_predicate();
   test_identity_and_authority(&reference);
   test_mutation_matrix(&reference);

   if (failures != 0) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("r3v-fill-route: all checks passed\n");
   return 0;
}
