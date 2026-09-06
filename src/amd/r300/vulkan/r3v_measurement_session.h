/*
 * SPDX-License-Identifier: MIT
 *
 * The bounded measurement session: a declaration that authorizes a finite
 * number of repetitions of a finite set of fills, bound to the actual
 * allocations of one live device.
 *
 * The one-shot authorization beside it is unchanged and stays the
 * ordinary path.  It names one submission by a digest over eighteen
 * fields, among them the destination's GEM handle, and its evidence
 * directory disarms after one attempt, so it admits exactly one
 * execution.  A GEM handle is local to the DRM file that created it, so
 * no operator can declare that digest before the allocation exists, and
 * a benchmark that needs several hundred identical deliveries cannot be
 * expressed as several hundred one-shot declarations.
 *
 * The session resolves that in two levels.  The declaration is written
 * before any device exists and names the platform, the deployment epoch,
 * the route, the destination's role, the exact cases, and the budgets; it is read and hashed once at vkCreateDevice, beside the route
 * gates, so nothing the environment does later moves a decision under a
 * recorded command buffer.  The binding happens inside the device: the
 * first authorized preparation of each case resolves the destination
 * through the recorded VkBuffer and its bound memory, holds that
 * resolution to the declared role, and records the buffer object's
 * handle, its allocation generation, and the concrete fill identity the
 * ordinary digest covers.  Every later repetition of that case recomputes
 * the identity and requires it to equal the bound one, so the operator
 * declares the resource and the operation while the driver records the
 * handle -- and a self-computed digest never authorizes an operation the
 * declaration does not name.
 *
 * Identity and repetition are separate predicates.  Two submissions that
 * agree in every field are still two executions, so the session carries
 * its own budget: one permitted execution is consumed immediately before
 * the kernel submission boundary and never refunded, an error after that
 * boundary spends its attempt and closes the session, and a closed
 * session refuses every further request.
 *
 * Two facts have two lifetimes, and the difference decides what a crash
 * leaves behind.  That a session started is durable: the first admission
 * writes the evidence directory's attempt token through
 * r3v_native_arming_disarm, fsynced file and directory, so a second
 * process against that directory finds the token standing and refuses as
 * already attempted.  How much a session spent is process-local: the
 * counters below live in the device and die with it.  A run that stops at
 * forty of four hundred and one that stops at three hundred and
 * ninety-nine leave the same durable object, so the token bounds
 * restarting rather than accounting, and a campaign's own spend is read
 * out of its published samples.
 *
 * The handle is not enough on its own.  A GEM handle is an index into one
 * DRM file's table and is recycled after the object it named is
 * destroyed, so the binding carries the allocation's own generation
 * beside it and a recycled number over a different object refuses.
 */

#ifndef R3V_MEASUREMENT_SESSION_H
#define R3V_MEASUREMENT_SESSION_H

#include "r3v_fill_route.h"

#include "amd/r300/common/r300_chip_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The declaration schema this file reads.  A manifest naming any other
 * value is a declaration for a different reader. */
#define R3V_MEASUREMENT_SESSION_SCHEMA "r3v-measurement-session-v1"

/* The stored termination reason, truncated at this width. */
#define R3V_MEASUREMENT_SESSION_REASON_SIZE 128u

#define R3V_MEASUREMENT_SESSION_MAX_CASES 32u
/* One arm's whole campaign: ten declared sizes at warmups plus
 * repetitions apiece, with room to spare.  A declaration above this bound
 * is refused rather than truncated. */
#define R3V_MEASUREMENT_SESSION_MAX_SUBMISSIONS 4096u
/* The ceiling on a declared completion wait.  The wait interface reads an
 * absolute deadline at or above INT64_MAX as unbounded, so a declared
 * relative interval stays far below it and no campaign declares a wait
 * that never returns. */
