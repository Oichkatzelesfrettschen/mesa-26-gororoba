/*
 * SPDX-License-Identifier: MIT
 *
 * Multi-factor arming gate for native DRM_RADEON_CS submission.
 */

#ifndef R3V_NATIVE_ARMING_H
#define R3V_NATIVE_ARMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The submission ioctl opens only when every factor holds at once, so a
 * single stale environment value, a different chip, a rebuilt IB, a
 * replaced kernel module, or a second attempt in the same evidence
 * directory all refuse.  The verdict names the first failing factor.
 */
enum r3v_native_arming_verdict {
   R3V_NATIVE_ARMING_ARMED = 0,
   R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED,
   R3V_NATIVE_ARMING_BUNDLE_UNDECLARED,
   R3V_NATIVE_ARMING_BUNDLE_MISMATCH,
   R3V_NATIVE_ARMING_UNKNOWN_CELL_KIND,
   R3V_NATIVE_ARMING_NONMAXIMUM_EXTENT,
   R3V_NATIVE_ARMING_CHIP_MISMATCH,
   R3V_NATIVE_ARMING_KERNEL_UNDECLARED,
   R3V_NATIVE_ARMING_KERNEL_MISMATCH,
   R3V_NATIVE_ARMING_MODULE_UNDECLARED,
   R3V_NATIVE_ARMING_MODULE_MISMATCH,
   R3V_NATIVE_ARMING_EVIDENCE_ABSENT,
   R3V_NATIVE_ARMING_ALREADY_ATTEMPTED,
   R3V_NATIVE_ARMING_SERIAL_BOUND_UNDECLARED,
   R3V_NATIVE_ARMING_SERIAL_BOUND_EXHAUSTED,
   R3V_NATIVE_ARMING_SERIAL_CONTINUITY_BROKEN,
   R3V_NATIVE_ARMING_BURST_DRAWS_UNDECLARED,
   R3V_NATIVE_ARMING_BURST_DRAWS_MISMATCH,
};

/* The recorded cell's kind.  Each kind freezes its own render geometry,
 * and the gate applies that kind's predicate: the triangle and
 * direct-write cells render the maximum public extent, while the
 * producer cell's target is the carrier row the reference layout
 * declares.  A kind outside this set carries no geometry contract and
 * refuses.  UNDECLARED is the zero value a fresh or reset command buffer
 * holds, so a stream installed without a kind cannot arm.
 */
enum r3v_native_cell_kind {
   R3V_NATIVE_CELL_KIND_UNDECLARED = 0,
   R3V_NATIVE_CELL_KIND_TRIANGLE,
   R3V_NATIVE_CELL_KIND_DIRECT_WRITE,
   R3V_NATIVE_CELL_KIND_R2VB_PRODUCER,
   R3V_NATIVE_CELL_KIND_R2VB_REINGEST,
   R3V_NATIVE_CELL_KIND_R2VB_FLOAT2_TUPLE,
   /* The serial status-load cell: the fetched FLOAT_2 tuple stream
    * submitted up to the declared serial bound while the paired-status
    * census samples, one outstanding submission at a time.
    */
   R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_SERIAL,
   /* The burst status-load cell: one submission whose IB carries the
    * tuple stream as a declared number of members, each retargeted to
    * its own carrier row, raising the GPU duty cycle of the single
    * ioctl the one-shot token admits.
    */
   R3V_NATIVE_CELL_KIND_R2VB_STATUS_LOAD_BURST,
};

/* Every fact the verdict rests on, collected before the decision so the
 * decision itself reads no environment and no filesystem.  A NULL string
 * is an undeclared or unreadable fact and refuses.
 */
