/*
 * SPDX-License-Identifier: MIT
 *
 * Ordered-submission plan for conformance runs over the native DRM
 * transport: schema, seal, identity binding, whole-entry matching, and
 * the monotonic replay session.
 */

#ifndef R3V_NATIVE_PLAN_H
#define R3V_NATIVE_PLAN_H

#include "r3v_native_arming.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A plan is the operator's authorization for one shard: an ordered list
 * of executable submissions the no-submit planning pass recorded, sealed
 * with the identities the live replay must present.  The live path admits
 * the next submission only when its complete entry equals the plan's
 * next entry, consumes entries monotonically, and latches terminally on
 * the first deviation, so an omitted, extra, reordered, enlarged, or
 * mutated submission stops the session at the ioctl boundary and stays
 * stopped.  One-shot attended cells keep their own gate; a plan session
 * and a one-shot authorization never coexist on one device.
 */

#define R3V_NATIVE_PLAN_SCHEMA_VERSION 1u
#define R3V_NATIVE_PLAN_HEX64 64u
#define R3V_NATIVE_PLAN_NAME_MAX 63u
#define R3V_NATIVE_PLAN_PATH_MAX 255u
#define R3V_NATIVE_PLAN_RELOC_MAX 64u
#define R3V_NATIVE_PLAN_SUBMISSION_MAX 65536u
/* Schema ceilings on the plan's own ceilings: an IB the radeon CS ioctl
 * accepts in one chunk, one day of runtime, and one terabyte of
 * cumulative traffic bound every plan whatever it declares.
 */
#define R3V_NATIVE_PLAN_IB_DWORDS_MAX (1u << 20)
#define R3V_NATIVE_PLAN_RUNTIME_SECONDS_MAX 86400u
#define R3V_NATIVE_PLAN_CUMULATIVE_BYTES_MAX (UINT64_C(1) << 40)
/* RADEON_GEM_DOMAIN_CPU | RADEON_GEM_DOMAIN_GTT | RADEON_GEM_DOMAIN_VRAM:
 * the only domain bits a relocation can carry.
 */
#define R3V_NATIVE_PLAN_DOMAIN_MASK 0x7u

enum r3v_native_plan_queue_claim {
   R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY = 0,
   R3V_NATIVE_PLAN_QUEUE_EXPERIMENTAL_COMPUTE_SUBSET,
   R3V_NATIVE_PLAN_QUEUE_CONFORMANT,
};

enum r3v_native_plan_direction {
   R3V_NATIVE_PLAN_DIRECTION_READ = 1,
   R3V_NATIVE_PLAN_DIRECTION_WRITE = 2,
   R3V_NATIVE_PLAN_DIRECTION_READ_WRITE = 3,
};

struct r3v_native_plan_reloc {
   char role[R3V_NATIVE_PLAN_NAME_MAX + 1];
   uint32_t read_domains;
   uint32_t write_domain;
   uint64_t size;
   enum r3v_native_plan_direction direction;
};

struct r3v_native_plan_submission {
   char ib_blake3[R3V_NATIVE_PLAN_HEX64 + 1];
   uint32_t ib_dwords;
   enum r3v_native_cell_kind cell_kind;
   char emitter[R3V_NATIVE_PLAN_NAME_MAX + 1];
   uint32_t reloc_count;
   struct r3v_native_plan_reloc relocs[R3V_NATIVE_PLAN_RELOC_MAX];
};

struct r3v_native_plan {
   uint32_t schema_version;
   char source_sha[41];
   bool source_clean;
   char dso_blake3[R3V_NATIVE_PLAN_HEX64 + 1];
   char deqp_sha256[R3V_NATIVE_PLAN_HEX64 + 1];
   char deqp_release[R3V_NATIVE_PLAN_NAME_MAX + 1];
   char partition_sha256[R3V_NATIVE_PLAN_HEX64 + 1];
   char caselist_sha256[R3V_NATIVE_PLAN_HEX64 + 1];
   enum r3v_native_plan_queue_claim queue_claim;
   char kernel_release[R3V_NATIVE_PLAN_NAME_MAX + 1];
   char module_srcversion[R3V_NATIVE_PLAN_NAME_MAX + 1];
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   char nonce[33];
   char evidence_dir[R3V_NATIVE_PLAN_PATH_MAX + 1];
   uint32_t max_ib_dwords;
   uint32_t max_relocs;
   /* Cumulative traffic ceiling: every admitted submission adds the size
    * of every relocation it carries, the completion object included, so
    * a shard re-referencing one target N times spends N times its size.
    */
   uint64_t max_cumulative_bytes;
   uint32_t max_submissions;
   uint32_t max_runtime_seconds;
   uint32_t submission_count;
   struct r3v_native_plan_submission *submissions;
   char seal[R3V_NATIVE_PLAN_HEX64 + 1];
};

/* Parse refusals name the first defect in the text; a plan that parses is
 * self-consistent: every declared field present, the seal equal to the
 * BLAKE3 of every byte before it, submission indices 0..N-1 in order, N
 * equal to the declared count, no submission above a ceiling, every
 * ceiling inside the schema ceilings, and the text byte-identical to
 * the writer's canonical form, so one authorization has one digest.
 */
