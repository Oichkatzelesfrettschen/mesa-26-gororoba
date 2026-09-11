/* SPDX-License-Identifier: MIT */

/* The compressed-content qualification sequence: the shipped plan's
 * steps and their order, the stage and class it targets, and the
 * refusal of a plan that carries no discriminator.
 */

#include "r300_zmask_clear_plan.h"
#include "r300_zmask_compressed_qualification.h"

/* The checks are the test; a release build keeps them. */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* A caller's copy of the shipped plan, so a mutation calibrates the
 * checker without touching the plan every other caller reads. */
struct mutable_plan {
   struct r300_zmask_qualification_step_row
      steps[R300_ZMASK_QUALIFICATION_STEP_COUNT];
   struct r300_zmask_qualification_plan plan;
};

static void
copy_shipped(struct mutable_plan *out)
{
   const struct r300_zmask_qualification_plan *shipped =
      r300_zmask_compressed_content_qualification();
   assert(shipped->step_count ==
          (uint32_t)R300_ZMASK_QUALIFICATION_STEP_COUNT);
   memcpy(out->steps, shipped->steps,
          shipped->step_count * sizeof(out->steps[0]));
   out->plan = *shipped;
   out->plan.steps = out->steps;
   assert(r300_zmask_qualification_plan_check(&out->plan) == 0);
}

/* The shipped sequence, read step by step: nonuniform initialization, a
 * compressed-write draw, a switch away, a compressed read, the
 * materialize, the disable, the resolver read, and the oracle
 * comparison, in that order and each at the ladder stage its
 * configuration needs. */
static void
check_shipped_plan(void)
{
   const struct r300_zmask_qualification_plan *plan =
      r300_zmask_compressed_content_qualification();
   assert(r300_zmask_compressed_qualification_self_check() == 0);
   assert(r300_zmask_qualification_plan_check(plan) == 0);
   assert(plan->target_class == R300_ZMASK_EVIDENCE_COMPRESSED_WRITE);
   assert(plan->required_stage ==
          R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED);

   /* The stage the plan names is the stage that reaches the class it
    * targets, which is the join the checker enforces. */
   enum r300_zmask_evidence_class stage_class;
   assert(r300_zmask_clear_stage_evidence_class(plan->required_stage,
                                                &stage_class));
   assert(stage_class == plan->target_class);

   assert(plan->steps[R300_ZMASK_QUALIFICATION_COMPRESSED_WRITE_DRAW].stage ==
          R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED);
   assert(plan->steps[R300_ZMASK_QUALIFICATION_COMPRESSED_READ].stage ==
          R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED);
   assert(plan->steps[R300_ZMASK_QUALIFICATION_MATERIALIZE].stage ==
          R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED);
   assert(plan->steps[R300_ZMASK_QUALIFICATION_DISABLE_METADATA_DEPENDENCE]
             .stage == R300_ZMASK_CLEAR_STAGE_BIND_CLEAR);

   /* Two discriminators, at the draw that cannot be a uniform clear and
    * at the comparison the ZB path did not produce. */
   assert(r300_zmask_qualification_discriminator_count(plan) == 2);
   assert(plan->steps[R300_ZMASK_QUALIFICATION_COMPRESSED_WRITE_DRAW]
             .discriminator);
   assert(plan->steps[R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE]
             .discriminator);

   for (int i = 0; i < R300_ZMASK_QUALIFICATION_STEP_COUNT; i++)
      assert(r300_zmask_qualification_step_name(
                (enum r300_zmask_qualification_step)i) != NULL);
   assert(r300_zmask_qualification_step_name(
             R300_ZMASK_QUALIFICATION_STEP_COUNT) == NULL);
}

/* The refusal the descriptor exists for: a plan whose every observation
 * the fast-clear substitution reproduces proves nothing about compressed
 * metadata, so it is refused as evidence however many steps it runs.
 */
static void
check_discriminator_free_plan_refused(void)
{
   struct mutable_plan probe;
   copy_shipped(&probe);
   for (uint32_t i = 0; i < probe.plan.step_count; i++)
      probe.steps[i].discriminator = false;
   assert(r300_zmask_qualification_discriminator_count(&probe.plan) == 0);
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);
   assert(!r300_zmask_qualification_materialize_admitted(&probe.plan));

   /* One discriminator restored is enough, and the full sequence still
    * runs: the refusal reads the flag, not the step count. */
   probe.steps[R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE].discriminator =
      true;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == 0);
   assert(r300_zmask_qualification_materialize_admitted(&probe.plan));
}

/* The remaining refusals, each calibrated on a single mutation of the
 * shipped plan so one rule fails at a time. */
static void
check_malformed_plans_refused(void)
{
   assert(r300_zmask_qualification_plan_check(NULL) == -EINVAL);
   assert(!r300_zmask_qualification_materialize_admitted(NULL));
   assert(r300_zmask_qualification_discriminator_count(NULL) == 0);

   struct mutable_plan probe;

   copy_shipped(&probe);
   probe.plan.steps = NULL;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   copy_shipped(&probe);
   probe.plan.step_count = 0u;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   copy_shipped(&probe);
   probe.plan.name = NULL;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   /* A stage that establishes no ZMASK read reaches no class, so it
    * cannot be the stage a plan requires. */
   copy_shipped(&probe);
   probe.plan.required_stage = R300_ZMASK_CLEAR_STAGE_BIND_CLEAR;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   /* A stage whose class is not the plan's target is refused as well,
    * so a compressed-write sequence cannot ride the fast-fill stage. */
   copy_shipped(&probe);
   probe.plan.required_stage = R300_ZMASK_CLEAR_STAGE_FAST_FILL;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   /* A plan that targets a lower class checks out and still leaves the
    * compressed encoding unqualified, which is what the materializer
    * prerequisite reads. */
   copy_shipped(&probe);
   probe.plan.required_stage = R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED;
   probe.plan.target_class = R300_ZMASK_EVIDENCE_COMPRESSED_READ;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == 0);
   assert(!r300_zmask_qualification_materialize_admitted(&probe.plan));

   copy_shipped(&probe);
   probe.plan.target_class = R300_ZMASK_EVIDENCE_CLASS_COUNT;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   /* Rows stand in execution order, so a row naming another step is a
    * sequence the harness would run out of order. */
   copy_shipped(&probe);
   probe.steps[R300_ZMASK_QUALIFICATION_SWITCH_AWAY].step =
      R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   copy_shipped(&probe);
   probe.steps[0].name = NULL;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   copy_shipped(&probe);
   probe.steps[0].refutes = NULL;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);

   copy_shipped(&probe);
   probe.steps[0].stage = (enum r300_zmask_clear_stage)99;
   assert(r300_zmask_qualification_plan_check(&probe.plan) == -EINVAL);
}

int
main(void)
{
   check_shipped_plan();
   check_discriminator_free_plan_refused();
   check_malformed_plans_refused();

   const struct r300_zmask_qualification_plan *plan =
      r300_zmask_compressed_content_qualification();
   printf("%s: %u steps, %u discriminators, stage %s\n", plan->name,
          plan->step_count,
          r300_zmask_qualification_discriminator_count(plan),
          r300_zmask_clear_stage_name(plan->required_stage));
   for (uint32_t i = 0; i < plan->step_count; i++) {
      printf("  %u %-56s %s\n", i, plan->steps[i].name,
             plan->steps[i].discriminator ? "discriminator" : "");
   }
   return 0;
}
