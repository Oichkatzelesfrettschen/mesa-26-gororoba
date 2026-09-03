/*
 * SPDX-License-Identifier: MIT
 *
 * Compute verb ledger: the finite set of kernel shapes the R300-class
 * raster substrate can stand in for, each bound to the functional unit
 * that executes it, its exactness class, and its route status, with
 * the failure policy every route shares.  The ledger is precommitted:
 * a route opens for a verb only by moving that verb's row, and a kernel
 * outside the rows refuses at pipeline creation.
 */

#ifndef R300_COMPUTE_VERB_H
#define R300_COMPUTE_VERB_H

#include "r300_compute_job.h"
#include "r300_grid_fold.h"
#include "r300_numeric_domain.h"

#include <stdbool.h>
#include <stdint.h>

/* The verbs.  Each names one kernel shape over storage-buffer elements
 * (the flattened invocation index i addresses every range); the shape
 * text in the row is the admitted grammar's meaning, and the direct
 * SPIR-V admitter lowers a module to a job only when the job's op maps
 * onto a row whose CPU route executes. */
enum r300_compute_verb {
   /* out[i] = in[i], 32-bit patterns. */
   R300_COMPUTE_VERB_IDENTITY_MAP = 0,
   /* out[i] = c, one 32-bit pattern. */
   R300_COMPUTE_VERB_CONST_FILL,
   /* out[i] = a * in[i] + b, binary32 scalar or vec4. */
   R300_COMPUTE_VERB_UNARY_AFFINE_MAP,
   /* out[i] = f(a[i], b[i]) for f in add, sub, mul, min, max, dot4. */
   R300_COMPUTE_VERB_BINARY_ARITHMETIC_MAP,
   /* out[i] = f(in[i]) for f in rcp, rsq, sqrt, exp2, log2, sin, cos,
    * fract, floor, round: the US scalar transcendental unit. */
   R300_COMPUTE_VERB_UNARY_TRANSCENDENTAL_MAP,
   /* out[i] = f(a[i], b[i]) for f in pow, div. */
   R300_COMPUTE_VERB_BINARY_TRANSCENDENTAL_MAP,
   /* out[i] = a[i] op b[i] for op in and, or, xor on uint32, through
    * the ROP logic op over packed UNORM8 lanes. */
   R300_COMPUTE_VERB_BITWISE_LOGICOP_MAP,
   /* out[i] = sum of in[i - 1 .. i + 1] (box-3 gather). */
   R300_COMPUTE_VERB_MULTITAP_GATHER,
   /* out[i] = mask[i] ? in[i] : out[i] (KILL-masked store over a
    * pre-seeded output). */
   R300_COMPUTE_VERB_PREDICATED_STORE,
   /* out = in after N doubling passes through a ping-pong carrier. */
   R300_COMPUTE_VERB_MULTIPASS_SCAN,
   /* out[0] = reduce(in[0 .. n)) for add, min, max through the blend
    * combiner. */
   R300_COMPUTE_VERB_REDUCE,
   /* out[i] = max(a[i] - b[i], 0) on UNORM8 lanes (SUB_CLAMP). */
   R300_COMPUTE_VERB_SATURATING_DIFF,
   /* out0..3[i] = f0..3(in[i]) through four color targets. */
   R300_COMPUTE_VERB_PARALLEL_4OUT_MAP,
   /* out[i] = ~in[i] on an 8-bit stencil plane (ZS_INVERT). */
   R300_COMPUTE_VERB_STENCIL_INVERT,
   /* out[i] = ~in[i] over whole uint32 words on the host executor; the
    * ROP INVERT logic op is the candidate raster carrier and holds the
    * row's gate until its truth-table probe runs. */
   R300_COMPUTE_VERB_BITWISE_NOT_MAP,
   R300_COMPUTE_VERB_COUNT,
};

/* A verb row carries the semantics the compute grammar exposes and nothing
 * about where the work runs.  Unit, exactness, tolerance, index class,
 * contracts, evidence, and gate all differ between an operation's routes --
 * the identity map's host route is bit-exact over every admitted 32-bit
 * record while its R2VB carrier promises the FP24 exact window -- so those
 * facts live in r300_operation_route.h, joined by operation_id. */
struct r300_compute_verb_row {
   enum r300_compute_verb verb;
   const char *name;
   /* The catalog operation this verb realizes: the semantic domain, the
    * compact catalog evidence summary, and the join into the route ledger.
    * Every verb has one. */
   enum r300_operation_id operation_id;
};

/* The kernel classes the substrate cannot lower and the admitter
 * refuses by name; a class is a named refusal, never a verb.  The
 * classes are silicon constraints: no scatter (a color export lands at
 * the fragment's own coordinate), no image stores, no workgroup shared
 * memory or control barrier, no general atomic (the combiner's add,
 * min, max, and the stencil increment are the only read-modify-write
 * units), and no integer shift (neither the FP24 ALU nor the ROP
 * expresses one). */
enum r300_compute_refusal_class {
   R300_COMPUTE_REFUSAL_ARBITRARY_SCATTER = 0,
   R300_COMPUTE_REFUSAL_IMAGE_STORE,
   R300_COMPUTE_REFUSAL_SHARED_MEMORY_OR_BARRIER,
   R300_COMPUTE_REFUSAL_GENERAL_ATOMIC,
   R300_COMPUTE_REFUSAL_INTEGER_SHIFT,
   R300_COMPUTE_REFUSAL_CLASS_COUNT,
};

