/* SPDX-License-Identifier: MIT */

/* The verdicts are asserted, so the translation unit keeps assert live
 * whatever the profile defines. */
#undef NDEBUG

#include "../r3v_native_arming.h"
#include "../r3v_native_zmask_admission.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* One clause's known-bad: the mutation that breaks exactly that clause on
 * an otherwise admitting candidate.  The table is matched against the
 * clause table by verdict, and the counts are compared, so a clause added
 * without a mutation fails here instead of going untested.
 */
struct clause_mutation {
   enum r3v_native_zmask_automatic_verdict verdict;
   void (*mutate)(struct r3v_native_zmask_automatic_candidate *c);
};

static void
mutate_compression(struct r3v_native_zmask_automatic_candidate *c)
{
   c->requests_compressed_writes = true;
}

static void
mutate_platform(struct r3v_native_zmask_automatic_candidate *c)
{
   c->platform_id = R300_PLATFORM_ID_NONE;
}

static void
mutate_format(struct r3v_native_zmask_automatic_candidate *c)
{
   c->format = VK_FORMAT_D16_UNORM;
}

static void
mutate_tiling(struct r3v_native_zmask_automatic_candidate *c)
{
   c->macrotile = false;
}

static void
mutate_sample_count(struct r3v_native_zmask_automatic_candidate *c)
{
   c->sample_count = 4u;
}

static void
mutate_envelope(struct r3v_native_zmask_automatic_candidate *c)
{
   c->pitch_pixels = R3V_NATIVE_ZMASK_QUALIFIED_PITCH_PIXELS * 2u;
}

static void
mutate_clear_scope(struct r3v_native_zmask_automatic_candidate *c)
{
   c->clear_aspect_mask = R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH;
}

static void
mutate_ownership(struct r3v_native_zmask_automatic_candidate *c)
{
   c->hyperz_ownership = R300_ZB_HYPERZ_UNOWNED;
}

static void
mutate_metadata_interval(struct r3v_native_zmask_automatic_candidate *c)
{
   c->metadata_interval_available = false;
}

static void
mutate_metadata_owner(struct r3v_native_zmask_automatic_candidate *c)
{
   c->conflicting_metadata_owner = true;
}

static void
mutate_fallback(struct r3v_native_zmask_automatic_candidate *c)
{
   c->materialize_scratch_initialized = false;
}

static void
mutate_aspect_operation(struct r3v_native_zmask_automatic_candidate *c)
{
   c->aspect_operation_supported = false;
}

static const struct clause_mutation clause_mutations[] = {
   {R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED, mutate_compression},
   {R3V_NATIVE_ZMASK_PLATFORM_UNQUALIFIED, mutate_platform},
   {R3V_NATIVE_ZMASK_FORMAT_UNQUALIFIED, mutate_format},
   {R3V_NATIVE_ZMASK_TILING_UNQUALIFIED, mutate_tiling},
   {R3V_NATIVE_ZMASK_SAMPLE_COUNT_UNQUALIFIED, mutate_sample_count},
   {R3V_NATIVE_ZMASK_ENVELOPE_UNQUALIFIED, mutate_envelope},
   {R3V_NATIVE_ZMASK_CLEAR_SCOPE_UNQUALIFIED, mutate_clear_scope},
   {R3V_NATIVE_ZMASK_OWNERSHIP_UNQUALIFIED, mutate_ownership},
   {R3V_NATIVE_ZMASK_METADATA_INTERVAL_UNQUALIFIED, mutate_metadata_interval},
   {R3V_NATIVE_ZMASK_METADATA_OWNER_CONFLICT, mutate_metadata_owner},
   {R3V_NATIVE_ZMASK_FALLBACK_UNQUALIFIED, mutate_fallback},
   {R3V_NATIVE_ZMASK_ASPECT_OPERATION_UNQUALIFIED, mutate_aspect_operation},
};

#define CLAUSE_MUTATION_COUNT \
   (sizeof(clause_mutations) / sizeof(*clause_mutations))

static struct r3v_native_zmask_automatic_candidate
admitting_candidate(void)
{
   struct r3v_native_zmask_automatic_candidate candidate;
   r3v_native_zmask_qualification_candidate(
      R3V_NATIVE_ARMING_PLATFORM, R3V_NATIVE_ARMING_PCI_VENDOR,
      R3V_NATIVE_ARMING_PCI_DEVICE, &candidate);
   return candidate;
}

static const struct clause_mutation *
mutation_for(enum r3v_native_zmask_automatic_verdict verdict)
{
   for (size_t index = 0u; index < CLAUSE_MUTATION_COUNT; index++) {
      if (clause_mutations[index].verdict == verdict)
         return &clause_mutations[index];
   }
   return NULL;
}

/* The known-good: the qualification's own candidate on the authorized
 * board admits, so a later refusal is the mutation's doing. */
static void
test_known_good_admits(void)
{
   const struct r3v_native_zmask_automatic_candidate candidate =
      admitting_candidate();
   const struct r3v_native_zmask_automatic_clause *clause = NULL;
   assert(r3v_native_zmask_automatic_admission(&candidate, &clause) ==
          R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT);
   assert(clause == NULL);
}

/* Every clause, one mutation apiece: the predicate names exactly the
 * clause the mutation broke. */