#define R3V_MEASUREMENT_SESSION_MAX_TIMEOUT_NS 300000000000ull
#define R3V_MEASUREMENT_SESSION_TEXT_MAX 65536u
#define R3V_MEASUREMENT_SESSION_NAME_MAX 64u
#define R3V_MEASUREMENT_SESSION_EPOCH_MAX 128u

/* Every `const char *` this interface takes is a NUL-terminated string,
 * read with strcmp or strlen and never past its terminator.  The digests
 * below are the exception the struct exists for: they are scanned to a
 * fixed width rather than to a terminator, so the width has to be part
 * of the type rather than a promise the caller makes.
 */

/* A digest as an object rather than a pointer.  An array parameter
 * decays to a pointer, so the width a scan walks would be a promise the
 * caller makes rather than a fact the type carries, and a shorter object
 * would be read past its end.  A struct carries its size into the type,
 * so the compiler rejects a caller that passes anything else and a NULL
 * still refuses at runtime. */
struct r3v_measurement_digest {
   char hex[R3V_FILL_ROUTE_DIGEST_HEX_SIZE];
};

/* Every rule the session holds a request to, in the order the checks
 * test them, so a refusal names one fact. */
enum r3v_measurement_session_refusal {
   R3V_MEASUREMENT_SESSION_ADMITTED = 0,
   /* No session is declared, so the ordinary one-shot path decides. */
   R3V_MEASUREMENT_SESSION_REFUSE_INACTIVE,
   R3V_MEASUREMENT_SESSION_REFUSE_MANIFEST_MALFORMED,
   R3V_MEASUREMENT_SESSION_REFUSE_SCHEMA,
   /* The declaration names another platform or another deployment. */
   R3V_MEASUREMENT_SESSION_REFUSE_EPOCH,
   /* The device resolved another executor for the request. */
   R3V_MEASUREMENT_SESSION_REFUSE_ROUTE_MISMATCH,
   /* The request's range, offset, or value matches no declared case. */
   R3V_MEASUREMENT_SESSION_REFUSE_CASE_UNDECLARED,
   /* The destination resolves outside the declared role. */
   R3V_MEASUREMENT_SESSION_REFUSE_ROLE_MISMATCH,
   /* The case is bound and this request resolves to another object. */
   R3V_MEASUREMENT_SESSION_REFUSE_DESTINATION_REBOUND,
   /* The recomputed submission identity differs from the bound one. */
   R3V_MEASUREMENT_SESSION_REFUSE_IDENTITY_MISMATCH,
   /* The case or the session has spent its declared executions. */
   R3V_MEASUREMENT_SESSION_REFUSE_BUDGET_EXHAUSTED,
   /* A live session already stands over a declaration. */
   R3V_MEASUREMENT_SESSION_REFUSE_ALREADY_OPEN,
   /* The session terminated and admits nothing further. */
   R3V_MEASUREMENT_SESSION_REFUSE_CLOSED,
   R3V_MEASUREMENT_SESSION_REFUSAL_COUNT,
};

const char *r3v_measurement_session_refusal_name(
   enum r3v_measurement_session_refusal r);

/* One declared case: the exact fill this session admits, and how many
 * times.  Warmups and repetitions are counted separately in the report
 * and identically against the budget, because both enter the kernel. */
struct r3v_measurement_case {
   uint32_t case_id;
   uint64_t fill_offset;
   uint64_t fill_bytes;
   uint32_t fill_value;
   uint32_t warmups;
   uint32_t repetitions;
};

/* The destination's declared role.  It names properties an allocation
 * has, never a handle: a handle is local to a DRM file that does not
 * exist when the declaration is written. */
struct r3v_measurement_role {
   uint64_t allocation_bytes;
   uint64_t buffer_bytes;
   uint64_t binding_offset;
   uint32_t memory_property_flags;
   uint32_t buffer_usage;
   uint32_t write_domain;
};

/* The declaration as read from its file, plus the digest over the whole
 * text.  A field the parser cannot read refuses the manifest rather than
 * defaulting. */
