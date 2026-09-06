/*
 * SPDX-License-Identifier: MIT
 *
 * The durable claim over one declared measurement arm.
 *
 * The session's counters are process-local: they live in the device and
 * die with it, so a declaration opened on two devices carries two
 * allowances and a restarted process starts over.  The claim is the
 * durable half.  It is one directory entry created with O_EXCL, so the
 * kernel picks exactly one winner among every device and every process
 * racing for the same arm, and a later run finds it standing.
 *
 * The entry lives in the campaign root -- the directory holding the
 * declaration -- rather than in the evidence output directory, because
 * the operator varies the output directory per repetition and a claim
 * that moved with it would replenish the campaign on every rename.  The
 * root is opened once and the fd is retained, so the entry is created
 * with openat against that pinned directory: a path alias, a rename, or
 * a replacement directory underneath the original path reaches the
 * inode this device pinned rather than a new one.
 *
 * Ownership travels with the fd, not with the file's presence.  A second
 * device can stat the entry; only the device whose openat returned a
 * descriptor holds the claim, and `held` records that.  A continuation
 * that read the file rather than creating it authorizes nothing.
 *
 * The claim bounds restarting, never accounting.  A run that stops at
 * forty of four hundred leaves the same entry as one that stops at three
 * hundred and ninety-nine; how much a campaign spent is read out of its
 * published samples.  An operator who deletes the campaign store deletes
 * the claim with it, which is the managed store's boundary rather than a
 * defect in it.
 */

#ifndef R3V_MEASUREMENT_CLAIM_H
#define R3V_MEASUREMENT_CLAIM_H

#include <stdbool.h>
#include <stdint.h>

/* The entry name: a fixed prefix, the session nonce, the arm, and a
 * suffix, each bounded by the manifest's own name width. */
#define R3V_MEASUREMENT_CLAIM_NAME_MAX 192u

/* What the entry records, so a standing claim names the campaign that
 * left it rather than only its own existence. */
struct r3v_measurement_claim_record {
   const char *session_nonce;
   const char *declaration_digest;
   const char *arm_name;
   const char *platform_name;
   uint32_t pci_vendor_id;
   uint32_t pci_device_id;
   const char *kernel_release;
   const char *module_srcversion;
   /* The scope the claim covers, in the words the refusal uses: one
    * session nonce and one arm, over every device and every process. */
   const char *claim_scope;
};

struct r3v_measurement_claim {
   /* The pinned campaign root, or -1.  Held open for the device's
    * lifetime so every openat reaches the directory this bound. */
   int root_fd;
   /* True once this device's own openat created the entry.  Every
    * continuation reads this rather than the entry's presence. */
   bool held;
   char name[R3V_MEASUREMENT_CLAIM_NAME_MAX];
};

/* Brings a claim to the state bind reads: no root, no name, unheld. */
void r3v_measurement_claim_init(struct r3v_measurement_claim *claim);

/* Pins the directory holding `declaration_path` and composes the entry
 * name from the session nonce and the arm.  Both names are restricted to
 * `[A-Za-z0-9._-]` and a leading dot is refused, so a declared name
 * reaches one entry in the pinned directory and never a path of its own
 * choosing.  Returns 0, or a negative errno: -EINVAL for an unusable
 * name, -ENAMETOOLONG for a composed entry above the bound, and the
 * open's own errno for a root that cannot be pinned.
 */
int r3v_measurement_claim_bind(struct r3v_measurement_claim *claim,
                               const char *declaration_path,
                               const char *session_nonce,
                               const char *arm_name);

/* Creates the entry exclusively, writes the record, and fsyncs the file
 * and the pinned directory before returning, so a claim this reports as
 * won survives power loss and the caller submits only after.
 *
 * Returns 0 with `claim->held` set, -EEXIST when another device or
 * process already holds the arm, -EALREADY when this device already
 * holds it, or another negative errno for a write or synchronization
 * failure.  A failure after the entry exists leaves it in place: the
 * entry records that an attempt started, and unlinking it during cleanup
 * would hand the arm back to a restart.
 */
int r3v_measurement_claim_acquire(
   struct r3v_measurement_claim *claim,
   const struct r3v_measurement_claim_record *record);

/* Releases the pinned root.  The entry stays on disk. */
void r3v_measurement_claim_finish(struct r3v_measurement_claim *claim);

#endif /* R3V_MEASUREMENT_CLAIM_H */
