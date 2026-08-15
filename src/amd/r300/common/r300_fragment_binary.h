/*
 * SPDX-License-Identifier: MIT
 *
 * R3V-owned R300 fragment-program binary descriptor.
 */

#ifndef R300_FRAGMENT_BINARY_H
#define R300_FRAGMENT_BINARY_H

#include <stdbool.h>
#include <stdint.h>

/* A fragment binary a Vulkan pipeline owns outright: every consumed dword is
 * deep-copied at construction, so the descriptor stays valid after every
 * Gallium shader object is destroyed.  cb_code is the pre-baked PKT0
 * sequence for the US/FG register block, emitted verbatim at draw time;
 * fg_depth_src and us_out_w are the two register values the block keeps
 * outside the sequence.  The content hash identifies the binary across
 * captures and manifests, and compiler_identity names the producing
 * compiler.
 */
#define R300_FRAGMENT_BINARY_HASH_SIZE 32
#define R300_FRAGMENT_BINARY_IDENTITY_SIZE 64

struct r300_fragment_binary {
   uint32_t *cb_code;
   uint32_t cb_code_size;
   uint32_t fg_depth_src;
   uint32_t us_out_w;
   uint8_t hash[R300_FRAGMENT_BINARY_HASH_SIZE];
   char compiler_identity[R300_FRAGMENT_BINARY_IDENTITY_SIZE];
   bool validated;
};

/* Structural admission for a cb_code stream: the stream is a whole number of
 * type-0 packets, every payload lies inside the stream, and every written
 * register falls in the US/FG block or on the GA US vector index/data pair.
 */
bool r300_fragment_binary_stream_valid(const uint32_t *cb_code,
                                       uint32_t cb_code_size);

/* Deep-copies cb_code, validates the stream, and computes the content hash
 * over the code and both register values. Returns 0, -EINVAL on a stream the
 * validator rejects, or -ENOMEM.
 */
int r300_fragment_binary_init(struct r300_fragment_binary *binary,
                              const uint32_t *cb_code, uint32_t cb_code_size,
                              uint32_t fg_depth_src, uint32_t us_out_w,
                              const char *compiler_identity);

void r300_fragment_binary_finish(struct r300_fragment_binary *binary);

#endif /* R300_FRAGMENT_BINARY_H */