enum r3v_native_plan_parse_result {
   R3V_NATIVE_PLAN_PARSE_OK = 0,
   R3V_NATIVE_PLAN_PARSE_NOT_A_PLAN,
   R3V_NATIVE_PLAN_PARSE_SCHEMA_VERSION,
   R3V_NATIVE_PLAN_PARSE_MALFORMED_LINE,
   R3V_NATIVE_PLAN_PARSE_DUPLICATE_FIELD,
   R3V_NATIVE_PLAN_PARSE_MISSING_FIELD,
   R3V_NATIVE_PLAN_PARSE_BAD_VALUE,
   R3V_NATIVE_PLAN_PARSE_SUBMISSION_ORDER,
   R3V_NATIVE_PLAN_PARSE_SUBMISSION_COUNT,
   R3V_NATIVE_PLAN_PARSE_RELOC_COUNT,
   R3V_NATIVE_PLAN_PARSE_CEILING_EXCEEDED,
   R3V_NATIVE_PLAN_PARSE_SEAL_MISSING,
   R3V_NATIVE_PLAN_PARSE_SEAL_MISMATCH,
   R3V_NATIVE_PLAN_PARSE_NONCANONICAL,
   R3V_NATIVE_PLAN_PARSE_OUT_OF_MEMORY,
};

enum r3v_native_plan_parse_result
r3v_native_plan_parse(const char *text, size_t size,
                      struct r3v_native_plan *plan);

void r3v_native_plan_finish(struct r3v_native_plan *plan);

/* Seals a serialized body in place: writes "seal\t<blake3>\n" at
 * text[body_size] and returns the total byte count, or 0 when the buffer
 * cannot hold the seal line.  The writer uses it; a planning pass that
 * assembles the body incrementally seals with it too.
 */
size_t r3v_native_plan_seal(char *text, size_t text_size, size_t body_size);

/* Serializes a plan (seal computed and written last) after holding every
 * field to the parser's predicates, so a plan the writer emits is a plan
 * the parser admits.  Returns the byte count written, or the count needed
 * when out is too small (out untouched), or -1 for a plan outside the
 * schema.
 */
long r3v_native_plan_write(const struct r3v_native_plan *plan, char *out,
                           size_t out_size);

/* The running identity a live replay presents; a NULL string is
 * undeclared and refuses.  gates_open is r3v_native_plan_gates_open over
 * the live environment: a plan run reads its authority from the plan
 * alone, so any open submission or experimental-route gate is
 * contamination.  evidence_dir_present and evidence_dir_empty come from
 * the caller's filesystem read of the plan's evidence directory.
 */
struct r3v_native_plan_identity {
   const char *source_sha;
   bool source_clean;
   const char *dso_blake3;
   const char *deqp_sha256;
   const char *deqp_release;
   const char *partition_sha256;
   const char *caselist_sha256;
   enum r3v_native_plan_queue_claim queue_claim;
   const char *kernel_release;
   const char *module_srcversion;
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   const char *nonce;
   bool evidence_dir_present;
   bool evidence_dir_empty;
   bool gates_open;
};

enum r3v_native_plan_bind_result {
   R3V_NATIVE_PLAN_BIND_OK = 0,
   R3V_NATIVE_PLAN_BIND_SOURCE,
   R3V_NATIVE_PLAN_BIND_SOURCE_DIRTY,
   R3V_NATIVE_PLAN_BIND_DSO,
   R3V_NATIVE_PLAN_BIND_DEQP,
   R3V_NATIVE_PLAN_BIND_DEQP_RELEASE,
   R3V_NATIVE_PLAN_BIND_PARTITION,
   R3V_NATIVE_PLAN_BIND_CASELIST,
   R3V_NATIVE_PLAN_BIND_QUEUE_CLAIM,
   R3V_NATIVE_PLAN_BIND_KERNEL,
   R3V_NATIVE_PLAN_BIND_MODULE,
   R3V_NATIVE_PLAN_BIND_PCI,
   R3V_NATIVE_PLAN_BIND_NONCE,
   R3V_NATIVE_PLAN_BIND_EVIDENCE_DIR,
   R3V_NATIVE_PLAN_BIND_GATE_CONTAMINATION,
};

enum r3v_native_plan_bind_result
r3v_native_plan_bind(const struct r3v_native_plan *plan,
                     const struct r3v_native_plan_identity *identity);

/* The one enumeration of every gate a plan run refuses: the hazard gate,
 * the operator authorization values, the R2VB route gates, the compute
 * queue gate, and every compute verb's GPU gate.  Returns true when any
 * of them carries a non-empty value in the environment read_env
 * presents, and names it through gate_out.
 */
bool r3v_native_plan_gates_open(const char *(*read_env)(void *ctx,
                                                        const char *name),
                                void *ctx, const char **gate_out);

