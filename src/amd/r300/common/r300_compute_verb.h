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

/* The evidence class behind a row.  SILICON_RETAINED: the lowering
 * delivered bit- or tolerance-exact results on RS482 in a retained
 * bundle (the Gallium-mediated lane's raster-verb corpus, retired with
 * that lane; the bundle identities ride the commit message and the
 * findings corpus).  SOURCE_GROUNDED: the unit and encoding follow from
 * the register programming guide and the ISA, with no retained
 * delivery.  HOST: the CPU executor's own tests. */
enum r300_compute_verb_evidence {
   R300_COMPUTE_VERB_EVIDENCE_HOST = 0,
   R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED,
   R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED,
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
   R300_COMPUTE_VERB_COUNT,
};

struct r300_compute_verb_row {
   enum r300_compute_verb verb;
   const char *name;
   /* The r300_virtual_op_catalog row (r300_numeric_domain.h) whose
    * domain and status the verb inherits, or NULL for a verb the catalog
    * carries no row for; the ledger test resolves every named op. */
   const char *catalog_op;
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

const struct r300_compute_verb_row *
r300_compute_verb_row(enum r300_compute_verb verb);

/* The row a job's op realizes, or NULL for an op outside the ledger. */
const struct r300_compute_verb_row *
r300_compute_verb_for_job(const struct r300_compute_job *job);

const char *r300_compute_refusal_class_name(enum r300_compute_refusal_class c);
const char *r300_compute_failure_clause_name(enum r300_compute_failure_clause c);

/* Well-formedness of a row table: rows in enum order with distinct
 * names and distinct gate names, an index class inside the grid-fold
 * enum, every FP24_BOUNDED row carrying a
 * positive tolerance and every exact row zero, an EXECUTING GPU route
 * only with SILICON_RETAINED evidence, and a HOST unit only on rows
 * whose GPU route is absent.  Returns true, or false with *reason
 * naming the first violated rule.  The ledger's own rows pass; a test
 * calibrates the checker on mutated copies. */
bool r300_compute_verb_rows_valid(const struct r300_compute_verb_row *rows,
                                  uint32_t count, const char **reason);

#endif /* R300_COMPUTE_VERB_H */
