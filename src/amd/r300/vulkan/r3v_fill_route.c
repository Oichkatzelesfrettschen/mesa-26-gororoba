/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_fill_route.h"

#include "amd/r300/common/r300_rb2d_linear_span.h"
#include "util/mesa-blake3.h"

#include <string.h>

const char *
r3v_fill_route_refusal_name(enum r3v_fill_route_refusal r)
{
   static const char *const names[R3V_FILL_ROUTE_REFUSAL_COUNT] = {
      [R3V_FILL_ROUTE_ADMITTED] = "admitted",
      [R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST] = "malformed_request",
      [R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND] = "buffer_unbound",
      [R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST] = "usage_transfer_dst",
      [R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY] = "range_empty",
      [R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT] = "range_alignment",
      [R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER] = "range_outside_buffer",
      [R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY] =
         "binding_outside_memory",
      [R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE] = "address_envelope",
      [R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE] =
         "memory_not_host_visible",
      [R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN] = "write_domain",
      [R3V_FILL_ROUTE_REFUSE_CELL_UNFROZEN] = "cell_unfrozen",
      [R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED] = "authority_undeclared",
      [R3V_FILL_ROUTE_REFUSE_AUTHORITY_MISMATCH] = "authority_mismatch",
   };
   return (unsigned)r < R3V_FILL_ROUTE_REFUSAL_COUNT ? names[r] : NULL;
}

enum r3v_fill_route_refusal
r3v_fill_route_memory_check(const struct r3v_fill_route_memory *m)
{
   if (m == NULL)
      return R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST;
   if (!m->bound)
      return R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND;
   if ((m->buffer_usage & R3V_FILL_ROUTE_USAGE_TRANSFER_DST) == 0)
      return R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST;
   if (m->fill_bytes == 0)
      return R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY;
   /* The pattern is one dword and the carrier row counts ARGB8888 pixels,
    * so a range off the dword grid names no whole pixel to fill. */
   if (m->fill_offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||
       m->fill_bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0)
      return R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT;
   /* The sum is formed only after the subtraction bounds it, so a range
    * whose far edge wraps 64 bits is refused rather than computed. */
   if (m->fill_offset > m->buffer_bytes ||
       m->fill_bytes > m->buffer_bytes - m->fill_offset)
      return R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER;
   if (m->binding_offset > m->memory_bytes ||
       m->buffer_bytes > m->memory_bytes - m->binding_offset)
      return R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY;
   /* DST_PITCH_OFFSET names the surface base in 1 KiB units of a 22-bit
    * field, which reaches 32 bits from the relocated buffer object base.
    * The fill's far edge is measured from that base, so it carries the
    * buffer's own binding offset inside the allocation. */
   const uint64_t far_edge =
      m->binding_offset + m->fill_offset + m->fill_bytes;
   if (far_edge > R300_RB2D_ADDRESS_SPACE_BYTES)
      return R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE;
   if ((m->memory_property_flags & R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE) == 0)
      return R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE;
   if (m->write_domain != R3V_FILL_ROUTE_DOMAIN_GTT)
      return R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN;
   return R3V_FILL_ROUTE_ADMITTED;
}

bool
r3v_fill_route_cell_frozen(const struct r3v_fill_route_cell *cell)
{
   if (cell == NULL)
      return false;
   /* One fill and one relocation is the whole cell: a second copy or a
    * second buffer object names work this stream does not carry. */
   if (cell->copy_count != 1 || cell->reference_count != 1)
      return false;
   if (!cell->copy_is_fill || !cell->destination_bound)
      return false;
   /* gpu_routed separates the fill this route resolved to the device from
    * one still awaiting the host store loop, so an unrouted record under
    * this cell kind is already a mismatch between the kind and the copy it
    * names. */
   if (!cell->gpu_routed)
      return false;
   if (cell->fill_bytes == 0 ||
       cell->fill_bytes % R3V_FILL_ROUTE_ELEMENT_BYTES != 0 ||
       cell->fill_offset % R3V_FILL_ROUTE_ELEMENT_BYTES != 0)
      return false;
   if (cell->fill_offset > cell->buffer_bytes ||
       cell->fill_bytes > cell->buffer_bytes - cell->fill_offset)
      return false;
   /* The 2D engine writes the destination and the stream reads nothing, so
    * the one relocation carries the write domain alone and names the
    * buffer object the fill's own destination binds. */
   if (cell->read_domains != 0 ||
       cell->write_domain != R3V_FILL_ROUTE_DOMAIN_GTT)
      return false;
   return cell->reference_names_destination;
}

/* Little-endian field appends.  The digest is compared across builds, so
 * every field enters the hash in a byte order the host's own does not
 * decide. */
