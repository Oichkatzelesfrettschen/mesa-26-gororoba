/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB NIR restaging: the structural classifier and the VS-to-producer-FS
 * restager, shared by the Gallium r300 driver and the direct Vulkan front
 * end.  Everything here operates on caller-owned NIR shaders and explicit
 * value parameters; driver state, BOs, and route policy stay with the
 * callers.
 */

#ifndef R300_R2VB_NIR_H
#define R300_R2VB_NIR_H

#include <stdbool.h>
#include <stdint.h>

#include "compiler/shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nir_builder;
struct nir_def;
struct nir_intrinsic_instr;
struct nir_shader;

/* VS input-list bound for the restage remap tables; matches Gallium's
 * PIPE_MAX_ATTRIBS so a shader admissible there fits here. */
#define R300_R2VB_NIR_MAX_INPUTS 32

/* Source identity of one application input feeding a producer pass: the
 * driver location the fetch reads and the input's rank in location order.
 * The producer mapping contract consumes these measured values; a caller
 * passing literals asserts an identity the scan never proved.  Re-ingest
 * maps passthrough outputs by driver location because component-packed
 * variables share one physical vertex element. */
struct r300_r2vb_position_source {
    uint8_t app_driver_location;
    uint8_t location_rank;
    bool valid;
};

/* Window-space placement for a producer position output.  With
 * divide_to_window clear the producer emits the raw clip-space vec4;
 * with it set the output is (clip.xyz / w_clip) * scale + translate with
 * w = 1, the form the re-ingest fetches verbatim.  scale/translate follow
 * pipe_viewport_state's first three components. */
struct r300_r2vb_restage_viewport {
    bool divide_to_window;
    float scale[3];
    float translate[3];
};

/* Identify the output location carried by a lowered store intrinsic.  NIR's
 * nir_intrinsic_store_output places the value in src[0], the location offset
 * in src[1], and the component mask in intrinsic indices (global -r
 * nir_intrinsic_store_output).  A constant offset within its declared slot
 * range becomes part of the target identity; indirect offsets and
 * out-of-range stores return false, keeping planner and live-restager
 * admission on one delivery record. */
bool r300_r2vb_output_store_location(const struct nir_intrinsic_instr *intr,
                                     gl_varying_slot *location);

/* This helper identifies an output store whose value is a direct
 * shader-input load.  Dereference stores and lowered store_output forms use
 * this classification when the producer distinguishes passthrough varyings
 * from computed ones. */
bool r300_r2vb_output_store_is_input_passthrough(
    const struct nir_intrinsic_instr *intr);

/* Remove output stores outside target from a caller-owned clone.  The
 * planner and live restager call this same target reduction before DCE and
 * emission. */
void r300_r2vb_prune_output_stores(struct nir_shader *nir,
                                   gl_varying_slot target);

/* Reduce a caller-owned clone to the position producer before structural
 * admission.  Non-position stores and the dead dependencies that feed them
 * stay outside the cv=0 cell. */
void r300_r2vb_prune_position_only(struct nir_shader *nir);

/* Source identity of the single application input feeding gl_Position.
 * scan_status reports a clone-allocation failure separately through
 * transient_failure so the caller retries instead of caching a reject. */
bool r300_r2vb_position_source_scan(struct nir_shader *vs_nir,
                                    struct r300_r2vb_position_source *out);

/* List form: fill up to max_sources rank-ordered records; returns the count,
 * or 0 on overflow, unrankable identity, or transient clone failure. */
unsigned r300_r2vb_position_source_scan_list_status(
    struct nir_shader *vs_nir, struct r300_r2vb_position_source *out,
    unsigned max_sources, bool *transient_failure);
bool r300_r2vb_position_source_scan_status(
    struct nir_shader *vs_nir, struct r300_r2vb_position_source *out,
    bool *transient_failure);

/* Source identity of the one application input feeding the computed varying
 * at `slot`: strip every store except that varying's, DCE, and require
 * exactly one surviving input.  Same rank/driver-location record as the
 * position scan, so the BO-fetch route feeds the varying pass through the
 * identical single-model-stream contract. */
bool r300_r2vb_varying_source_scan(struct nir_shader *vs_nir, int slot,
                                   struct r300_r2vb_position_source *out);

/* Apply the producer position-output placement: identity without
 * divide_to_window, otherwise perspective divide plus viewport with the
 * 1/32768 FP24 reciprocal guard. */
struct nir_def *r300_r2vb_nir_divide_position(
    struct nir_builder *b, struct nir_def *pos,
    const struct r300_r2vb_restage_viewport *vp);

/* Re-stage a vertex shader as the producer fragment shader for one target
 * output: clone the VS NIR, keep its arithmetic verbatim, remap attribute
 * inputs to flat fragment inputs, reduce to the target's store, and place
 * the position output per `vp`.  Returns the derived FS NIR (caller owns)
 * or NULL. */
struct nir_shader *r300_r2vb_nir_restage_vs_as_fs(
    struct nir_shader *vs_nir, gl_varying_slot target,
    const struct r300_r2vb_restage_viewport *vp);

/* Test injection: the next position-source scan reports a transient clone
 * failure. */
void r300_r2vb_test_fail_position_source_clone_once(void);

#ifdef __cplusplus
}
#endif

#endif /* R300_R2VB_NIR_H */
