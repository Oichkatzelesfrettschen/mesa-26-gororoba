/*
 * SPDX-License-Identifier: MIT
 *
 * Admission predicate for the first automatic ZMASK selection candidate.
 *
 * Automatic selection is the standing decision to substitute the ZMASK
 * fast clear for the ordinary combined depth and stencil clear without an
 * operator gate.  The decision has two halves, and this pair holds both:
 * the candidate in front of the driver sits inside the one configuration
 * the public ZMASK lifecycle qualification describes, and the eight
 * results that qualification predicts are retained.  The predicate
 * answers the first half and names the first clause that refuses;
 * r3v_native_zmask_promotion_missing answers the second.
 *
 * The logical-image invariant every clause serves:
 *
 *    A ZMASK representation is storage, so the logical image an
 *    application reads is the image it wrote, for every aspect the
 *    specification requires to survive.
 *
 * Vulkan 1.0 grants the storage freedom and fixes that boundary in three
 * places.  Resource Creation, "Images": VK_IMAGE_TILING_OPTIMAL lays
 * texels out in an implementation-dependent arrangement.  Resource
 * Creation, "Image Layouts": images are stored in implementation-
 * dependent opaque layouts, and each layout bounds the operations its
 * subresources support.  Synchronization and Cache Control, "Image Layout
 * Transitions": a transition happens inside a memory dependency, and a
 * transition whose old layout matches the subresource's current layout
 * preserves that range's contents -- only VK_IMAGE_LAYOUT_UNDEFINED
 * discards it.  The "Image Memory Barriers" note states the equal-layout
 * case directly: with old and new layout equal, data is preserved
 * whatever the values say.  Metadata that stands in for depth memory is
 * therefore admissible storage, and a candidate whose clauses all hold is
 * one where the substitution stays invisible to the logical image.
 *
 * Compression is a separate question and a separate verdict.  A candidate
 * asking the depth pipe to write compressed tiles raises ZB_BW_CNTL
 * WR_COMP_ENABLE, which changes what depth memory holds, while the fast
 * clear leaves every stored value alone -- so a compressed request
 * refuses at the first clause and cannot reach admission through the
 * fast-clear rows behind it.
 */

#ifndef R3V_NATIVE_ZMASK_ADMISSION_H
#define R3V_NATIVE_ZMASK_ADMISSION_H

#include "amd/r300/common/r300_chip_identity.h"
#include "amd/r300/common/r300_zb_combined_clear.h"
#include "amd/r300/common/r300_zb_hyperz_admission.h"

#include <vulkan/vulkan_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The depth image the public ZMASK lifecycle qualification describes: a
 * 64x64 VK_FORMAT_D24_UNORM_S8_UINT surface at a 64-pixel row pitch,
 * based 2048 bytes into its allocation behind the prefix guard.  The
 * qualification froze one image rather than a range, so the envelope
 * clause compares for equality: a geometry, pitch, or base outside these
 * values is a configuration the qualification says nothing about.  The
 * ZMASK RAM fit follows from the same values -- r300_zmask_layout_compute
 * resolves this level inside the 5120-dword RS480 budget -- so the
 * envelope clause pins the layout too.
 */
#define R3V_NATIVE_ZMASK_QUALIFIED_WIDTH 64u
#define R3V_NATIVE_ZMASK_QUALIFIED_HEIGHT 64u
#define R3V_NATIVE_ZMASK_QUALIFIED_PITCH_PIXELS 64u
#define R3V_NATIVE_ZMASK_QUALIFIED_BASE_BYTES 2048u
#define R3V_NATIVE_ZMASK_QUALIFIED_SAMPLE_COUNT 1u
#define R3V_NATIVE_ZMASK_QUALIFIED_FORMAT VK_FORMAT_D24_UNORM_S8_UINT

/* The verdict names the first clause the candidate fails, so a refusal
 * reports one mechanism rather than a conjunction. */
