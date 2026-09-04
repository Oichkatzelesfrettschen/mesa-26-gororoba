/*
 * SPDX-License-Identifier: MIT
 *
 * Operation route ledger: the routes that realize a catalog operation, one
 * row per route rather than one row per operation.  An operation names what
 * a kernel computes; a route names where and how it runs, and the two carry
 * different facts.  The identity map shows why: its host route moves every
 * admitted 32-bit record bit-exactly, while its R2VB carrier route promises
 * the FP24 exact window, so exactness, index class, evidence, and gate are
 * route properties and only the operation identity is shared.
 *
 * The join key is enum r300_operation_id.  A compute verb resolves to one
 * operation, an operation resolves to zero or more routes, and a route
 * carries its own executor, maturity, unit, contracts, exactness bound,
 * evidence pair, and opt-in gate.
 *
 * Route identity is stable across maturity: a route keeps its id when it
 * advances from CANDIDATE to EXECUTING, so an identity never encodes a
 * status word.  An operation with no candidate carries no row.
 */

#ifndef R300_OPERATION_ROUTE_H
#define R300_OPERATION_ROUTE_H

#include "r300_grid_fold.h"
#include "r300_numeric_domain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The functional unit a route executes on.  R300-class silicon has no
 * compute engine: a kernel maps onto the host, the texture fetch, the FP24
 * fragment ALU, the RB3D blend, clear, or ROP output stages, the ZB stencil
 * unit, or the R2VB carrier, and the unit fixes what the route can promise.
 *
 * A typed unit lands with the route row that references it, so the
 * vocabulary never carries a value no row names.
 */
enum r300_execution_unit {
   /* Host execution; the CPU executor is the authority. */
   R300_EXECUTION_UNIT_HOST = 0,
   /* Bytes fetched through the TX unit and exported through the RB3D
    * color backend without arithmetic: a UNORM8 lane round-trips
    * exactly, so packed 32-bit patterns survive. */
   R300_EXECUTION_UNIT_TX_RB3D_COPY,
   /* The US fragment ALU (s1e7m16 FP24). */
   R300_EXECUTION_UNIT_US_FP24_ALU,
   /* The RB3D blend combiner (COMB_FCN ADD, MIN, MAX, SUB_CLAMP). */
   R300_EXECUTION_UNIT_RB3D_BLEND,
   /* The RB3D ROP logic op (RB3D_ROPCNTL bits 8-11 select one of sixteen
    * codes) on packed UNORM8 lanes. */
   R300_EXECUTION_UNIT_RB3D_ROP,
   /* The ZB stencil unit over an 8-bit stencil plane. */
   R300_EXECUTION_UNIT_ZB_STENCIL,
   /* The R2VB producer carrier: records through the VAP fetch, the US
    * varying datapath, and a C4_32_FP color export. */
   R300_EXECUTION_UNIT_R2VB_CARRIER,
   /* The RB3D color-buffer clear path, with no fragment-ALU arithmetic. */
   R300_EXECUTION_UNIT_RB3D_CLEAR,
   /* The RB2D solid-brush fill: DST_PITCH_OFFSET names a linear surface,
    * DP_GUI_MASTER_CNTL selects the solid brush and ROP3_P, and each
    * DST_WIDTH_HEIGHT write launches one rectangle.  No VAP, RS, US, RB3D,
    * or ZB state participates (r300_rb2d_fill.h). */
   R300_EXECUTION_UNIT_RB2D_FILL,
   R300_EXECUTION_UNIT_COUNT,
};

/* Stable route identities, append-only.  The name states unit and operation
 * so the identity survives every maturity change. */
enum r300_operation_route_id {
   R300_OPERATION_ROUTE_NONE = 0,

   R300_OPERATION_ROUTE_HOST_IDENTITY_MAP,
   R300_OPERATION_ROUTE_R2VB_IDENTITY_MAP,

