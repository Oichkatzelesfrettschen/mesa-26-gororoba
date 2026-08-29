/*
 * SPDX-License-Identifier: MIT
 *
 * Multi-factor arming gate for native DRM_RADEON_CS submission.
 */

#ifndef R3V_NATIVE_ARMING_H
#define R3V_NATIVE_ARMING_H

#include "amd/r300/common/r300_chip_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Collection and disarm share one bounded path capacity so token probes and
 * exclusive token creation address identical names. */
#define R3V_NATIVE_ARMING_PATH_MAX 1024u

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
   /* The public GPU-producer route: the producer pass composed over the
    * application's admitted records ahead of the recorded consumer
    * triangle in one IB, so the device writes the carrier the consumer
    * fetches.  The geometry contract is the consumer's maximum public
    * extent plus the carrier's read-write GTT relocation.
    */
   R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC,
   /* The depth control cell: two triangles at different window depths
    * against a host-filled Z16 surface, so the color output and the
    * depth memory both follow the depth test.  The geometry contract is
    * the cell's fixed 64x64 target plus the three-slot reference layout
    * with the depth surface's read-write GTT relocation.
    */
   R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL,
   /* The fetched GPU-producer route: the fetched producer composed ahead
    * of the recorded consumer triangle, the producer reading the
    * application's vertex BO and a driver-owned slot BO through the
    * two-array fetched body.  The geometry contract is the consumer's
    * maximum public extent plus four relocations: the carrier read-write,
    * the color target written, the slot and source arrays read.
    */
   R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED,
   /* The compute identity carrier: the fetched producer pass alone, the
    * input storage buffer as its source array and the output storage
    * buffer as its carrier row, under the compute gate and the identity
    * verb's own gate.  The geometry contract is three relocations: the
    * output written, the slot and input arrays read. */
   R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER,
   /* The render-shape triangle: the qualified cell over a declared
    * extent, pitch, lane order, and fragment constant
    * (r300_triangle_render_shape).  The digest binds every parameter,
    * so the geometry contract is the declared shape itself, and the
    * recorder refuses a shape the family's admission refuses.
    */
   R3V_NATIVE_CELL_KIND_TRIANGLE_RENDER_SHAPE,
   /* The sampled triangle: the varying cell fetching TX unit 0 over the
    * declared linear texture, three relocations -- vertex and texture
    * read, color target written.  The geometry contract is the render
    * family's extent plus the texture's own declared geometry.
    */
   R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED,
   /* The composed render-then-sample triangle: one stream renders a
    * target, publishes it through the destination-cache flush, then
    * samples it into a second target.  Five relocations -- two vertex
    * reads, two color writes, and the first target read again as the
    * texture -- so the submission binds one buffer object under both a
    * write and a read use site.
    */
   R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE,
   /* The multisample resolve triangle: one stream renders into a
    * sample-expanded surface with GB_AA_CONFIG's subsample set live,
    * then covers the extent again under
    * RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE so the downsampled
    * samples reach RB3D_AARESOLVE_OFFSET.  Five relocations -- two
    * vertex reads, the multisample surface written by both halves, and
    * the resolve destination -- over four buffer objects, the
    * multisample surface device-local and never host-mapped.
    */
   R3V_NATIVE_CELL_KIND_TRIANGLE_MSAA_RESOLVE,
   /* Several recorded render passes concatenated into one indirect
    * buffer, each with its own first-draw contract, cell, and merged
    * relocation entries.  The concatenation is the recording's own, so
    * no offline emitter reproduces its digest and the geometry
    * predicate reports the kind unfrozen: the stream executes on the
    * closed-gate path and an armed submission refuses before any ioctl.
    */
   R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS,
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
#define R3V_NATIVE_ARMING_PCI_VENDOR ((uint32_t)R300_PCI_VENDOR_ATI)
#define R3V_NATIVE_ARMING_PCI_DEVICE ((uint32_t)R300_PCI_DEVICE_RS482)

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