/* The failure policy every compute route obeys.  Each clause is a
 * contract the routes' tests name by id.
 */
enum r300_compute_failure_clause {
   /* A kernel outside the ledger, or whose row's CPU route is not
    * executing, refuses at pipeline creation with its construct named;
    * no admitted pipeline reaches an unmatched dispatch. */
   R300_COMPUTE_FAILURE_REFUSE_AT_ADMISSION = 0,
   /* A GPU route opens on the compute gate and the verb's own gate at
    * their exact values; unset, empty, and every other value keep the
    * CPU route. */
   R300_COMPUTE_FAILURE_EXACT_GATE_PER_VERB,
   /* The route resolves before submission; after the ioctl is accepted
    * no CPU re-execution of the dispatch occurs. */
   R300_COMPUTE_FAILURE_NO_FALLBACK_AFTER_SUBMIT,
   /* A refusal precedes the first application-visible write: the
    * output range is untouched and the gate token unspent. */
   R300_COMPUTE_FAILURE_REFUSE_BEFORE_WRITE,
   /* A completed GPU delivery is judged against the CPU oracle under
    * the row's exactness class; a divergence reports device loss and
    * quarantines the verb's GPU capability on the device. */
   R300_COMPUTE_FAILURE_ORACLE_DIVERGENCE_QUARANTINES,
   /* No capability bit advertises on a GPU route before the verb's
    * attended RS482 cell is retained. */
   R300_COMPUTE_FAILURE_ADVERTISE_AFTER_SILICON,
   R300_COMPUTE_FAILURE_CLAUSE_COUNT,
};

/* The ledger rows in enum order. */
const struct r300_compute_verb_row *r300_compute_verb_rows(uint32_t *count);

/* The compute-queue claim.  VK_QUEUE_COMPUTE_BIT asserts the full
 * compute contract, so the ledger advertises it unconditionally only
 * when every row executes on both routes -- the dual-route coverage predicate, the
 * ratchet that closes the gate as rows land.  That predicate measures
 * this ledger's dual-route matrix and nothing wider: the Vulkan compute
 * contract also spans the SPIR-V execution model, the descriptor and
 * memory models, workgroup shared memory, barriers, general atomics, and
 * image stores, and the refusal classes below name the ones this
 * substrate declines outright, so full coverage here is a necessary
 * condition for the unconditional bit and never a conformance verdict.
 * Until coverage closes, the bit is a nonconformant claim behind this
 * exact opt-in, and the gated claim additionally asserts that some row
 * executes on the ungated CPU route, so losing the delivered verb closes
 * the gated claim as well.
 * The GPU routes carry their own per-verb gates and never enter the
 * gated claim.  Compute pipeline creation opens with the same claim.
 * The _rows forms take a table so a test calibrates them on mutated
 * copies; the bare forms read the ledger.
 *
 * TODO: missing work --
 *           an r3v_compute_queue_contract_complete() authority over the
 *           SPIR-V execution model, the descriptor and memory models, the
 *           workgroup model, barriers, atomics, image operations, limits,
 *           and advertised features, which r300_compute_verb_queue_claim
 *           requires beside dual-route coverage before the unconditional
 *           bit opens.
 *       reason --
 *           dual-route coverage stands at 1 of 15 rows, so the
 *           unconditional branch is unreachable today and the wider
 *           contract has no consumer yet; a route landing that closed
 *           coverage would open the bit on the ledger alone.
 *       tracking-artifact --
 *           r300_compute_verb_queue_claim, enum
 *           r300_compute_refusal_class, and Vulkan 1.0 chapter 9
 *           (Compute Pipelines) with chapter 7 (Synchronization).
 */
#define R300_COMPUTE_QUEUE_CLAIM_GATE "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL"
#define R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE "1"
bool r300_compute_dual_route_coverage_complete_rows(
   const struct r300_compute_verb_row *rows, uint32_t count);
bool r300_compute_verb_queue_claim_rows(
   const struct r300_compute_verb_row *rows, uint32_t count, bool gate_open);
bool r300_compute_dual_route_coverage_complete(void);
bool r300_compute_verb_queue_claim(bool gate_open);

const struct r300_compute_verb_row *
r300_compute_verb_row(enum r300_compute_verb verb);

/* The row a job's op realizes, or NULL for an op outside the ledger. */
const struct r300_compute_verb_row *
r300_compute_verb_for_job(const struct r300_compute_job *job);

const char *r300_compute_refusal_class_name(enum r300_compute_refusal_class c);
const char *r300_compute_failure_clause_name(enum r300_compute_failure_clause c);

/* Well-formedness of a row table: rows in enum order, distinct names, and a
 * catalog operation that resolves on every row.  Route-level rules live with
 * the route table in r300_operation_route_rows_valid.  Returns true, or false
 * with *reason naming the first violated rule.  The ledger's own rows pass; a
 * test calibrates the checker on mutated copies. */
bool r300_compute_verb_rows_valid(const struct r300_compute_verb_row *rows,
                                  uint32_t count, const char **reason);

#endif /* R300_COMPUTE_VERB_H */