struct r3v_measurement_manifest {
   char schema[R3V_MEASUREMENT_SESSION_NAME_MAX];
   char session_nonce[R3V_MEASUREMENT_SESSION_NAME_MAX];
   char platform[R3V_MEASUREMENT_SESSION_NAME_MAX];
   /* The route the campaign measures, held against the route the device
    * resolved for the request: a declaration written for one executor
    * authorizes no submission another performs. */
   char route[R3V_MEASUREMENT_SESSION_NAME_MAX];
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   char kernel_release[R3V_MEASUREMENT_SESSION_EPOCH_MAX];
   char module_srcversion[R3V_MEASUREMENT_SESSION_EPOCH_MAX];
   struct r3v_measurement_role role;
   uint32_t case_count;
   struct r3v_measurement_case cases[R3V_MEASUREMENT_SESSION_MAX_CASES];
   uint32_t max_total_submissions;
   uint64_t completion_timeout_ns;
};

/* What one case's first authorized preparation recorded.  handle and
 * generation together name one object over the device's lifetime;
 * identity is the ordinary fill digest computed against that object. */
struct r3v_measurement_binding {
   bool bound;
   uint32_t destination_handle;
   uint64_t memory_generation;
   struct r3v_measurement_digest identity;
   uint32_t executions_consumed;
};

struct r3v_measurement_session {
   bool active;
   bool closed;
   /* Why the session terminated, for the refusal every later request
    * carries.  The characters are copied in, so a caller's stack buffer
    * names a reason that outlives the frame it was formatted in. */
   char closed_reason[R3V_MEASUREMENT_SESSION_REASON_SIZE];
   struct r3v_measurement_manifest manifest;
   struct r3v_measurement_digest manifest_digest;
   struct r3v_measurement_binding bindings[R3V_MEASUREMENT_SESSION_MAX_CASES];
   /* Claimed once before any sample and decremented at the kernel
    * boundary; an attempt that entered the ioctl is never refunded. */
   uint32_t remaining_submissions;
   uint32_t consumed_submissions;
};

/* Reads a declaration out of its text.  Returns ADMITTED with *out
 * filled, or a refusal with *reason naming the first line or field it
 * cannot read.  The text is key = value, one per line, with # comments;
 * a `case` line carries id, offset, bytes, value, warmups, repetitions
 * separated by commas.  Every field the manifest declares is required:
 * an absent one refuses rather than defaulting.
 */
enum r3v_measurement_session_refusal
r3v_measurement_manifest_parse(const char *text, size_t length,
                               struct r3v_measurement_manifest *out,
                               const char **reason);

/* Holds a declaration to the deployment it runs on: the PCI pair, the
 * running kernel release, and the loaded module srcversion.  A stream
 * authorized against one radeon build is not authorized against
 * another. */
enum r3v_measurement_session_refusal
r3v_measurement_manifest_epoch_check(
   const struct r3v_measurement_manifest *manifest, uint32_t pci_vendor_id,
   uint32_t pci_device_id, const char *kernel_release,
   const char *module_srcversion, const char **reason);

/* Brings a session to the state open reads: inactive, unclosed, with no
 * binding and no allowance.  Every session passes through this before its
 * first open, because open decides a reopen by reading the struct the
 * caller supplied and an uninitialized read decides nothing. */
void r3v_measurement_session_init(struct r3v_measurement_session *session);

/* Holds the declaration's platform name to the board the device resolved.
 * The epoch check compares the PCI pair, the kernel release, and the
 * module srcversion; a PCI id names a die class shared by boards whose
 * memory, thermal, and recovery behavior a campaign never qualified, so
 * the declared platform is resolved to its stable id and required to
 * equal the running one.  A name no platform row carries resolves to
 * R300_PLATFORM_ID_NONE and refuses, as does a running board that
 * resolved to none.
 */
