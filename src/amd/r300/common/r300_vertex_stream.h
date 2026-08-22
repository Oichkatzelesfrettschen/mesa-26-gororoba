/*
 * SPDX-License-Identifier: MIT
 */

#ifndef AMD_R300_VERTEX_STREAM_H
#define AMD_R300_VERTEX_STREAM_H

#include <stdbool.h>
#include <stdint.h>

/* One bound vertex-record stream.  Data points at the vertex-zero record,
 * after the binding base and attribute offset.  Stride is the
 * byte distance between records; zero repeats the first record for every
 * vertex.  Size_bytes bounds the readable range starting at data. */
struct r300_vertex_stream {
   const uint8_t *data;
   uint32_t stride;
   uint64_t size_bytes;
   /* Robust buffer access (the Vulkan robustBufferAccess and GL robust
    * access rule): a record whose start plus its record bytes exceeds
    * size_bytes reads raw zeros, and the components the format does not
    * carry synthesize as (0, 0, 1).  Clear, the same record refuses the
    * whole gather before any write. */
   bool oob_reads_zero;
};

#endif /* AMD_R300_VERTEX_STREAM_H */
