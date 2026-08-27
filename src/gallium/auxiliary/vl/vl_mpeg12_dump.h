/*
 * SPDX-License-Identifier: MIT
 */

#ifndef VL_MPEG12_DUMP_H
#define VL_MPEG12_DUMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "util/format/u_formats.h"

struct pipe_context;
struct pipe_sampler_view;

#define VL_MPEG12_DUMP_MAX_STAGES 3
#define VL_MPEG12_DUMP_MAX_PLANES 3

struct vl_mpeg12_dump_io {
   FILE *(*open_unique)(const char *path, int mode);
   size_t (*write)(const void *data, size_t size, size_t count, FILE *stream);
   int (*sync_file)(FILE *stream);
   int (*close)(FILE *stream);
   int (*rename)(const char *old_path, const char *new_path);
   int (*remove_file)(const char *path);
   int (*remove_directory)(const char *path);
   int (*mkdir)(const char *path, int mode);
   int (*sync_directory)(const char *path);
};

struct vl_mpeg12_dump {
   char *session_path;
   uint64_t frame;
   struct vl_mpeg12_dump_io io;
};

struct vl_mpeg12_dump_stage {
   const char *name;
   enum pipe_format buffer_format;
   struct pipe_sampler_view **planes;
   unsigned plane_count;
};

const struct vl_mpeg12_dump_io *
vl_mpeg12_dump_default_io(void);

int
vl_mpeg12_dump_init(struct vl_mpeg12_dump *dump, const char *root_path);

int
vl_mpeg12_dump_init_with_io(struct vl_mpeg12_dump *dump,
                            const char *root_path,
                            const struct vl_mpeg12_dump_io *io);

void
vl_mpeg12_dump_cleanup(struct vl_mpeg12_dump *dump);

bool
vl_mpeg12_dump_enabled(const struct vl_mpeg12_dump *dump);

int
vl_mpeg12_dump_reserve_frame(struct vl_mpeg12_dump *dump,
                             uint64_t *frame_out);

int
vl_mpeg12_dump_validate_source_span(unsigned layer_count,
                                    uint64_t layer_stride,
                                    unsigned rows_per_layer,
                                    uint64_t row_stride,
                                    size_t row_bytes);

int
vl_mpeg12_dump_frame(struct vl_mpeg12_dump *dump,
                     uint64_t frame,
                     struct pipe_context *pipe,
                     const struct vl_mpeg12_dump_stage *stages,
                     unsigned stage_count);

#endif /* VL_MPEG12_DUMP_H */
