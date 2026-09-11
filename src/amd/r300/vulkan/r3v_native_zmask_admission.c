/*
 * SPDX-License-Identifier: MIT
 *
 * Admission predicate for the first automatic ZMASK selection candidate.
 */

#include "r3v_native_zmask_admission.h"

#include "r3v_native_arming.h"

#include <errno.h>
#include <string.h>

static bool
clause_compression(const struct r3v_native_zmask_automatic_candidate *c)
{
   return !c->requests_compressed_writes;
}

static bool
clause_platform(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->platform_id == R3V_NATIVE_ARMING_PLATFORM &&
          c->pci_vendor_id == R3V_NATIVE_ARMING_PCI_VENDOR &&
          c->pci_device_id == R3V_NATIVE_ARMING_PCI_DEVICE;
}

static bool
clause_format(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->format == R3V_NATIVE_ZMASK_QUALIFIED_FORMAT;
}

static bool
clause_tiling(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->microtile && c->macrotile;
}

static bool
clause_sample_count(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->sample_count == R3V_NATIVE_ZMASK_QUALIFIED_SAMPLE_COUNT;
}

static bool
clause_envelope(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->width == R3V_NATIVE_ZMASK_QUALIFIED_WIDTH &&
          c->height == R3V_NATIVE_ZMASK_QUALIFIED_HEIGHT &&
          c->pitch_pixels == R3V_NATIVE_ZMASK_QUALIFIED_PITCH_PIXELS &&
          c->base_bytes == R3V_NATIVE_ZMASK_QUALIFIED_BASE_BYTES;
}

static bool
clause_clear_scope(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->clear_aspect_mask == R300_ZB_COMBINED_CLEAR_ASPECTS &&
          c->full_surface_clear;
}

static bool
clause_ownership(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->hyperz_ownership == R300_ZB_HYPERZ_OWNED;
}

static bool
clause_metadata_interval(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->metadata_interval_available;
}

static bool
clause_metadata_owner(const struct r3v_native_zmask_automatic_candidate *c)
{
   return !c->conflicting_metadata_owner;
}

static bool
clause_fallback(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->ordinary_fallback_available && c->materialize_scratch_initialized;
}

static bool
clause_aspect_operation(const struct r3v_native_zmask_automatic_candidate *c)
{
   return c->aspect_operation_supported;
}

/* The clauses in evaluation order.  Compression stands first because its
 * refusal is unconditional: a candidate asking for compressed depth writes
 * has to name compression whatever else about it disagrees, so no later
 * row can shadow the one verdict the fast-clear promotion never covers.
 * Every row after it reads one fact and the predicate stops at the first
 * that refuses, so a refusal names one mechanism.
 */
static const struct r3v_native_zmask_automatic_clause clause_rows[] = {
   {
      .name = "compressed-writes-withheld",
      .verdict = R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED,
      .holds = clause_compression,
   },
   {
      .name = "rs485m-resolved-platform",
      .verdict = R3V_NATIVE_ZMASK_PLATFORM_UNQUALIFIED,
      .holds = clause_platform,
   },
   {
      .name = "d24-unorm-s8-uint-format",
      .verdict = R3V_NATIVE_ZMASK_FORMAT_UNQUALIFIED,
      .holds = clause_format,
   },
   {
      .name = "microtiled-and-macrotiled",
      .verdict = R3V_NATIVE_ZMASK_TILING_UNQUALIFIED,
      .holds = clause_tiling,
   },
   {
      .name = "single-sample",
      .verdict = R3V_NATIVE_ZMASK_SAMPLE_COUNT_UNQUALIFIED,
      .holds = clause_sample_count,
   },
   {
      .name = "qualified-geometry-pitch-and-base",
      .verdict = R3V_NATIVE_ZMASK_ENVELOPE_UNQUALIFIED,
      .holds = clause_envelope,
   },
   {
      .name = "full-surface-combined-clear",
      .verdict = R3V_NATIVE_ZMASK_CLEAR_SCOPE_UNQUALIFIED,
      .holds = clause_clear_scope,
   },
   {
      .name = "hyperz-ownership-acquired",
      .verdict = R3V_NATIVE_ZMASK_OWNERSHIP_UNQUALIFIED,
      .holds = clause_ownership,
   },
   {
      .name = "zmask-metadata-interval-available",
      .verdict = R3V_NATIVE_ZMASK_METADATA_INTERVAL_UNQUALIFIED,
      .holds = clause_metadata_interval,
   },
   {
      .name = "no-conflicting-metadata-owner",
      .verdict = R3V_NATIVE_ZMASK_METADATA_OWNER_CONFLICT,
      .holds = clause_metadata_owner,
   },
   {
      .name = "ordinary-fallback-and-materializer",
      .verdict = R3V_NATIVE_ZMASK_FALLBACK_UNQUALIFIED,
      .holds = clause_fallback,
   },
   {
      .name = "supported-aspect-operations",
      .verdict = R3V_NATIVE_ZMASK_ASPECT_OPERATION_UNQUALIFIED,
      .holds = clause_aspect_operation,
   },
};