   R300_OPERATION_ROUTE_RB3D_CLEAR_CONST_FILL,
   R300_OPERATION_ROUTE_US_FP24_UNARY_AFFINE,
   R300_OPERATION_ROUTE_US_FP24_BINARY_ARITHMETIC,
   R300_OPERATION_ROUTE_US_FP24_UNARY_TRANSCENDENTAL,
   R300_OPERATION_ROUTE_US_FP24_BINARY_TRANSCENDENTAL,
   R300_OPERATION_ROUTE_RB3D_ROP_BITWISE_LOGIC,
   R300_OPERATION_ROUTE_US_FP24_MULTITAP,
   R300_OPERATION_ROUTE_TX_RB3D_PREDICATED_STORE,
   R300_OPERATION_ROUTE_US_FP24_MULTIPASS_SCAN,
   R300_OPERATION_ROUTE_RB3D_BLEND_REDUCE,
   R300_OPERATION_ROUTE_RB3D_BLEND_SATURATING_DIFF,
   R300_OPERATION_ROUTE_US_FP24_PARALLEL_4OUT,
   R300_OPERATION_ROUTE_ZB_STENCIL_INVERT,

   R300_OPERATION_ROUTE_HOST_BITWISE_NOT,
   R300_OPERATION_ROUTE_RB3D_ROP_BITWISE_NOT,

   R300_OPERATION_ROUTE_RB2D_CONST_FILL,
   R300_OPERATION_ROUTE_HOST_TRANSFER_CONST_FILL,

   R300_OPERATION_ROUTE_COUNT,
};

/* Where a route runs.  Placement is independent of maturity: a GPU route
 * exists as a candidate long before it executes. */
enum r300_operation_route_executor {
   R300_OPERATION_ROUTE_EXECUTOR_HOST = 0,
   R300_OPERATION_ROUTE_EXECUTOR_GPU,
};

/* What a caller wants done, in terms no API owns.  An operation says what
 * is computed and a route says where; neither says whether the bytes are a
 * transfer destination, a bound render target, or a storage buffer a kernel
 * writes, and a unit that serves one of those serves the others only where
 * a row says so.  CONSTFILL shows the split: the RB2D solid brush writes a
 * linear transfer destination, the RB3D clear path writes a bound color
 * target, and a compute kernel writes a storage buffer through descriptor
 * offsets, so one operation reaches three routes that are not
 * interchangeable.
 *
 * The vocabulary is a mask because a route may serve several uses, and it
 * is append-only: a use lands with the row that names it, so the enum never
 * carries a value no route serves.  A request names exactly one use.
 */
enum r300_operation_route_use {
   /* A linear byte range in a buffer, named by offset and size, reached
    * through a transfer command. */
   R300_ROUTE_USE_TRANSFER_BUFFER = 1u << 0,
   /* A color target bound to the output stage, reached through a render
    * pass or an attachment clear. */
   R300_ROUTE_USE_RENDER_ATTACHMENT = 1u << 1,
   /* A storage buffer a compute kernel writes through its descriptor
    * binding, with the invocation index selecting the element. */
   R300_ROUTE_USE_COMPUTE_STORAGE_BUFFER = 1u << 2,
};

/* Every bit the vocabulary defines; the validator refuses a row outside it
 * so an unnamed bit never reaches the selector as an eligible use. */
#define R300_ROUTE_USE_ALL                                                    \
   (R300_ROUTE_USE_TRANSFER_BUFFER | R300_ROUTE_USE_RENDER_ATTACHMENT |       \
    R300_ROUTE_USE_COMPUTE_STORAGE_BUFFER)

/* How far a route has come.  CANDIDATE names a unit and an exactness bound
 * with no implementation or contract behind it; PRECOMMITTED carries both
 * plus an admission contract and opens by a later row movement with its
 * tests; EXECUTING runs today.  A route that does not exist has no row, so
 * absence is the missing row rather than a state. */
enum r300_operation_route_state {
   R300_OPERATION_ROUTE_CANDIDATE = 0,
   R300_OPERATION_ROUTE_PRECOMMITTED,
   R300_OPERATION_ROUTE_EXECUTING,
};

/* What a route may promise against the CPU oracle.  A BIT_EXACT route
 * compares byte for byte; an FP24 window route compares exactly inside the
 * s1e7m16 fixed-point window (integers to 2^17, r300_grid_fold.h) and
 * refuses outside it (-EDOM at admission); an FP24_BOUNDED route compares
 * within the declared tolerance, never tighter. */