enum r3v_native_zmask_automatic_verdict {
   R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT = 0,
   /* Compressed depth writes, which the fast-clear rows never authorize. */
   R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED,
   R3V_NATIVE_ZMASK_PLATFORM_UNQUALIFIED,
   R3V_NATIVE_ZMASK_FORMAT_UNQUALIFIED,
   R3V_NATIVE_ZMASK_TILING_UNQUALIFIED,
   R3V_NATIVE_ZMASK_SAMPLE_COUNT_UNQUALIFIED,
   R3V_NATIVE_ZMASK_ENVELOPE_UNQUALIFIED,
   R3V_NATIVE_ZMASK_CLEAR_SCOPE_UNQUALIFIED,
   R3V_NATIVE_ZMASK_OWNERSHIP_UNQUALIFIED,
   R3V_NATIVE_ZMASK_METADATA_INTERVAL_UNQUALIFIED,
   R3V_NATIVE_ZMASK_METADATA_OWNER_CONFLICT,
   R3V_NATIVE_ZMASK_FALLBACK_UNQUALIFIED,
   R3V_NATIVE_ZMASK_ASPECT_OPERATION_UNQUALIFIED,
};

/* Everything the clauses read, collected by the caller so the decision is
 * a pure function of the description rather than of live driver state. */
struct r3v_native_zmask_automatic_candidate {
   /* The resolved board, which hazardous routes ask for by identity: the
    * PCI pair 1002:5974 sits on desktop Xpress 1100 systems the
    * qualification never covered, so the pair is a consistency check on
    * the resolution rather than the authority. */
   enum r300_platform_id platform_id;
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;

   /* Vulkan 1.0, Formats, "Depth/Stencil Formats": D24_UNORM_S8_UINT
    * carries 24 unsigned-normalized depth bits beside 8 stencil bits in
    * 32, the width ZMASK compresses. */
   VkFormat format;
   /* Vulkan 1.0, Resource Creation, "Images": VK_IMAGE_TILING_OPTIMAL
    * grants the implementation-dependent arrangement ZMASK needs.  ZMASK
    * covers a microtiled level, and the 8x8 compression block adds
    * macrotiling on top of that; the qualification image carries both. */
   bool microtile;
   bool macrotile;
   /* VkImageCreateInfo::samples, per Resource Creation, "Images".  ZMASK
    * resolves one sample per pixel. */
   uint32_t sample_count;

   uint32_t width;
   uint32_t height;
   uint32_t pitch_pixels;
   uint64_t base_bytes;

   /* R300_ZB_COMBINED_CLEAR_ASPECTS.  Vulkan 1.0, Clear Commands,
    * "Clearing Images Outside a Render Pass Instance": the aspect mask
    * selects which aspects vkCmdClearDepthStencilImage writes, and the
    * substitution replaces one clear covering depth and stencil
    * together. */
   uint32_t clear_aspect_mask;
   /* The clear reaches every pixel of the level.  A partial clear leaves
    * tiles whose stored depth disagrees with ZB_DEPTHCLEARVALUE, which
    * the zeroed ZMASK would then report as cleared. */
   bool full_surface_clear;

   /* RADEON_INFO_WANT_HYPERZ names one owning file descriptor, and
    * r300_packet0_check rejects a non-owner's ZB_ZMASK_PITCH write and
    * 3D_CLEAR_ZMASK packet outright. */
   enum r300_zb_hyperz_ownership hyperz_ownership;

   /* The metadata interval the substitution writes into, and whether a
    * different live image already holds the ZMASK RAM. */
   bool metadata_interval_available;
   bool conflicting_metadata_owner;

   /* The ordinary combined clear the driver falls back to, and the
    * materializer that resolves metadata into depth memory before any
    * reader that does not consult the ZMASK.  Together they are what
    * keeps the logical image equal to the metadata-backed one. */
   bool ordinary_fallback_available;
   bool materialize_scratch_initialized;

   /* Every aspect operation the active transition carries is one the
    * ZMASK representation answers.  Vulkan 1.0, Synchronization and Cache
    * Control, "Image Layout Transitions": a transition out of a matching
    * old layout preserves the range, so an operation the representation
    * cannot answer would break that preservation. */
   bool aspect_operation_supported;

   /* ZB_BW_CNTL WR_COMP_ENABLE: compressed depth writes, refused at the
    * first clause. */
   bool requests_compressed_writes;
};

/* One clause of the predicate.  holds() reads the candidate and answers
 * whether the clause admits; verdict is what the predicate returns when it
 * refuses; name is the durable identifier a report cites. */