const struct r3v_native_zmask_automatic_clause *
r3v_native_zmask_automatic_clauses(uint32_t *count_out)
{
   if (count_out != NULL)
      *count_out = (uint32_t)(sizeof(clause_rows) / sizeof(*clause_rows));
   return clause_rows;
}

enum r3v_native_zmask_automatic_verdict
r3v_native_zmask_automatic_admission(
   const struct r3v_native_zmask_automatic_candidate *candidate,
   const struct r3v_native_zmask_automatic_clause **clause_out)
{
   if (clause_out != NULL)
      *clause_out = NULL;
   /* A candidate that describes nothing refuses at the first clause, which
    * is the compression row: the absent description is read as the request
    * the promotion never covers rather than as a board question. */
   if (candidate == NULL) {
      if (clause_out != NULL)
         *clause_out = &clause_rows[0];
      return clause_rows[0].verdict;
   }

   for (size_t index = 0u;
        index < sizeof(clause_rows) / sizeof(*clause_rows); index++) {
      if (!clause_rows[index].holds(candidate)) {
         if (clause_out != NULL)
            *clause_out = &clause_rows[index];
         return clause_rows[index].verdict;
      }
   }
   return R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT;
}

const char *
r3v_native_zmask_automatic_verdict_name(
   enum r3v_native_zmask_automatic_verdict verdict)
{
   switch (verdict) {
   case R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT:
      return "admit";
   case R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED:
      return "compression-unqualified";
   case R3V_NATIVE_ZMASK_PLATFORM_UNQUALIFIED:
      return "platform-unqualified";
   case R3V_NATIVE_ZMASK_FORMAT_UNQUALIFIED:
      return "format-unqualified";
   case R3V_NATIVE_ZMASK_TILING_UNQUALIFIED:
      return "tiling-unqualified";
   case R3V_NATIVE_ZMASK_SAMPLE_COUNT_UNQUALIFIED:
      return "sample-count-unqualified";
   case R3V_NATIVE_ZMASK_ENVELOPE_UNQUALIFIED:
      return "envelope-unqualified";
   case R3V_NATIVE_ZMASK_CLEAR_SCOPE_UNQUALIFIED:
      return "clear-scope-unqualified";
   case R3V_NATIVE_ZMASK_OWNERSHIP_UNQUALIFIED:
      return "ownership-unqualified";
   case R3V_NATIVE_ZMASK_METADATA_INTERVAL_UNQUALIFIED:
      return "metadata-interval-unqualified";
   case R3V_NATIVE_ZMASK_METADATA_OWNER_CONFLICT:
      return "metadata-owner-conflict";
   case R3V_NATIVE_ZMASK_FALLBACK_UNQUALIFIED:
      return "fallback-unqualified";
   case R3V_NATIVE_ZMASK_ASPECT_OPERATION_UNQUALIFIED:
      return "aspect-operation-unqualified";
   }
   return "unknown";
}

