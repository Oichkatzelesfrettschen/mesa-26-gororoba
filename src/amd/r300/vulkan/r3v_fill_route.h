/*
 * SPDX-License-Identifier: MIT
 *
 * The RB2D constant-fill route's own admission: the memory contract a
 * destination passes before any rectangle exists, the frozen-cell predicate
 * the arming gate reads, and the arming authority that binds one operator
 * declaration to the whole semantic and submit identity.
 *
 * The split follows the layers above it.  r300_rb2d_fill.h owns
 * DST_PITCH_OFFSET's packing and the rectangle field widths;
 * r300_rb2d_linear_span.h owns the decomposition of a byte interval onto a
 * carrier; r3v_route_policy.h owns which executor performs an operation.
 * This file owns what none of them can see: whether the destination is a
 * buffer this route may write at all, whether the cell about to be
 * installed matches the shape its kind freezes, and whether an operator
 * authorized this exact submission rather than some other one.
 *
 * The authority is the reason this file exists.  An IB digest alone binds
 * the register stream and nothing else: the same stream reaches a different
 * destination through a different relocation, a different binding offset,
 * or a different buffer object, and every one of those is a different
 * submission on the same bytes.  The identity below hashes the allocation,
 * the carrier, the decomposition, the stream, the relocation sites, the
 * buffer-object role, and the deployment epoch into one value, so an
 * authorization names one submission and admits no other.
 *
 * This translation unit names no Vulkan type.  The two Vulkan constants it
 * needs -- the transfer-destination usage bit and the host-visible memory
 * property -- are spelled here as values and asserted equal to their
 * Vulkan spellings where the two meet, so a standalone test builds the
 * whole admission without the API headers.
 */

#ifndef R3V_FILL_ROUTE_H
#define R3V_FILL_ROUTE_H

#include "amd/r300/common/r300_rb2d_fill.h"

#include <stdbool.h>
#include <stdint.h>

/* VK_BUFFER_USAGE_TRANSFER_DST_BIT.  vkCmdFillBuffer requires it of its
 * destination, so a buffer created without it names no fill this route may
 * perform.  r3v_native_fill_route.c asserts this value against the Vulkan
 * spelling. */
#define R3V_FILL_ROUTE_USAGE_TRANSFER_DST 0x00000002u

/* VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT.  The first route writes a
 * host-visible allocation so the oracle reads the result back through the
 * same mapping the sentinel was written through; a device-local
 * destination is a later cell with its own readback path. */
#define R3V_FILL_ROUTE_MEMORY_HOST_VISIBLE 0x00000002u

/* RADEON_GEM_DOMAIN_GTT.  One destination, written by the 2D engine and
 * read by nothing in the stream. */
#define R3V_FILL_ROUTE_DOMAIN_GTT 0x2u

/* The fill pattern is one dword and the span cuts on dword boundaries, so
 * the range counts whole elements of this width. */
#define R3V_FILL_ROUTE_ELEMENT_BYTES 4u

/* BLAKE3 hex plus terminator: the width every declared digest takes. */
#define R3V_FILL_ROUTE_DIGEST_HEX_SIZE 65u

/* Every rule this route holds a request to, in the order the checks test
 * them, so a refusal names one fact.  A caller reads the name; the
 * enumerated form lets a test name the arm it exercises. */
enum r3v_fill_route_refusal {
   R3V_FILL_ROUTE_ADMITTED = 0,
   /* The caller handed the layer something it cannot read.  A defect in
    * the calling code rather than a shape the route declines. */
   R3V_FILL_ROUTE_REFUSE_MALFORMED_REQUEST,
   /* The destination buffer reaches no device memory, so the relocation
    * would name no buffer object. */
   R3V_FILL_ROUTE_REFUSE_BUFFER_UNBOUND,
   R3V_FILL_ROUTE_REFUSE_USAGE_TRANSFER_DST,
   R3V_FILL_ROUTE_REFUSE_RANGE_EMPTY,
   R3V_FILL_ROUTE_REFUSE_RANGE_ALIGNMENT,
   R3V_FILL_ROUTE_REFUSE_RANGE_OUTSIDE_BUFFER,
   R3V_FILL_ROUTE_REFUSE_BINDING_OUTSIDE_MEMORY,
   /* The far edge of the fill, measured from the relocated buffer base,
    * lands outside what DST_PITCH_OFFSET's offset field addresses. */
   R3V_FILL_ROUTE_REFUSE_ADDRESS_ENVELOPE,
   R3V_FILL_ROUTE_REFUSE_MEMORY_NOT_HOST_VISIBLE,
   R3V_FILL_ROUTE_REFUSE_WRITE_DOMAIN,
   /* The cell about to be installed does not match the shape its kind
    * freezes, so the arming gate would report a nonmaximum extent for a
    * stream this route built. */
   R3V_FILL_ROUTE_REFUSE_CELL_UNFROZEN,
   /* No identity was declared, or the declared one is not a digest. */
   R3V_FILL_ROUTE_REFUSE_AUTHORITY_UNDECLARED,
   /* The declared identity names a different submission. */
   R3V_FILL_ROUTE_REFUSE_AUTHORITY_MISMATCH,
   R3V_FILL_ROUTE_REFUSAL_COUNT,
};

const char *r3v_fill_route_refusal_name(enum r3v_fill_route_refusal r);

/* The destination as the memory layers describe it.  buffer_bytes and
 * binding_offset are the VkBuffer's size and its offset inside the
 * VkDeviceMemory it binds; memory_bytes is that allocation's size.
 * fill_offset and fill_bytes are the range vkCmdFillBuffer named, measured
 * from the buffer base. */