struct r3v_native_zmask_automatic_clause {
   const char *name;
   enum r3v_native_zmask_automatic_verdict verdict;
   bool (*holds)(const struct r3v_native_zmask_automatic_candidate *c);
};

/* The clauses in the order the predicate evaluates them. */
const struct r3v_native_zmask_automatic_clause *
r3v_native_zmask_automatic_clauses(uint32_t *count_out);

/* Walks the clause table and returns the first refusal, writing the
 * refusing clause through clause_out when it is non-NULL.  A NULL
 * candidate describes nothing and refuses at the first clause. */
enum r3v_native_zmask_automatic_verdict r3v_native_zmask_automatic_admission(
   const struct r3v_native_zmask_automatic_candidate *candidate,
   const struct r3v_native_zmask_automatic_clause **clause_out);

const char *r3v_native_zmask_automatic_verdict_name(
   enum r3v_native_zmask_automatic_verdict verdict);

/* Fills the candidate the qualification froze, with the caller's resolved
 * board in place of the qualification's own.  Every other field carries
 * the qualification image's value, so a caller holding a board and
 * nothing else asks exactly one question: does this device stand where
 * the qualification stood.
 */
void r3v_native_zmask_qualification_candidate(
   enum r300_platform_id platform_id, uint32_t pci_vendor_id,
   uint32_t pci_device_id,
   struct r3v_native_zmask_automatic_candidate *out);

/* The results the promotion needs retained.  Each row names one result by
 * a durable identifier and points at the flag a record sets when that
 * result is in hand. */
#define R3V_NATIVE_ZMASK_PROMOTION_REQUIREMENT_COUNT 8u

struct r3v_native_zmask_promotion_record {
   /* The fast clear produces the image the ordinary clear produces. */
   bool fast_clear_substitution;
   /* A partial depth update through the metadata materializer keeps the
    * untouched pixels at the clear value and the updated pixels at their
    * written depth and stencil. */
   bool partial_update_through_materialization;
   /* Materialized depth memory reads back exactly, byte for byte. */
   bool materialized_exact_readback;
   /* Metadata follows image A to image B and back to A without carrying
    * B's state into A. */
   bool metadata_switch_aba;
   /* The prefix guard, storage padding, and tail guard around the surface
    * envelope all survive the substitution. */
   bool allocation_boundary_preservation;
   /* Metadata state recorded in one command buffer is the state the next
    * command buffer's submission observes. */
   bool cross_command_buffer_ordering;
   /* The result reproduces through the default Vulkan loader on the
    * public entry points, not through a driver-internal harness. */
   bool default_loader_public_path;
   /* The run leaves no DRM CS rejection, no lockup, and no reset in the
    * kernel log. */
   bool kernel_log_clean;
};

struct r3v_native_zmask_promotion_requirement {
   const char *result_id;
   const char *requirement;
   size_t retained_offset;
};

const struct r3v_native_zmask_promotion_requirement *
r3v_native_zmask_promotion_requirements(uint32_t *count_out);

/* Counts the requirements the record leaves unretained and writes the
 * first of them through first_missing_out when it is non-NULL.  A NULL
 * record retains nothing and reports every requirement missing. */
uint32_t r3v_native_zmask_promotion_missing(
   const struct r3v_native_zmask_promotion_record *record,
   const struct r3v_native_zmask_promotion_requirement **first_missing_out);

bool r3v_native_zmask_promotion_complete(
   const struct r3v_native_zmask_promotion_record *record);

/* The retained promotion record, or NULL while any requirement stands
 * outstanding.  Returning NULL is what holds automatic selection closed:
 * the device ANDs this record against the admission predicate, so the
 * absent record makes the conjunction false whatever the candidate says.
 */
const struct r3v_native_zmask_promotion_record *
r3v_native_zmask_promotion_retained(void);

/* Table self-consistency: every clause named with a distinct verdict and
 * a predicate, every requirement named with a distinct identifier and a
 * distinct flag inside the record.  Returns 0 or -EINVAL. */
int r3v_native_zmask_admission_tables_self_check(void);

#endif /* R3V_NATIVE_ZMASK_ADMISSION_H */
