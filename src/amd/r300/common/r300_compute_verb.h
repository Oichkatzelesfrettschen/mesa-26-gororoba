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

/* The functional unit a verb's raster lowering executes on.  R300-class
 * silicon has no compute engine: a kernel maps onto the texture fetch,
 * the FP24 fragment ALU, the RB3D blend or ROP output stage, or the ZB
 * stencil unit, and the unit fixes what the lowering can promise.
 */
enum r300_compute_verb_unit {
   /* Host execution only; the CPU executor is the authority. */
   R300_COMPUTE_VERB_UNIT_HOST = 0,
   /* Bytes fetched through the TX unit and exported through the RB3D
    * color backend without arithmetic: a UNORM8 lane round-trips
    * exactly, so packed 32-bit patterns survive. */
   R300_COMPUTE_VERB_UNIT_TX_RB3D_COPY,
   /* The US fragment ALU (s1e7m16 FP24). */
   R300_COMPUTE_VERB_UNIT_US_FP24_ALU,
   /* The RB3D blend combiner (COMB_FCN ADD, MIN, MAX, SUB_CLAMP). */
   R300_COMPUTE_VERB_UNIT_RB3D_BLEND,
   /* The RB3D ROP logic op (RB3D_ROPCNTL) on packed UNORM8 lanes. */
   R300_COMPUTE_VERB_UNIT_RB3D_ROP,
   /* The ZB stencil unit over an 8-bit stencil plane. */
   R300_COMPUTE_VERB_UNIT_ZB_STENCIL,
   /* The R2VB producer carrier: records through the VAP fetch, the US
    * varying datapath, and a C4_32_FP color export. */
   R300_COMPUTE_VERB_UNIT_R2VB_CARRIER,
   /* The RB3D color-buffer clear path, with no fragment-ALU arithmetic. */
   R300_COMPUTE_VERB_UNIT_RB3D_CLEAR,
   R300_COMPUTE_VERB_UNIT_COUNT,
};

/* What a route may promise against the CPU oracle.  The class is part
 * of the verb's contract: a BIT_EXACT verb compares byte for byte; an
 * FP24 window verb compares exactly inside the s1e7m16 fixed-point
 * window (integers to 2^17, r300_grid_fold.h) and refuses outside it
 * (-EDOM at admission); an FP24_BOUNDED verb compares within the
 * declared tolerance, never tighter. */
enum r300_compute_verb_exactness {
   R300_COMPUTE_VERB_BIT_EXACT = 0,
   R300_COMPUTE_VERB_FP24_EXACT_WINDOW,
   R300_COMPUTE_VERB_FP24_BOUNDED,
};

/* A route's status for one verb.  EXECUTING routes run today; a
 * PRECOMMITTED route has its lowering, oracle, and gate named here and
 * opens by a later row movement with its tests; ABSENT names no route. */
enum r300_compute_verb_route_status {
   R300_COMPUTE_VERB_ROUTE_ABSENT = 0,
   R300_COMPUTE_VERB_ROUTE_PRECOMMITTED,
   R300_COMPUTE_VERB_ROUTE_EXECUTING,
};

/* How strong a row's evidence is, saying nothing about what the evidence
 * is about: the subject is the separate scope enum below, and the two
 * read together or not at all.  SILICON_RETAINED: a retained RS482 bundle
 * holds a bit- or tolerance-exact delivery (the Gallium-mediated lane's
 * raster-verb corpus, retired with that lane; the bundle identities ride
 * the commit message and the findings corpus).  What that bundle
 * exercised -- one functional unit's behavior, or this verb's exact route
 * -- is the row's scope, so SILICON_RETAINED at RASTER_CELL scope
 * supports a proposed route and leaves the route absent.  SOURCE_GROUNDED:
 * the unit and encoding follow from the register programming guide and the
 * ISA, with no retained delivery.  HOST: the CPU executor's own tests. */
enum r300_compute_verb_evidence {
   R300_COMPUTE_VERB_EVIDENCE_HOST = 0,
   R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED,
   R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED,
};

/* The smallest claim to which a row's evidence applies.  Scope is separate
 * from strength: a retained raster-cell observation may support a proposed
 * route, but it does not establish execution.  An executing native GPU route
 * must name evidence for the exact retained route cell.
 *
 * subject                 the claim the evidence reaches
 * HOST_EXECUTOR           the CPU executor ran it
 * UNIT_CONTRACT           the unit's encoding follows from the manuals
 * RASTER_CELL             one functional unit behaved so in a retained cell
 * NATIVE_GPU_ROUTE_CELL   this verb's own route delivered in a retained cell
 *
 * Only the last subject reaches a route.  A reader who sees an evidence
 * strength without its scope has read half a row. */