void
r3v_native_zmask_qualification_candidate(
   enum r300_platform_id platform_id, uint32_t pci_vendor_id,
   uint32_t pci_device_id,
   struct r3v_native_zmask_automatic_candidate *out)
{
   if (out == NULL)
      return;
   /* Every field but the board carries the qualification image's own
    * value, so the caller's one question is whether its device stands
    * where the qualification stood. */
   *out = (struct r3v_native_zmask_automatic_candidate){
      .platform_id = platform_id,
      .pci_vendor_id = pci_vendor_id,
      .pci_device_id = pci_device_id,
      .format = R3V_NATIVE_ZMASK_QUALIFIED_FORMAT,
      .microtile = true,
      .macrotile = true,
      .sample_count = R3V_NATIVE_ZMASK_QUALIFIED_SAMPLE_COUNT,
      .width = R3V_NATIVE_ZMASK_QUALIFIED_WIDTH,
      .height = R3V_NATIVE_ZMASK_QUALIFIED_HEIGHT,
      .pitch_pixels = R3V_NATIVE_ZMASK_QUALIFIED_PITCH_PIXELS,
      .base_bytes = R3V_NATIVE_ZMASK_QUALIFIED_BASE_BYTES,
      .clear_aspect_mask = R300_ZB_COMBINED_CLEAR_ASPECTS,
      .full_surface_clear = true,
      .hyperz_ownership = R300_ZB_HYPERZ_OWNED,
      .metadata_interval_available = true,
      .conflicting_metadata_owner = false,
      .ordinary_fallback_available = true,
      .materialize_scratch_initialized = true,
      .aspect_operation_supported = true,
      .requests_compressed_writes = false,
   };
}

#define REQUIREMENT_OFFSET(field) \
   offsetof(struct r3v_native_zmask_promotion_record, field)

/* The eight results the promotion consumes.  Each identifier is durable:
 * a retained bundle, a finding, and this table name the same result by the
 * same string, so a report of what stands outstanding joins across them.
 */
static const struct r3v_native_zmask_promotion_requirement requirement_rows[] = {
   {
      .result_id = "zmask-fast-clear-substitution",
      .requirement = "the fast clear produces the image the ordinary "
                     "combined clear produces",
      .retained_offset = REQUIREMENT_OFFSET(fast_clear_substitution),
   },
   {
      .result_id = "zmask-partial-update-through-materialization",
      .requirement = "a partial depth update through the materializer "
                     "holds untouched pixels at the clear value and "
                     "updated pixels at their written depth and stencil",
      .retained_offset =
         REQUIREMENT_OFFSET(partial_update_through_materialization),
   },
   {
      .result_id = "zmask-materialized-exact-readback",
      .requirement = "materialized depth memory reads back byte for byte",
      .retained_offset = REQUIREMENT_OFFSET(materialized_exact_readback),
   },
   {
      .result_id = "zmask-aba-metadata-switch",
      .requirement = "metadata follows image A to image B and back to A "
                     "without carrying B's state into A",
      .retained_offset = REQUIREMENT_OFFSET(metadata_switch_aba),
   },
   {
      .result_id = "zmask-allocation-boundary-preservation",
      .requirement = "the prefix guard, storage padding, and tail guard "
                     "around the surface envelope survive the substitution",
      .retained_offset = REQUIREMENT_OFFSET(allocation_boundary_preservation),
   },
   {
      .result_id = "zmask-cross-command-buffer-ordering",
      .requirement = "metadata state recorded in one command buffer is the "
                     "state the next submission observes",
      .retained_offset = REQUIREMENT_OFFSET(cross_command_buffer_ordering),
   },
   {
      .result_id = "zmask-default-loader-public-path",
      .requirement = "the result reproduces through the default Vulkan "
                     "loader on the public entry points",
      .retained_offset = REQUIREMENT_OFFSET(default_loader_public_path),
   },
   {
      .result_id = "zmask-kernel-log-clean",
      .requirement = "the run leaves no DRM CS rejection, lockup, or reset "
                     "in the kernel log",
      .retained_offset = REQUIREMENT_OFFSET(kernel_log_clean),
   },
};

