/*
 * SPDX-License-Identifier: MIT
 *
 * The sequence that qualifies compressed ZMASK contents, as data a
 * harness executes step by step.
 *
 * The ladder's compressed stages establish the register configuration;
 * they do not by themselves establish that the metadata carries anything
 * but zeros.  Stage C clears the whole ZMASK, so every tile is in the
 * cleared state and stage E's decompression group reproduces stage D's
 * image, an observation equally consistent with the metadata being
 * ignored.  A compressed tile reaches the ZMASK only from a compressed
 * write, so the sequence opens with nonuniform contents and a draw that
 * cannot remain a uniform clear, and only then reads the metadata back.
 *
 * A plan is evidence only when it carries a discriminator: a step whose
 * observation the fast-clear substitution cannot produce.  A plan without
 * one is refused, because a run that reaches its last step still proves
 * nothing about the compressed representation.
 *
 * The contract the sequence qualifies is an API contract as well as a
 * register one.  Vulkan Resource Creation, Image Layouts stores an image
 * in an implementation-dependent opaque layout, which is what admits a
 * compressed ZMASK representation as storage for the same logical image;
 * Vulkan Synchronization and Cache Control, Image Layout Transitions
 * requires a transition whose old layout matches the current layout to
 * preserve the contents of the range, which is what the switch-away and
 * compressed-read steps exercise, and the materialized bytes are the
 * bridge back to the ordinary uncompressed model.
 */

#ifndef R300_ZMASK_COMPRESSED_QUALIFICATION_H
#define R300_ZMASK_COMPRESSED_QUALIFICATION_H

#include "r300_zmask_clear_plan.h"

#include <stdbool.h>
#include <stdint.h>

/* The sequence in execution order.  Each step is one observable action
 * on the surface, and the order is the dependency order: nothing can
 * read a compressed tile before a compressed write produces one, and
 * nothing verifies the materialized bytes while the metadata still
 * answers the reads.
 */
enum r300_zmask_qualification_step {
   /* Ordinary uncompressed initialization to contents no uniform value
    * reproduces, so a later uniform image is a defect rather than a
    * pass. */
   R300_ZMASK_QUALIFICATION_INITIALIZE_NONUNIFORM = 0,
   /* A draw under the in-use group whose fragments write depth values
    * that vary across the surface, so the resulting tiles cannot be the
    * cleared state. */
   R300_ZMASK_QUALIFICATION_COMPRESSED_WRITE_DRAW,
   /* Bind something else and come back, so the compressed contents
    * survive a switch rather than living in a cache that never
    * retired. */
   R300_ZMASK_QUALIFICATION_SWITCH_AWAY,
   /* Read the surface back under the decompression group, which is the
    * configuration that consults the metadata. */
   R300_ZMASK_QUALIFICATION_COMPRESSED_READ,
   /* Store every compressed tile to depth memory, which is the bridge
    * back to the ordinary uncompressed representation. */
   R300_ZMASK_QUALIFICATION_MATERIALIZE,
   /* Return ZB_BW_CNTL to zero, so the following read reaches depth
    * memory and the metadata explains nothing about it. */
   R300_ZMASK_QUALIFICATION_DISABLE_METADATA_DEPENDENCE,
   /* Read the materialized bytes through the qualified tiled address
    * resolver, so the image the host sees is the image the ZB wrote. */
   R300_ZMASK_QUALIFICATION_VERIFY_THROUGH_RESOLVER,
   /* Compare to an oracle computed without the ZB, so the verdict rests
    * on a source the compressed path did not produce. */
   R300_ZMASK_QUALIFICATION_COMPARE_TO_ORACLE,
   R300_ZMASK_QUALIFICATION_STEP_COUNT,
};

struct r300_zmask_qualification_step_row {
   enum r300_zmask_qualification_step step;
   /* The ladder stage the step runs at.  A step that establishes no
    * ZMASK state runs at R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY. */
   enum r300_zmask_clear_stage stage;
   /* The step's observation cannot be produced by the fast-clear
    * substitution, so it separates a run that used the metadata from a
    * run that read depth memory straight through. */
   bool discriminator;
   const char *name;
   /* The explanation the step removes, named so a reader sees what the
    * plan would still admit without it. */
   const char *refutes;
};

/* One named sequence.  target_class is the class the sequence sets out to
 * demonstrate and required_stage is the ladder stage that must build for
 * the sequence to run at all, which is the stage whose evidence class
 * equals target_class. */
struct r300_zmask_qualification_plan {
   const char *name;
   const struct r300_zmask_qualification_step_row *steps;
   uint32_t step_count;
   enum r300_zmask_clear_stage required_stage;
   enum r300_zmask_evidence_class target_class;
};

const char *r300_zmask_qualification_step_name(
   enum r300_zmask_qualification_step step);

/* The shipped sequence: the eight steps above, targeting the
 * compressed-write class at R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED. */
const struct r300_zmask_qualification_plan *
r300_zmask_compressed_content_qualification(void);

/* The discriminators a plan carries. */
uint32_t r300_zmask_qualification_discriminator_count(
   const struct r300_zmask_qualification_plan *plan);

/* Whether a plan stands as evidence: it names itself, carries steps in
 * execution order with each row naming its own step, names a
 * required_stage whose evidence class is the target class, and carries at
 * least one discriminator.  A plan whose every step the fast-clear
 * substitution explains is -EINVAL however many steps it runs, because
 * its last step observes nothing the metadata had to participate in.
 * Returns 0 or -EINVAL.
 */
int r300_zmask_qualification_plan_check(
   const struct r300_zmask_qualification_plan *plan);

/* The plan-level prerequisite the ZMASK_COMPRESSED materializer
 * consumes.  Materializing a compressed representation reads tiles whose
 * encoding nothing in this tree has observed, so the materializer opens
 * on a plan that checks out and targets the compressed-write class; a
 * plan that reaches only the fast-clear or compressed-read class leaves
 * the compressed encoding unqualified and the materializer refuses.
 */
bool r300_zmask_qualification_materialize_admitted(
   const struct r300_zmask_qualification_plan *plan);

/* The shipped plan held to the rules above. */
int r300_zmask_compressed_qualification_self_check(void);

#endif /* R300_ZMASK_COMPRESSED_QUALIFICATION_H */