static void
put_u32(struct mesa_blake3 *ctx, uint32_t v)
{
   const uint8_t bytes[4] = { (uint8_t)(v), (uint8_t)(v >> 8),
                              (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
   _mesa_blake3_update(ctx, bytes, sizeof(bytes));
}

static void
put_u64(struct mesa_blake3 *ctx, uint64_t v)
{
   put_u32(ctx, (uint32_t)v);
   put_u32(ctx, (uint32_t)(v >> 32));
}

/* A length-prefixed string.  The prefix is what keeps two adjacent strings
 * from hashing as one: a kernel release ending where a srcversion begins
 * would otherwise be indistinguishable from the pair that splits one
 * character earlier. */
static void
put_string(struct mesa_blake3 *ctx, const char *s)
{
   const size_t length = strlen(s);
   put_u64(ctx, (uint64_t)length);
   _mesa_blake3_update(ctx, s, length);
}

bool
r3v_fill_route_identity_digest(const struct r3v_fill_route_identity *id,
                               char out[R3V_FILL_ROUTE_DIGEST_HEX_SIZE],
                               const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (id == NULL || out == NULL) {
      *reason = "identity names no record";
      return false;
   }
   if (id->ib == NULL || id->ib_dwords == 0) {
      *reason = "identity names no command stream";
      return false;
   }
   if (id->rect_count == 0 || id->rects == NULL) {
      *reason = "identity names no rectangle list";
      return false;
   }
   if (id->segment_count == 0) {
      *reason = "identity names no segment";
      return false;
   }
   if (id->relocation_count == 0 || id->reloc_sites == NULL) {
      *reason = "identity names no relocation site";
      return false;
   }
   if (id->kernel_release == NULL || id->module_srcversion == NULL) {
      *reason = "identity names no deployment epoch";
      return false;
   }

   /* The stream enters through its own digest, the value the arming gate
    * compares on its own, so the identity carries both the stream and the
    * length it was sized to rather than the stream alone. */
   blake3_hash stream;
   _mesa_blake3_compute(id->ib, (size_t)id->ib_dwords * sizeof(*id->ib),
                        stream);

   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   put_u64(&ctx, id->allocation_bytes);
   put_u64(&ctx, id->buffer_bytes);
   put_u64(&ctx, id->binding_offset);
   put_u64(&ctx, id->fill_offset);
   put_u64(&ctx, id->fill_bytes);
   put_u32(&ctx, id->fill_value);

   put_u32(&ctx, id->pitch_bytes);
   put_u32(&ctx, id->format);
   put_u32(&ctx, id->segment_count);
   put_u32(&ctx, id->rect_count);
   for (uint32_t r = 0; r < id->rect_count; r++) {
      put_u32(&ctx, id->rects[r].x);
      put_u32(&ctx, id->rects[r].y);
      put_u32(&ctx, id->rects[r].width);
      put_u32(&ctx, id->rects[r].height);
      put_u32(&ctx, id->rects[r].value);
   }

   put_u32(&ctx, id->ib_dwords);
   _mesa_blake3_update(&ctx, stream, sizeof(stream));

   put_u32(&ctx, id->relocation_count);
   for (uint32_t s = 0; s < id->relocation_count; s++) {
      put_u32(&ctx, id->reloc_sites[s].ib_index);
      put_u32(&ctx, id->reloc_sites[s].slot);
   }
   put_u32(&ctx, id->read_domains);
   put_u32(&ctx, id->write_domain);

   put_string(&ctx, id->kernel_release);
   put_string(&ctx, id->module_srcversion);

   blake3_hash digest;
   _mesa_blake3_final(&ctx, digest);
   for (unsigned i = 0; i < BLAKE3_OUT_LEN; i++) {
      static const char hex[] = "0123456789abcdef";
      out[i * 2] = hex[digest[i] >> 4];
      out[i * 2 + 1] = hex[digest[i] & 0xf];
   }
   out[BLAKE3_OUT_LEN * 2] = '\0';
   return true;
}

/* A declared value is a digest or it is nothing.  A shorter, longer, or
 * uppercase value would compare unequal anyway; naming it undeclared keeps
 * a typo from reading as a submission an operator authorized and refused. */
static bool
is_digest(const char *value)
{
   if (value == NULL)
      return false;
   size_t i = 0;
   for (; value[i] != '\0'; i++) {
      const char c = value[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return i == BLAKE3_OUT_LEN * 2;
}

enum r3v_fill_route_refusal
r3v_fill_route_authority_check(const struct r3v_fill_route_identity *id,
                               const char *declared,
                               char actual[R3V_FILL_ROUTE_DIGEST_HEX_SIZE],
                               const char **reason)
{
   const char *ignored = NULL;
   if (reason == NULL)
      reason = &ignored;
   *reason = NULL;

   if (actual == NULL) {
      *reason = "authority names no storage for the computed identity";
      return R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST;
   }
   actual[0] = '\0';
   if (!r3v_fill_route_identity_digest(id, actual, reason))
      return R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST;
   if (!is_digest(declared)) {
      *reason = "no submission identity is declared";
      return R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED;
   }
   if (strcmp(declared, actual) != 0) {
      *reason = "the declared identity names a different submission";
      return R3V_FILL_ROUTE_REFUSE_AUTHORITY_MISMATCH;
   }
   return R3V_FILL_ROUTE_ADMITTED;
}