static void
test_every_clause_names_itself(void)
{
   uint32_t clause_count = 0u;
   const struct r3v_native_zmask_automatic_clause *clauses =
      r3v_native_zmask_automatic_clauses(&clause_count);
   assert(clauses != NULL);
   assert(clause_count == (uint32_t)CLAUSE_MUTATION_COUNT);

   for (uint32_t index = 0u; index < clause_count; index++) {
      const struct clause_mutation *mutation =
         mutation_for(clauses[index].verdict);
      assert(mutation != NULL);

      struct r3v_native_zmask_automatic_candidate candidate =
         admitting_candidate();
      mutation->mutate(&candidate);

      const struct r3v_native_zmask_automatic_clause *named = NULL;
      const enum r3v_native_zmask_automatic_verdict verdict =
         r3v_native_zmask_automatic_admission(&candidate, &named);
      assert(verdict == clauses[index].verdict);
      assert(named == &clauses[index]);
      assert(strcmp(r3v_native_zmask_automatic_verdict_name(verdict),
                    "unknown") != 0);
   }
}

/* Compression stands first, so a candidate that requests compressed writes
 * names compression even while a second clause also refuses. */
static void
test_compression_precedes_every_clause(void)
{
   for (size_t index = 0u; index < CLAUSE_MUTATION_COUNT; index++) {
      struct r3v_native_zmask_automatic_candidate candidate =
         admitting_candidate();
      clause_mutations[index].mutate(&candidate);
      candidate.requests_compressed_writes = true;
      assert(r3v_native_zmask_automatic_admission(&candidate, NULL) ==
             R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED);
   }

   assert(r3v_native_zmask_automatic_admission(NULL, NULL) ==
          R3V_NATIVE_ZMASK_COMPRESSION_UNQUALIFIED);
}

static struct r3v_native_zmask_promotion_record
complete_record(void)
{
   struct r3v_native_zmask_promotion_record record;
   uint32_t count = 0u;
   const struct r3v_native_zmask_promotion_requirement *requirements =
      r3v_native_zmask_promotion_requirements(&count);
   memset(&record, 0, sizeof(record));
   for (uint32_t index = 0u; index < count; index++) {
      *(bool *)((char *)&record + requirements[index].retained_offset) = true;
   }
   return record;
}

/* The requirement evaluator against an empty, a partial, and a complete
 * record. */
static void
test_promotion_requirements(void)
{
   uint32_t count = 0u;
   const struct r3v_native_zmask_promotion_requirement *requirements =
      r3v_native_zmask_promotion_requirements(&count);
   assert(requirements != NULL);
   assert(count == R3V_NATIVE_ZMASK_PROMOTION_REQUIREMENT_COUNT);

   struct r3v_native_zmask_promotion_record empty;
   memset(&empty, 0, sizeof(empty));
   const struct r3v_native_zmask_promotion_requirement *first = NULL;
   assert(r3v_native_zmask_promotion_missing(&empty, &first) == count);
   assert(first == &requirements[0]);
   assert(!r3v_native_zmask_promotion_complete(&empty));

   /* Partial: each single retained result leaves the other seven, and the
    * first missing row is the first one the record does not hold. */
   for (uint32_t index = 0u; index < count; index++) {
      struct r3v_native_zmask_promotion_record partial;
      memset(&partial, 0, sizeof(partial));
      *(bool *)((char *)&partial + requirements[index].retained_offset) = true;
      first = NULL;
      assert(r3v_native_zmask_promotion_missing(&partial, &first) ==
             count - 1u);
      assert(first == &requirements[index == 0u ? 1u : 0u]);
      assert(!r3v_native_zmask_promotion_complete(&partial));
   }

   const struct r3v_native_zmask_promotion_record complete = complete_record();
   first = NULL;
   assert(r3v_native_zmask_promotion_missing(&complete, &first) == 0u);
   assert(first == NULL);
   assert(r3v_native_zmask_promotion_complete(&complete));

   /* A record the caller does not hold retains nothing. */
   first = NULL;
   assert(r3v_native_zmask_promotion_missing(NULL, &first) == count);
   assert(first == &requirements[0]);
   assert(!r3v_native_zmask_promotion_complete(NULL));
}

/* Automatic selection is closed by construction: the retained record is
 * absent, so the device's conjunction is false whatever its candidate
 * says. */
static void
test_promotion_record_absent(void)
{
   assert(r3v_native_zmask_promotion_retained() == NULL);
   assert(!r3v_native_zmask_promotion_complete(
      r3v_native_zmask_promotion_retained()));

   const struct r3v_native_zmask_automatic_candidate candidate =
      admitting_candidate();
   const bool device_expression =
      r3v_native_zmask_promotion_complete(
         r3v_native_zmask_promotion_retained()) &&
      r3v_native_zmask_automatic_admission(&candidate, NULL) ==
         R3V_NATIVE_ZMASK_AUTOMATIC_ADMIT;
   assert(!device_expression);
}

int
main(void)
{
   assert(r3v_native_zmask_admission_tables_self_check() == 0);
   test_known_good_admits();
   test_every_clause_names_itself();
   test_compression_precedes_every_clause();
   test_promotion_requirements();
   test_promotion_record_absent();
   printf("r3v_native_zmask_admission_test: OK\n");
   return 0;
}