enum r300_compute_verb_exactness {
   R300_COMPUTE_VERB_BIT_EXACT = 0,
   R300_COMPUTE_VERB_FP24_EXACT_WINDOW,
   R300_COMPUTE_VERB_FP24_BOUNDED,
};

/* How strong a route's evidence is, saying nothing about what the evidence
 * is about: the subject is the scope enum below, and the two read together
 * or not at all.  SILICON_RETAINED: a retained RS482 bundle holds a bit- or
 * tolerance-exact delivery (the Gallium-mediated lane's raster-verb corpus,
 * retired with that lane; the bundle identities ride the commit message and
 * the findings corpus).  SOURCE_GROUNDED: the unit and encoding follow from
 * the register programming guide and the ISA, with no retained delivery.
 * HOST: the CPU executor's own tests, which reach a host route alone. */
enum r300_compute_verb_evidence {
   R300_COMPUTE_VERB_EVIDENCE_HOST = 0,
   R300_COMPUTE_VERB_EVIDENCE_SOURCE_GROUNDED,
   R300_COMPUTE_VERB_EVIDENCE_SILICON_RETAINED,
};

/* The smallest claim to which a route's evidence applies.
 *
 * subject                 the claim the evidence reaches
 * HOST_EXECUTOR           the CPU executor ran it
 * UNIT_CONTRACT           the unit's encoding follows from the manuals
 * RASTER_CELL             one functional unit behaved so in a retained cell
 * NATIVE_GPU_ROUTE_CELL   this route itself delivered in a retained cell
 *
 * Only the last subject reaches a route, so an executing GPU route names it
 * and a candidate never does.  HOST_EXECUTOR belongs to a host route: the
 * validator refuses it on a GPU row, which keeps a GPU route with no
 * evidence from resolving to the CPU executor's tests by enum order. */
enum r300_compute_verb_evidence_scope {
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_HOST_EXECUTOR = 0,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_UNIT_CONTRACT,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_RASTER_CELL,
   R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL,
};

struct r300_operation_route_row {
   enum r300_operation_route_id route_id;
   const char *name;

   /* The catalog operation this route realizes; the join key every
    * consumer and every external ledger uses. */
   enum r300_operation_id operation_id;
   enum r300_operation_route_executor executor;
   enum r300_operation_route_state state;
   enum r300_execution_unit unit;

   /* The semantic uses this route serves, one or more bits of
    * enum r300_operation_route_use.  A selection names one use and reaches
    * only rows whose mask carries it, so a route qualified for one use
    * never answers for another. */
   uint32_t uses;

   /* The implementation, GPU route contract, and admission contract are a
    * set: all three are NONE on a candidate and all three are concrete on a
    * precommitted or executing GPU route.  A host route carries NONE for
    * all three; the GPU route contract stays GPU-specific rather than
    * acquiring an invented host identity for symmetry. */
   enum r300_operation_implementation_id implementation_id;
   enum r300_gpu_route_contract_id gpu_route_contract_id;
   enum r300_route_admission_id admission_id;

   /* How the route consumes the invocation index (r300_grid_fold.h): a GPU
    * route bounds its invocations by this class through the FP24
    * exact-index guards, never by the CPU ceiling. */
   enum r300_grid_index_class index_class;
   enum r300_compute_verb_exactness exactness;
   /* The declared per-component relative tolerance of an FP24_BOUNDED
    * route, zero for the exact classes. */
   float tolerance;

   enum r300_compute_verb_evidence evidence;
   enum r300_compute_verb_evidence_scope evidence_scope;

   /* The exact opt-in this route takes beside the compute gate; the value
    * that opens it is the literal "1".  A host route carries NULL: the host
    * path is the default and takes no opt-in. */
   const char *gate;
};

const struct r300_operation_route_row *
r300_operation_route_rows(uint32_t *count);

/* The row for a route id, or NULL for NONE, COUNT, and anything outside
 * the enum. */
const struct r300_operation_route_row *
r300_operation_route(enum r300_operation_route_id route_id);