enum r300_compute_verb_evidence_scope {
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR = 0,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_UNIT_CONTRACT,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL,
};

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

struct r300_compute_verb_row {
   enum r300_compute_verb verb;
   const char *name;
   /* The catalog operation whose semantic domain and compact catalog
    * evidence/status summary this verb references.  Every verb has one. */
   enum r300_operation_id operation_id;
   /* The implementation and route contract are a pair.  Both are NONE for an
    * absent GPU route and both are concrete for a precommitted or executing
    * route. */
   enum r300_operation_implementation_id implementation_id;
   enum r300_gpu_route_contract_id gpu_route_contract_id;
   /* How the kernel consumes the invocation index (r300_grid_fold.h):
    * a GPU route bounds its invocations by this class through the
    * FP24 exact-index guards, never by the CPU ceiling. */
   enum r300_grid_index_class index_class;
   /* The raster unit the GPU route lowers onto. */
   enum r300_compute_verb_unit unit;
   enum r300_compute_verb_exactness exactness;
   /* The declared per-component relative tolerance of an FP24_BOUNDED
    * verb, zero for the exact classes. */
   float tolerance;
   enum r300_compute_verb_route_status cpu_route;
   enum r300_compute_verb_route_status gpu_route;
   enum r300_compute_verb_evidence evidence;
   enum r300_compute_verb_evidence_scope evidence_scope;
   /* The exact opt-in a GPU route for this verb takes, beside the
    * compute gate; the value that opens it is the literal "1". */
   const char *gpu_gate;
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
 * when every row executes on both routes (the conformant predicate,
 * the ratchet that closes the gate as rows land); until then the bit
 * is a nonconformant claim behind this exact opt-in, and the gated
 * claim additionally asserts that some row executes on the ungated CPU
 * route, so losing the delivered verb closes the gated claim as well.
 * The GPU routes carry their own per-verb gates and never enter the
 * gated claim.  Compute pipeline creation opens with the same claim.
 * The _rows forms take a table so a test calibrates them on mutated
 * copies; the bare forms read the ledger.
 */
#define R300_COMPUTE_QUEUE_CLAIM_GATE "R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL"
#define R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE "1"
bool r300_compute_verb_queue_conformant_rows(
   const struct r300_compute_verb_row *rows, uint32_t count);
bool r300_compute_verb_queue_claim_rows(
   const struct r300_compute_verb_row *rows, uint32_t count, bool gate_open);
bool r300_compute_verb_queue_conformant(void);
bool r300_compute_verb_queue_claim(bool gate_open);

const struct r300_compute_verb_row *
r300_compute_verb_row(enum r300_compute_verb verb);

/* The row a job's op realizes, or NULL for an op outside the ledger. */
const struct r300_compute_verb_row *
r300_compute_verb_for_job(const struct r300_compute_job *job);

const char *r300_compute_refusal_class_name(enum r300_compute_refusal_class c);
const char *r300_compute_failure_clause_name(enum r300_compute_failure_clause c);

/* Stable tokens for the four fields a route sheet renders beside a verb.
 * Each returns NULL outside its enum, so a sheet that cannot name a field
 * refuses the row rather than printing a blank column.  The evidence and
 * scope tokens are a pair: a renderer that prints one prints both, because
 * a strength without its subject reads as a route claim it does not make. */
const char *r300_compute_verb_unit_name(enum r300_compute_verb_unit u);
const char *r300_compute_verb_route_status_name(
   enum r300_compute_verb_route_status s);
const char *r300_compute_verb_evidence_name(enum r300_compute_verb_evidence e);
const char *r300_compute_verb_evidence_scope_name(
   enum r300_compute_verb_evidence_scope s);

/* Well-formedness of a row table: rows in enum order with distinct
 * names and distinct gate names, an index class inside the grid-fold
 * enum, every FP24_BOUNDED row carrying a
 * positive tolerance and every exact row zero, an EXECUTING GPU route
 * only with SILICON_RETAINED evidence scoped to that exact route cell,
 * raster-cell evidence only on absent or precommitted routes, and a HOST unit
 * only on rows whose GPU route is absent.  Returns true, or false with *reason
 * naming the first violated rule.  The ledger's own rows pass; a test
 * calibrates the checker on mutated copies. */
bool r300_compute_verb_rows_valid(const struct r300_compute_verb_row *rows,
                                  uint32_t count, const char **reason);

#endif /* R300_COMPUTE_VERB_H */