enum r3v_measurement_session_refusal
r3v_measurement_manifest_platform_check(
   const struct r3v_measurement_manifest *manifest,
   enum r300_platform_id resolved, const char **reason);

/* Opens a session over a parsed declaration, which
 * r3v_measurement_session_init has brought to a readable state.
 *
 * The budget is the sum every case's warmups and repetitions account
 * for, so a submission the budget admits is one some case names.  A
 * declared total below that sum cannot run the campaign it declares and
 * refuses here rather than exhausting partway through. */
enum r3v_measurement_session_refusal
r3v_measurement_session_open(
   struct r3v_measurement_session *session,
   const struct r3v_measurement_manifest *manifest,
   const struct r3v_measurement_digest *manifest_digest,
   const char **reason);

/* The declared case one request names, or NULL.  A request matches on
 * offset, size, and value together: a case is the exact fill, not a
 * range it falls inside. */
const struct r3v_measurement_case *
r3v_measurement_session_find_case(const struct r3v_measurement_session *session,
                                  uint64_t fill_offset, uint64_t fill_bytes,
                                  uint32_t fill_value, uint32_t *index_out);

/* Holds the executor the device resolved to the one the declaration
 * measures.  A campaign written for the windowed route authorizes no
 * submission the frozen route performs, whatever else agrees. */
enum r3v_measurement_session_refusal
r3v_measurement_session_route_check(
   const struct r3v_measurement_session *session, const char *route_name,
   const char **reason);

/* Holds an observed destination to the declared role. */
enum r3v_measurement_session_refusal
r3v_measurement_session_role_check(
   const struct r3v_measurement_session *session,
   const struct r3v_measurement_role *observed, const char **reason);

/* Binds a case to the object this request resolved to, or holds it to
 * the object it already bound.  The first call for a case records the
 * handle, the generation, and the identity; every later call requires
 * all three to match.  It reserves nothing: the execution is consumed
 * separately, at the kernel boundary.
 *
 * The offset, size, and value are the ones the recorded operation
 * carries, and they are held against the case the index names.  The
 * index alone authorizes nothing: a request that names one case and
 * fills another refuses as undeclared.
 */
enum r3v_measurement_session_refusal
r3v_measurement_session_bind(struct r3v_measurement_session *session,
                             uint32_t case_index, uint64_t fill_offset,
                             uint64_t fill_bytes, uint32_t fill_value,
                             uint32_t destination_handle,
                             uint64_t memory_generation,
                             const struct r3v_measurement_digest *identity,
                             const char **reason);

/* Consumes one permitted execution.  Called immediately before the
 * kernel submission boundary, so an attempt that entered the ioctl is
 * counted whatever the ioctl returns.
 *
 * It names the whole submission again -- the case, the operation, the
 * object, and the identity -- and holds every field against the binding,
 * so the execution spent is the execution the bind authorized.  A bind
 * and a consume that described different submissions would let one
 * allocation's authorization pay for another's delivery, and the
 * binding alone cannot see that substitution.  A consumed submission
 * naming another object or another stream terminates the campaign. */
enum r3v_measurement_session_refusal
r3v_measurement_session_consume(
   struct r3v_measurement_session *session, uint32_t case_index,
   uint64_t fill_offset, uint64_t fill_bytes, uint32_t fill_value,
   uint32_t destination_handle, uint64_t memory_generation,
   const struct r3v_measurement_digest *identity, const char **reason);

/* Terminates the session.  Every later request refuses as closed and
 * names this reason.  Closing a closed session keeps the first reason:
 * the first failure is the one that ended the run.
 *
 * The characters are copied into the session and truncated at
 * R3V_MEASUREMENT_SESSION_REASON_SIZE, so a formatted stack buffer is a
 * valid reason and no storage obligation reaches the caller. */
void r3v_measurement_session_close(struct r3v_measurement_session *session,
                                   const char *why);

#endif /* R3V_MEASUREMENT_SESSION_H */