/* How many routes an operation carries, across every use and maturity.  It
 * answers a reader asking how wide an operation's route set is and makes no
 * coverage claim, so it takes no use; a caller deciding whether an operation
 * runs asks r300_operation_has_executing_route(), which does. */
uint32_t
r300_operation_route_count_for_operation(enum r300_operation_id operation_id);

/* Whether an operation has an executing route on one executor that serves
 * one use.  The use is part of the question: a unit that fills a linear
 * transfer destination answers nothing about a kernel writing a storage
 * buffer, so a coverage claim that drops the use reports the first route to
 * execute as covering every caller of the same operation. */
bool
r300_operation_has_executing_route(enum r300_operation_id operation_id,
                                   enum r300_operation_route_executor executor,
                                   enum r300_operation_route_use use);

/* Select the route an operation takes on one executor for one use.
 *
 * gate_state is indexed by route id and holds, for each route, whether that
 * route's own gate stands open; passing NULL treats every gate as closed.
 * A route is eligible when it realizes the operation, serves the named use,
 * runs on the named executor, is EXECUTING, and either carries no gate or
 * has its own gate open.  A gate belongs to exactly one route, so opening
 * one never makes another eligible.
 *
 * use names exactly one bit of enum r300_operation_route_use.  Zero bits
 * and several bits both refuse: a caller performs one operation for one
 * purpose, and a mask standing in for a request would let the selector pick
 * across purposes.
 *
 * Selection fails closed: zero eligible routes and two or more eligible
 * routes both return NULL with *reason naming the refusal, because table
 * order is not a route policy and a second eligible route means the policy
 * that would choose between them has not been written.  A caller holding
 * such a policy enumerates with r300_operation_route_eligible() and names
 * its choice.
 */
const struct r300_operation_route_row *
r300_operation_select_route(enum r300_operation_id operation_id,
                            enum r300_operation_route_executor executor,
                            enum r300_operation_route_use use,
                            const bool *gate_state, const char **reason);

/* The _rows form takes a table so a test calibrates the selector on a
 * mutated copy; the table the ledger ships holds one executing route per
 * operation, executor, and use, so the two-eligible refusal is reachable
 * only from such a copy until a second executing route lands with its
 * policy. */
const struct r300_operation_route_row *
r300_operation_select_route_rows(const struct r300_operation_route_row *t,
                                 uint32_t count,
                                 enum r300_operation_id operation_id,
                                 enum r300_operation_route_executor executor,
                                 enum r300_operation_route_use use,
                                 const bool *gate_state, const char **reason);

/* Write the eligible routes into out[] and return how many there are,
 * writing at most max and returning the full count so a caller detects a
 * short buffer by a return above max.  Eligibility is the selector's, so
 * the two agree by construction: the selector is this enumeration plus the
 * rule that one route and only one route decides.  A caller that owns a
 * policy over several eligible routes reads them here.
 */
uint32_t
r300_operation_route_eligible(enum r300_operation_id operation_id,
                              enum r300_operation_route_executor executor,
                              enum r300_operation_route_use use,
                              const bool *gate_state,
                              const struct r300_operation_route_row **out,
                              uint32_t max);

/* The uses a route serves, spelled for diagnostics as a "|"-joined list of
 * lowercase names into buf, which is returned.  An empty mask spells
 * "none". */
char *r300_operation_route_use_names(uint32_t uses, char *buf, size_t size);

const char *r300_operation_route_executor_name(
   enum r300_operation_route_executor e);
const char *r300_operation_route_state_name(enum r300_operation_route_state s);
const char *r300_execution_unit_name(enum r300_execution_unit u);
const char *
r300_compute_verb_evidence_name(enum r300_compute_verb_evidence e);
const char *r300_compute_verb_evidence_scope_name(
   enum r300_compute_verb_evidence_scope s);
const char *
r300_compute_verb_exactness_name(enum r300_compute_verb_exactness e);

/* Well-formedness of a route table.  Returns true, or false with *reason
 * naming the first violated rule.  The ledger's own rows pass; a test
 * calibrates the checker on mutated copies. */
bool r300_operation_route_rows_valid(const struct r300_operation_route_row *t,
                                     uint32_t count, const char **reason);

#endif /* R300_OPERATION_ROUTE_H */