const struct r3v_native_zmask_promotion_requirement *
r3v_native_zmask_promotion_requirements(uint32_t *count_out)
{
   if (count_out != NULL)
      *count_out =
         (uint32_t)(sizeof(requirement_rows) / sizeof(*requirement_rows));
   return requirement_rows;
}

static bool
requirement_retained(
   const struct r3v_native_zmask_promotion_requirement *requirement,
   const struct r3v_native_zmask_promotion_record *record)
{
   return *(const bool *)((const char *)record + requirement->retained_offset);
}

uint32_t
r3v_native_zmask_promotion_missing(
   const struct r3v_native_zmask_promotion_record *record,
   const struct r3v_native_zmask_promotion_requirement **first_missing_out)
{
   if (first_missing_out != NULL)
      *first_missing_out = NULL;
   const uint32_t count =
      (uint32_t)(sizeof(requirement_rows) / sizeof(*requirement_rows));
   /* A record the caller does not hold retains nothing, so every
    * requirement stands outstanding and the first row is the one to
    * report. */
   if (record == NULL) {
      if (first_missing_out != NULL)
         *first_missing_out = &requirement_rows[0];
      return count;
   }

   uint32_t missing = 0u;
   for (uint32_t index = 0u; index < count; index++) {
      if (requirement_retained(&requirement_rows[index], record))
         continue;
      if (missing == 0u && first_missing_out != NULL)
         *first_missing_out = &requirement_rows[index];
      missing++;
   }
   return missing;
}

bool
r3v_native_zmask_promotion_complete(
   const struct r3v_native_zmask_promotion_record *record)
{
   return r3v_native_zmask_promotion_missing(record, NULL) == 0u;
}

const struct r3v_native_zmask_promotion_record *
r3v_native_zmask_promotion_retained(void)
{
   /* Every requirement stands outstanding: the lifecycle qualification is
    * an offline prediction and no silicon run has retained a result
    * against it.  The absent record is what holds automatic selection
    * closed at the device. */
   return NULL;
}

int
r3v_native_zmask_admission_tables_self_check(void)
{
   const size_t clause_count = sizeof(clause_rows) / sizeof(*clause_rows);
   for (size_t index = 0u; index < clause_count; index++) {
      if (clause_rows[index].name == NULL ||
          clause_rows[index].holds == NULL ||
          clause_rows[index].verdict == R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT)
         return -EINVAL;
      for (size_t other = 0u; other < index; other++) {
         if (clause_rows[index].verdict == clause_rows[other].verdict ||
             strcmp(clause_rows[index].name, clause_rows[other].name) == 0)
            return -EINVAL;
      }
   }

   const size_t requirement_count =
      sizeof(requirement_rows) / sizeof(*requirement_rows);
   if (requirement_count != R3V_NATIVE_ZMASK_PROMOTION_REQUIREMENT_COUNT)
      return -EINVAL;
   for (size_t index = 0u; index < requirement_count; index++) {
      if (requirement_rows[index].result_id == NULL ||
          requirement_rows[index].requirement == NULL ||
          requirement_rows[index].retained_offset >=
             sizeof(struct r3v_native_zmask_promotion_record))
         return -EINVAL;
      for (size_t other = 0u; other < index; other++) {
         if (requirement_rows[index].retained_offset ==
                requirement_rows[other].retained_offset ||
             strcmp(requirement_rows[index].result_id,
                    requirement_rows[other].result_id) == 0)
            return -EINVAL;
      }
   }
   return 0;
}