struct r3v_native_arming_facts {
   /* Exact-value hazard gate; "1" opens, every other value stays closed. */
   const char *hazard_gate;
   /* Operator-declared IB digest and the digest of the IB about to be
    * submitted; they must agree, so a rebuilt or edited cell refuses.
    */
   const char *authorized_ib_blake3;
   const char *actual_ib_blake3;
   /* The recorded cell kind, naming which geometry contract the extent
    * fact was computed against.
    */
   enum r3v_native_cell_kind cell_kind;
   /* The recorded geometry differs from the cell kind's frozen geometry,
    * so the run has no attended-run identity.
    */
   bool nonmaximum_extent;
   /* The enumerated chip, checked against the authorized RS480-family
    * identity rather than any supported identity.
    */
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   /* Deployment identity: release string and radeon module srcversion. */
   const char *authorized_kernel_release;
   const char *running_kernel_release;
   const char *authorized_module_srcversion;
   const char *running_module_srcversion;
   /* One-shot evidence: the directory exists, and no attempt token from
    * an earlier arming sits in it.
    */
   bool evidence_dir_present;
   bool attempt_token_present;
   /* Serial authority: the exact-value declared submission bound
    * (R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS, decimal 1 through 64;
    * 0 is undeclared or malformed and refuses the serial kind), and the
    * count this device instance has already admitted.  The attempt token
    * still disarms the directory against every other process: a serial
    * continuation is admitted only when this instance wrote the token
    * itself, which serial_submissions_consumed being nonzero records.
    */
   uint32_t serial_authorized_submissions;
   uint32_t serial_submissions_consumed;
   /* Burst authority: the exact-value declared member count
    * (R3V_NATIVE_AUTHORIZED_BURST_DRAWS, decimal 1 through 64; 0 is
    * undeclared or malformed and refuses the burst kind), and the
    * member count the recorded cell actually composed, installed by the
    * queue like the extent fact.  The two agree or the gate refuses:
    * the operator authorizes a specific duty-cycle depth, never a
    * kind.
    */
   uint32_t burst_authorized_draws;
   uint32_t burst_recorded_draws;
};

#define R3V_NATIVE_ARMING_SERIAL_MAX_SUBMISSIONS 64u
#define R3V_NATIVE_ARMING_BURST_MAX_DRAWS 64u

/* The authorized attended-run chip: RS482, the only identity whose
 * silicon behavior the cell's falsifiers were written against.
 */
#define R3V_NATIVE_ARMING_PCI_VENDOR 0x1002u
#define R3V_NATIVE_ARMING_PCI_DEVICE 0x5974u

/* Pure decision over the collected facts. */
enum r3v_native_arming_verdict
r3v_native_arming_evaluate(const struct r3v_native_arming_facts *facts);

const char *
r3v_native_arming_verdict_name(enum r3v_native_arming_verdict verdict);

/* The seam every fact crosses on its way into the gate.  Collection reads the
 * environment, uname, sysfs, and the evidence directory; routing all four
 * through one interface is what lets the positive verdict be exercised
 * without the hardware, the kernel, or the operator's environment, and what
 * bounds the collector's side effects to the calls named here.
 */
struct r3v_native_arming_provider {
   const char *(*read_env)(void *ctx, const char *name);
   /* Writes the running kernel release, or leaves storage empty when it is
    * unreadable, which refuses.
    */
   void (*read_kernel_release)(void *ctx, char *out, size_t size);
   /* Writes the running radeon module srcversion, or leaves storage empty
    * when no module is loaded.
    */
   void (*read_module_srcversion)(void *ctx, char *out, size_t size);
   bool (*directory_present)(void *ctx, const char *path);
   bool (*file_present)(void *ctx, const char *path);
   void *ctx;
};

/* The provider the production path uses: getenv, uname, sysfs, stat. */
const struct r3v_native_arming_provider *r3v_native_arming_host_provider(void);

/* Collects through an explicit provider.  r3v_native_arming_collect is this
 * over the host provider.
 */
void r3v_native_arming_collect_from(
   const struct r3v_native_arming_provider *provider,
   struct r3v_native_arming_facts *facts, uint32_t pci_vendor_id,
   uint32_t pci_device_id, enum r3v_native_cell_kind cell_kind,
   const char *actual_ib_blake3, const char *evidence_dir,
   char *kernel_storage, size_t kernel_size, char *module_storage,
   size_t module_size);

/* Collects the live facts: environment values, the running kernel
 * release from uname, and the radeon module srcversion from sysfs.  The
 * caller supplies the chip identity, the IB digest, and the evidence
 * directory.  Returned string fields point at the caller-owned storage
 * passed in or at environment storage, so the facts live as long as the
 * call frame that built them.
 */
void r3v_native_arming_collect(struct r3v_native_arming_facts *facts,
                               uint32_t pci_vendor_id, uint32_t pci_device_id,
                               enum r3v_native_cell_kind cell_kind,
                               const char *actual_ib_blake3,
                               const char *evidence_dir,
                               char *kernel_storage, size_t kernel_size,
                               char *module_storage, size_t module_size);

/* Writes the one-shot attempt token into the evidence directory.  A run
 * that reaches the ioctl leaves the token behind, so a second arming in
 * the same directory refuses.  Returns 0 or a negative errno.
 */
int r3v_native_arming_disarm(const char *evidence_dir,
                             const char *declared_digest);

#endif /* R3V_NATIVE_ARMING_H */
