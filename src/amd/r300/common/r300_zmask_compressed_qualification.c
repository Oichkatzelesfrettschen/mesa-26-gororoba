/* SPDX-License-Identifier: MIT */

#include "r300_zmask_compressed_qualification.h"

#include <errno.h>
#include <stddef.h>

/* The two discriminators sit at opposite ends of the sequence and remove
 * different explanations.  The compressed-write draw removes the uniform
 * clear: a surface whose depth varies across it is not the cleared state
 * whatever the metadata says.  The oracle comparison removes the bypass:
 * bytes that match a value the ZB never produced are bytes the compressed
 * path carried, and the disable step before it means the metadata answers
 * none of that read.
 */
static const struct r300_zmask_qualification_step_row compressed_steps[] = {
   {R300_ZMASK_QUALIFICATION_INITIALIZE_NONUNIFORM,
    R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, false,
    "nonuniform ordinary initialization",
    "a uniform image that any clear value reproduces"},
   {R300_ZMASK_QUALIFICATION_COMPRESSED_WRITE_DRAW,
    R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED, true,
    "compressed-write draw that cannot remain a uniform clear",
    "the fast-clear substitution explaining the contents"},
   {R300_ZMASK_QUALIFICATION_SWITCH_AWAY,
    R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, false, "switch away and return",
    "contents living in a cache that never retired"},
   {R300_ZMASK_QUALIFICATION_COMPRESSED_READ,
    R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED, false,
    "compressed read under the decompression group",
    "the written tiles being readable only under the group that wrote "
    "them"},
   {R300_ZMASK_QUALIFICATION_MATERIALIZE,
    R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED, false,
    "materialize every compressed tile to depth memory",
    "contents reachable only while the metadata is bound"},
   {R300_ZMASK_QUALIFICATION_DISABLE_METADATA_DEPENDENCE,
    R300_ZMASK_CLEAR_STAGE_BIND_CLEAR, false,
    "return ZB_BW_CNTL to zero",
    "the metadata answering the verifying read"},
   {R300_ZMASK_QUALIFICATION_VERIFY_THROUGH_RESOLVER,
    R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, false,
    "read back through the qualified tiled address resolver",
    "an address model that reads the wrong words"},
   {R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE,
    R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY, true,
    "compare to an independent oracle",
    "the ZB path grading its own output"},
};

#define COMPRESSED_STEP_COUNT \
   (sizeof(compressed_steps) / sizeof(compressed_steps[0]))

static const struct r300_zmask_qualification_plan compressed_plan = {
   .name = "compressed ZMASK content qualification",
   .steps = compressed_steps,
   .step_count = (uint32_t)COMPRESSED_STEP_COUNT,
   .required_stage = R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED,
   .target_class = R300_ZMASK_EVIDENCE_COMPRESSED_WRITE,
};

const struct r300_zmask_qualification_plan *
r300_zmask_compressed_content_qualification(void)
{
   return &compressed_plan;
}

const char *
r300_zmask_qualification_step_name(enum r300_zmask_qualification_step step)
{
   if ((uint32_t)step >= COMPRESSED_STEP_COUNT)
      return NULL;
   return compressed_steps[step].name;
}

uint32_t
r300_zmask_qualification_discriminator_count(
   const struct r300_zmask_qualification_plan *plan)
{
   if (plan == NULL || plan->steps == NULL)
      return 0u;
   uint32_t count = 0u;
   for (uint32_t i = 0; i < plan->step_count; i++) {
      if (plan->steps[i].discriminator)
         count++;
   }
   return count;
}

int
r300_zmask_qualification_plan_check(
   const struct r300_zmask_qualification_plan *plan)
{
   if (plan == NULL || plan->steps == NULL || plan->step_count == 0u ||
       plan->name == NULL ||
       plan->target_class >= R300_ZMASK_EVIDENCE_CLASS_COUNT)
      return -EINVAL;

   /* The stage the plan names must be the stage that reaches the class
    * the plan targets, so a sequence cannot claim a class its own
    * register configuration never enables. */
   enum r300_zmask_evidence_class stage_class;
   if (!r300_zmask_clear_stage_evidence_class(plan->required_stage,
                                              &stage_class) ||
       stage_class != plan->target_class)
      return -EINVAL;

   for (uint32_t i = 0; i < plan->step_count; i++) {
      const struct r300_zmask_qualification_step_row *row = &plan->steps[i];
      if (row->step != (enum r300_zmask_qualification_step)i ||
          row->name == NULL || row->refutes == NULL)
         return -EINVAL;
      if (r300_zmask_clear_stage_name(row->stage) == NULL)
         return -EINVAL;
   }

   /* The refusal the plan exists to enforce: a sequence whose every
    * observation the fast-clear substitution reproduces is not evidence
    * for a compressed representation, however far it runs. */
   if (r300_zmask_qualification_discriminator_count(plan) == 0u)
      return -EINVAL;

   return 0;
}

bool
r300_zmask_qualification_materialize_admitted(
   const struct r300_zmask_qualification_plan *plan)
{
   return r300_zmask_qualification_plan_check(plan) == 0 &&
          plan->target_class == R300_ZMASK_EVIDENCE_COMPRESSED_WRITE;
}

int
r300_zmask_compressed_qualification_self_check(void)
{
   const struct r300_zmask_qualification_plan *plan =
      r300_zmask_compressed_content_qualification();
   if (r300_zmask_qualification_plan_check(plan) != 0)
      return -EINVAL;
   if (plan->step_count != (uint32_t)R300_ZMASK_QUALIFICATION_STEP_COUNT)
      return -EINVAL;
   /* The sequence's own dependency order: the compressed write precedes
    * the compressed read, the materialize precedes the disable, and the
    * disable precedes both verifying steps. */
   if (R300_ZMASK_QUALIFICATION_COMPRESSED_WRITE_DRAW >=
          R300_ZMASK_QUALIFICATION_COMPRESSED_READ ||
       R300_ZMASK_QUALIFICATION_MATERIALIZE >=
          R300_ZMASK_QUALIFICATION_DISABLE_METADATA_DEPENDENCE ||
       R300_ZMASK_QUALIFICATION_DISABLE_METADATA_DEPENDENCE >=
          R300_ZMASK_QUALIFICATION_VERIFY_THROUGH_RESOLVER ||
       R300_ZMASK_QUALIFICATION_VERIFY_THROUGH_RESOLVER >=
          R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE)
      return -EINVAL;
   if (!r300_zmask_qualification_materialize_admitted(plan))
      return -EINVAL;
   return r300_zmask_clear_stages_self_check();
}