/* Whole-entry comparison of a live submission against a plan entry. */
enum r3v_native_plan_match_result {
   R3V_NATIVE_PLAN_MATCH_OK = 0,
   R3V_NATIVE_PLAN_MATCH_DIGEST,
   R3V_NATIVE_PLAN_MATCH_DWORDS,
   R3V_NATIVE_PLAN_MATCH_CELL_KIND,
   R3V_NATIVE_PLAN_MATCH_EMITTER,
   R3V_NATIVE_PLAN_MATCH_RELOC_COUNT,
   R3V_NATIVE_PLAN_MATCH_RELOC_ROLE,
   R3V_NATIVE_PLAN_MATCH_RELOC_DOMAINS,
   R3V_NATIVE_PLAN_MATCH_RELOC_SIZE,
   R3V_NATIVE_PLAN_MATCH_RELOC_DIRECTION,
};

enum r3v_native_plan_match_result
r3v_native_plan_match(const struct r3v_native_plan_submission *expected,
                      const struct r3v_native_plan_submission *actual);

/* The replay session: bound once, consumed monotonically, terminal on
 * the first failure.  Every refusal after the terminal latch reports
 * TERMINAL, so a caller that ignored one refusal cannot proceed on the
 * next.  The session records elapsed seconds and referenced bytes to
 * hold the plan's runtime and byte ceilings.
 */
enum r3v_native_plan_session_result {
   R3V_NATIVE_PLAN_SESSION_ADMITTED = 0,
   R3V_NATIVE_PLAN_SESSION_UNBOUND,
   R3V_NATIVE_PLAN_SESSION_TERMINAL,
   R3V_NATIVE_PLAN_SESSION_EXHAUSTED,
   R3V_NATIVE_PLAN_SESSION_MISMATCH,
   /* The submit carried a number of executable command buffers other
    * than one: two or more, or zero.  A fence-only submit reaches the
    * session only when the caller admits it; the device consults the
    * session at the IB handoff, which a zero-buffer submit never reaches.
    */
   R3V_NATIVE_PLAN_SESSION_EXECUTABLE_COUNT,
   R3V_NATIVE_PLAN_SESSION_RUNTIME_EXCEEDED,
   R3V_NATIVE_PLAN_SESSION_BYTES_EXCEEDED,
   R3V_NATIVE_PLAN_SESSION_INCOMPLETE,
   R3V_NATIVE_PLAN_SESSION_CONSUMED,
};

struct r3v_native_plan_session {
   const struct r3v_native_plan *plan;
   bool bound;
   bool terminal;
   bool completed;
   uint32_t next_index;
   uint64_t referenced_bytes;
   enum r3v_native_plan_session_result terminal_reason;
   enum r3v_native_plan_match_result last_mismatch;
};

/* A session starts zeroed (r3v_native_plan_session_init); bind attaches
 * a plan whose identity check passed, and a session bound once refuses a
 * second bind (CONSUMED), since a plan authorizes one session.
 */
void r3v_native_plan_session_init(struct r3v_native_plan_session *session);

enum r3v_native_plan_session_result
r3v_native_plan_session_bind(struct r3v_native_plan_session *session,
                             const struct r3v_native_plan *plan);

/* Admits the next live submission when executable_count is 1, the
 * session is live, entries remain, the whole entry matches, and the
 * runtime and byte ceilings hold; every other outcome latches terminal.
 * On admission the entry is consumed before the caller proceeds.
 * elapsed_seconds is the caller's monotonic reading since bind; the
 * device supplies it from CLOCK_MONOTONIC.
 */
enum r3v_native_plan_session_result
r3v_native_plan_session_admit(struct r3v_native_plan_session *session,
                              const struct r3v_native_plan_submission *actual,
                              uint32_t executable_count,
                              uint64_t elapsed_seconds);

/* Latches the session terminal on a transport, completion, or evidence
 * failure the caller observed after admission.
 */
void r3v_native_plan_session_fail(struct r3v_native_plan_session *session,
                                  enum r3v_native_plan_session_result why);

/* Proves plan exhaustion after the last expected submission: every entry
 * consumed and no failure latched.  An incomplete session is INCOMPLETE
 * and latches terminal, so a shard that stopped short cannot be resumed
 * or reported complete.
 */
enum r3v_native_plan_session_result
r3v_native_plan_session_finish(struct r3v_native_plan_session *session);

const char *r3v_native_plan_parse_result_name(
   enum r3v_native_plan_parse_result r);
const char *r3v_native_plan_bind_result_name(
   enum r3v_native_plan_bind_result r);
const char *r3v_native_plan_match_result_name(
   enum r3v_native_plan_match_result r);
const char *r3v_native_plan_session_result_name(
   enum r3v_native_plan_session_result r);
const char *r3v_native_plan_queue_claim_name(
   enum r3v_native_plan_queue_claim c);
bool r3v_native_plan_queue_claim_parse(const char *name,
                                       enum r3v_native_plan_queue_claim *out);
const char *r3v_native_plan_cell_kind_name(enum r3v_native_cell_kind k);
bool r3v_native_plan_cell_kind_parse(const char *name,
                                     enum r3v_native_cell_kind *out);

#endif /* R3V_NATIVE_PLAN_H */