struct r3v_fill_route_memory {
   bool bound;
   uint32_t buffer_usage;
   uint32_t memory_property_flags;
   uint32_t write_domain;
   uint64_t buffer_bytes;
   uint64_t binding_offset;
   uint64_t memory_bytes;
   uint64_t fill_offset;
   uint64_t fill_bytes;
};

/* Holds a destination to the rules this route writes under: the buffer is
 * bound and carries the transfer-destination usage, the range counts whole
 * dwords starting on a dword boundary, it closes inside the VkBuffer, the
 * VkBuffer closes inside its VkDeviceMemory, the far edge from the
 * relocated base stays inside R300_RB2D_ADDRESS_SPACE_BYTES, and the
 * allocation is host-visible GTT.
 *
 * The address bound has a second home: r300_rb2d_linear_span.c holds it as
 * the emitter's own field bound, for every caller that reaches the
 * decomposition without passing here.  This check fires first on this
 * route's path, so a wrapped range is named as a memory fact rather than
 * as a decomposition failure.
 */
enum r3v_fill_route_refusal
r3v_fill_route_memory_check(const struct r3v_fill_route_memory *m);

/* The recorded cell one RB2D fill installs, in the terms the geometry
 * predicate reads.  The command buffer carries one fill and one
 * relocation; reference_names_destination says that relocation names the
 * buffer object the fill's own destination binds. */
struct r3v_fill_route_cell {
   uint32_t copy_count;
   uint32_t reference_count;
   bool copy_is_fill;
   bool gpu_routed;
   bool destination_bound;
   bool reference_names_destination;
   uint64_t fill_offset;
   uint64_t fill_bytes;
   uint64_t buffer_bytes;
   uint32_t read_domains;
   uint32_t write_domain;
};

/* Whether the cell matches the shape R3V_NATIVE_CELL_KIND_RB2D_FILL_PUBLIC
 * freezes: one routed fill, one relocation naming that fill's own buffer
 * object under the write domain alone, and a dword range closing inside
 * the bound buffer.
 *
 * The route calls this on the cell it is about to install and the arming
 * gate's geometry fact calls it on the cell the command buffer carries, so
 * the predicate that admits is the predicate that judges.
 */
bool r3v_fill_route_cell_frozen(const struct r3v_fill_route_cell *cell);

/* One relocation site in the emitted stream: the dword whose payload the
 * kernel rewrites, and the slot it names. */
struct r3v_fill_route_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

/* Everything one submission of this route is.  The digest below covers
 * every field, so two submissions agreeing on all of them are the same
 * submission and any other pair differs.
 *
 * rects covers every rectangle of every segment in emission order, so a
 * segment boundary that moves changes the value even when the rectangle
 * list does not.  destination_handle names the buffer object the
 * relocation carries, so a submission over a different allocation of the
 * same shape is a different submission.  ib is hashed through its own digest, which is the value
 * the arming gate compares separately, so the identity carries both the
 * stream and the length that stream was sized to.  kernel_release and
 * module_srcversion are the deployment epoch: a stream authorized against
 * one radeon build is not authorized against another.
 */
struct r3v_fill_route_identity {
   uint64_t allocation_bytes;
   uint64_t buffer_bytes;
   uint64_t binding_offset;
   uint64_t fill_offset;
   uint64_t fill_bytes;
   uint32_t fill_value;

   uint32_t pitch_bytes;
   uint32_t format;
   uint32_t segment_count;
   uint32_t rect_count;
   const struct r300_rb2d_fill_rect *rects;

   uint32_t ib_dwords;
   const uint32_t *ib;

   uint32_t relocation_count;
   const struct r3v_fill_route_reloc_site *reloc_sites;
   uint32_t read_domains;
   uint32_t write_domain;
   /* The buffer object the relocation names, as the kernel names it for
    * this open file.  Two allocations of one size, bound at one offset and
    * filled over one range, are the same submission in every other field,
    * so the destination's own identity is what keeps an authorization from
    * opening the route over a different buffer with the same shape.  The
    * handle is the object's name rather than its address, so a run that
    * allocates in a different order reports a different identity and
    * refuses, which is the fail-closed direction. */
   uint32_t destination_handle;

   const char *kernel_release;
   const char *module_srcversion;
};

/* Writes the identity's digest as lowercase hex.  Returns false with
 * *reason naming the first field it cannot read: an absent record, an
 * absent rectangle or stream or relocation array behind a nonzero count, a
 * zero-length stream, or an absent deployment string.  A field this cannot
 * read leaves the identity undefined rather than hashed as a zero.
 */
bool r3v_fill_route_identity_digest(const struct r3v_fill_route_identity *id,
                                    char out[R3V_FILL_ROUTE_DIGEST_HEX_SIZE],
                                    const char **reason);

/* Holds one submission to the operator's declaration.  declared is the
 * value R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3 carries, read once at
 * device creation beside the route gates.  actual receives the computed
 * digest whichever way the check goes, so a refused run reports the value
 * an operator would have to declare to admit it.
 *
 * An absent declaration, one of the wrong width, and one carrying a
 * character outside lowercase hex all refuse as undeclared: a malformed
 * value is not a weaker authorization, it is none.
 */
enum r3v_fill_route_refusal
r3v_fill_route_authority_check(const struct r3v_fill_route_identity *id,
                               const char *declared,
                               char actual[R3V_FILL_ROUTE_DIGEST_HEX_SIZE],
                               const char **reason);

#endif /* R3V_FILL_ROUTE_H */
