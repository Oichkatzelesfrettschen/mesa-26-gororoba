/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_state.h"

#include "util/detect_os.h"
#include "util/format/u_format.h"
#include "util/os_file.h"
#include "util/u_math.h"

#include "vl_mpeg12_dump.h"

#define FIXTURE_STAGE_COUNT 3
#define FIXTURE_PLANE_COUNT 3
#define FIXTURE_LAYER_COUNT 4
#define FIXTURE_DATA_SIZE 512

#if DETECT_OS_WINDOWS
#include <direct.h>
#define TEST_CHANGE_DIRECTORY _chdir
#define TEST_GET_CURRENT_DIRECTORY _getcwd
#define TEST_PATH_SEPARATOR "\\"
#else
#include <unistd.h>
#define TEST_CHANGE_DIRECTORY chdir
#define TEST_GET_CURRENT_DIRECTORY getcwd
#define TEST_PATH_SEPARATOR "/"
#endif

static unsigned failures;

#define CHECK(condition, message)                                         \
   do {                                                                   \
      if (!(condition)) {                                                 \
         fprintf(stderr, "FAIL:%d: %s\n", __LINE__, (message));          \
         ++failures;                                                      \
      }                                                                   \
   } while (0)

struct fake_resource {
   struct pipe_resource resource;
   struct pipe_transfer transfer;
   uint8_t data[FIXTURE_DATA_SIZE];
   unsigned stage;
   unsigned plane;
   unsigned stride;
   unsigned layer_stride;
   unsigned packed_row_bytes;
   unsigned packed_rows_per_layer;
   unsigned map_count;
   unsigned unmap_count;
};

struct dump_fixture {
   struct pipe_context pipe;
   struct fake_resource resources[FIXTURE_STAGE_COUNT][FIXTURE_PLANE_COUNT];
   struct pipe_sampler_view views[FIXTURE_STAGE_COUNT][FIXTURE_PLANE_COUNT];
   struct pipe_sampler_view *plane_views[FIXTURE_STAGE_COUNT][FIXTURE_PLANE_COUNT];
   struct vl_mpeg12_dump_stage stages[FIXTURE_STAGE_COUNT];
};

static unsigned map_calls;
static unsigned fail_map_call;
static bool fail_map_with_transfer;

static uint8_t
fixture_byte(unsigned stage, unsigned plane, unsigned layer,
             unsigned row, unsigned column)
{
   return 1 + stage * 71 + plane * 23 + layer * 17 + row * 11 + column;
}

static void *
fake_texture_map(struct pipe_context *pipe,
                 struct pipe_resource *resource,
                 unsigned level,
                 unsigned usage,
                 const struct pipe_box *box,
                 struct pipe_transfer **transfer_out)
{
   (void)pipe;
   struct fake_resource *fake = (struct fake_resource *)resource;
   ++map_calls;
   CHECK(level == 1, "the selected sampler-view level is mapped");
   CHECK(usage == PIPE_MAP_READ, "the synchronized CPU read-map flag is used");
   CHECK(box->x == 0 && box->y == 0 && box->z == 1,
         "the selected layer range starts at layer one");
   CHECK(box->width == (int)u_minify(resource->width0, level) &&
            box->height == (int)u_minify(resource->height0, level) &&
            box->depth == 2,
         "the selected mip extent and layer count are mapped");

   if (fail_map_call && map_calls == fail_map_call) {
      if (fail_map_with_transfer) {
         fake->transfer.resource = resource;
         *transfer_out = &fake->transfer;
         ++fake->map_count;
      } else {
         *transfer_out = NULL;
      }
      return NULL;
   }

   ++fake->map_count;

   fake->transfer.resource = resource;
   fake->transfer.level = level;
   fake->transfer.usage = usage;
   fake->transfer.box = *box;
   fake->transfer.stride = fake->stride;
   fake->transfer.layer_stride = fake->layer_stride;
   *transfer_out = &fake->transfer;
   return fake->data + (size_t)box->z * fake->layer_stride;
}

static void
fake_texture_unmap(struct pipe_context *pipe, struct pipe_transfer *transfer)
{
   (void)pipe;
   struct fake_resource *fake = (struct fake_resource *)transfer->resource;
   ++fake->unmap_count;
}

static void
init_fixture(struct dump_fixture *fixture)
{
   static const char *stage_names[FIXTURE_STAGE_COUNT] = {
      "coeff", "stage1", "out",
   };

   memset(fixture, 0, sizeof(*fixture));
   fixture->pipe.texture_map = fake_texture_map;
   fixture->pipe.texture_unmap = fake_texture_unmap;

   for (unsigned stage = 0; stage < FIXTURE_STAGE_COUNT; ++stage) {
      fixture->stages[stage].name = stage_names[stage];
      fixture->stages[stage].buffer_format = PIPE_FORMAT_IYUV;
      fixture->stages[stage].planes = fixture->plane_views[stage];
      fixture->stages[stage].plane_count = FIXTURE_PLANE_COUNT;

      for (unsigned plane = 0; plane < FIXTURE_PLANE_COUNT; ++plane) {
         struct fake_resource *fake = &fixture->resources[stage][plane];
         struct pipe_sampler_view *view = &fixture->views[stage][plane];

         memset(fake->data, 0xee, sizeof(fake->data));
         fake->stage = stage;
         fake->plane = plane;
         fake->resource.width0 = 8;
         fake->resource.height0 = 6;
         fake->resource.depth0 = 1;
         fake->resource.array_size = FIXTURE_LAYER_COUNT;
         fake->resource.last_level = 1;
         fake->resource.target = PIPE_TEXTURE_2D_ARRAY;
         fake->resource.format = PIPE_FORMAT_R8_UINT;
         fake->stride = 7;
         fake->layer_stride = 29;
         fake->packed_row_bytes = 4;
         fake->packed_rows_per_layer = 3;

         if (stage == 0 && plane == 0) {
            fake->resource.width0 = 10;
            fake->resource.height0 = 10;
            fake->resource.format = PIPE_FORMAT_DXT1_RGBA;
            fake->stride = 19;
            fake->layer_stride = 47;
            fake->packed_row_bytes = 16;
            fake->packed_rows_per_layer = 2;
         }

         for (unsigned layer = 0; layer < FIXTURE_LAYER_COUNT; ++layer) {
            for (unsigned row = 0; row < fake->packed_rows_per_layer; ++row) {
               for (unsigned column = 0; column < fake->packed_row_bytes;
                    ++column) {
                  fake->data[layer * fake->layer_stride +
                             row * fake->stride + column] =
                     fixture_byte(stage, plane, layer, row, column);
               }
            }
         }

         view->texture = &fake->resource;
         view->format = fake->resource.format == PIPE_FORMAT_R8_UINT
                           ? PIPE_FORMAT_R8_UNORM
                           : fake->resource.format;
         view->target = PIPE_TEXTURE_2D_ARRAY;
         view->u.tex.first_level = 1;
         view->u.tex.last_level = 1;
         view->u.tex.first_layer = 1;
         view->u.tex.last_layer = 2;
         fixture->plane_views[stage][plane] = view;
      }
   }
}

static bool
make_path(char *path, size_t capacity, const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   int length = vsnprintf(path, capacity, format, arguments);
   va_end(arguments);
   return length >= 0 && (size_t)length < capacity;
}

static bool
create_test_root(char *root, size_t capacity)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();

   for (unsigned serial = 0; serial < 1024; ++serial) {
      if (!make_path(root, capacity, "vl-mpeg12-dump-test-%08u", serial))
         return false;
      int result = io->mkdir(root, 0700);
      if (result == 0)
         return true;
      if (result != -EEXIST)
         return false;
   }
   return false;
}

static int
dump_next_frame_with_state(
   struct vl_mpeg12_dump *dump,
   struct pipe_context *pipe,
   const struct vl_mpeg12_dump_stage *stages,
   unsigned stage_count,
   enum vl_mpeg12_dump_frame_state *frame_state_out)
{
   *frame_state_out = VL_MPEG12_DUMP_FRAME_CLEANED;
   uint64_t frame;
   int result = vl_mpeg12_dump_reserve_frame(dump, &frame);
   if (result)
      return result;

   return vl_mpeg12_dump_frame(dump, frame, pipe, stages, stage_count,
                               frame_state_out);
}

static int
dump_next_frame(struct vl_mpeg12_dump *dump,
                struct pipe_context *pipe,
                const struct vl_mpeg12_dump_stage *stages,
                unsigned stage_count)
{
   enum vl_mpeg12_dump_frame_state frame_state;
   return dump_next_frame_with_state(dump, pipe, stages, stage_count,
                                     &frame_state);
}

static bool
expected_payload_filename(char *filename, size_t capacity,
                          const struct vl_mpeg12_dump_stage *stage,
                          unsigned plane)
{
   const struct pipe_sampler_view *view = stage->planes[plane];
   unsigned level = view->u.tex.first_level;
   unsigned width = u_minify(view->texture->width0, level);
   unsigned height = u_minify(view->texture->height0, level);
   unsigned first_layer = view->u.tex.first_layer;
   unsigned last_layer = view->u.tex.last_layer;
   unsigned layer_count = last_layer - first_layer + 1;

   return make_path(filename, capacity,
      "%s_storage%u_level%u_layers%u-%u_%ux%ux%u_storage-%s_view-%s.raw",
      stage->name, plane, level,
      first_layer, last_layer, width, height, layer_count,
      util_format_short_name(view->texture->format),
      util_format_short_name(view->format));
}

static bool
payload_path(char *path, size_t capacity,
             const struct vl_mpeg12_dump *dump,
             uint64_t frame,
             const struct vl_mpeg12_dump_stage *stage,
             unsigned plane)
{
   char filename[1024];
   return expected_payload_filename(filename, sizeof(filename), stage, plane) &&
          make_path(path, capacity,
                    "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                    TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR "%s",
                    dump->session_path, frame, filename);
}

static size_t
expected_payload(const struct dump_fixture *fixture,
                 unsigned stage, unsigned plane, uint8_t *expected,
                 size_t capacity)
{
   const struct fake_resource *fake = &fixture->resources[stage][plane];
   size_t offset = 0;
   for (unsigned layer = 1; layer <= 2; ++layer) {
      for (unsigned row = 0; row < fake->packed_rows_per_layer; ++row) {
         for (unsigned column = 0; column < fake->packed_row_bytes; ++column) {
            CHECK(offset < capacity, "the expected payload buffer is large enough");
            if (offset < capacity)
               expected[offset++] =
                  fixture_byte(stage, plane, layer, row, column);
         }
      }
   }
   return offset;
}

static bool
file_matches(const char *path, const uint8_t *expected, size_t expected_size)
{
   size_t actual_size = 0;
   char *actual = os_read_file(path, &actual_size);
   bool matches = actual && actual_size == expected_size &&
                  memcmp(actual, expected, expected_size) == 0;
   free(actual);
   return matches;
}

static bool
payload_matches(const struct vl_mpeg12_dump *dump,
                const struct dump_fixture *fixture,
                uint64_t frame, unsigned stage, unsigned plane)
{
   char path[2048];
   uint8_t expected[128];
   size_t expected_size = expected_payload(fixture, stage, plane, expected,
                                           sizeof(expected));
   return payload_path(path, sizeof(path), dump, frame,
                       &fixture->stages[stage], plane) &&
          file_matches(path, expected, expected_size);
}

static bool
manifest_is_complete(const struct vl_mpeg12_dump *dump,
                     const struct dump_fixture *fixture,
                     uint64_t frame)
{
   char path[2048];
   if (!make_path(path, sizeof(path),
                  "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                  TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR
                  "manifest.tsv",
                  dump->session_path, frame))
      return false;

   size_t size = 0;
   char *manifest = os_read_file(path, &size);
   if (!manifest)
      return false;

   char expected[8192];
   size_t offset = 0;
   int length = snprintf(
      expected, sizeof(expected),
      "stage\tbuffer_format\tstorage_format\tview_format\tstorage_plane\t"
      "level\tfirst_layer\tlast_layer\twidth\theight\tlayer_count\t"
      "row_bytes\trows_per_layer\ttotal_bytes\tfile\n");
   if (length < 0 || (size_t)length >= sizeof(expected)) {
      free(manifest);
      return false;
   }
   offset = length;

   for (unsigned stage = 0; stage < FIXTURE_STAGE_COUNT; ++stage) {
      for (unsigned plane = 0; plane < FIXTURE_PLANE_COUNT; ++plane) {
         const struct vl_mpeg12_dump_stage *dump_stage =
            &fixture->stages[stage];
         const struct pipe_sampler_view *view = dump_stage->planes[plane];
         const struct fake_resource *fake = &fixture->resources[stage][plane];
         char filename[1024];
         if (!expected_payload_filename(filename, sizeof(filename), dump_stage,
                                        plane)) {
            free(manifest);
            return false;
         }

         unsigned level = view->u.tex.first_level;
         unsigned width = u_minify(view->texture->width0, level);
         unsigned height = u_minify(view->texture->height0, level);
         unsigned layer_count =
            view->u.tex.last_layer - view->u.tex.first_layer + 1;
         uint64_t total_bytes =
            (uint64_t)fake->packed_row_bytes *
            fake->packed_rows_per_layer * layer_count;
         length = snprintf(
            expected + offset, sizeof(expected) - offset,
            "%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%" PRIu64
            "\t%s\n",
            dump_stage->name,
            util_format_short_name(dump_stage->buffer_format),
            util_format_short_name(view->texture->format),
            util_format_short_name(view->format), plane, level,
            view->u.tex.first_layer, view->u.tex.last_layer, width, height,
            layer_count, fake->packed_row_bytes,
            fake->packed_rows_per_layer, total_bytes, filename);
         if (length < 0 || (size_t)length >= sizeof(expected) - offset) {
            free(manifest);
            return false;
         }
         offset += length;
      }
   }

   bool complete = size == offset && memcmp(manifest, expected, offset) == 0;
   free(manifest);
   return complete;
}

static void
check_complete_frame(const struct vl_mpeg12_dump *dump,
                     const struct dump_fixture *fixture,
                     uint64_t frame)
{
   for (unsigned stage = 0; stage < FIXTURE_STAGE_COUNT; ++stage) {
      for (unsigned plane = 0; plane < FIXTURE_PLANE_COUNT; ++plane) {
         CHECK(payload_matches(dump, fixture, frame, stage, plane),
               "the packed plane payload matches the selected rows and layers");
      }
   }
   CHECK(manifest_is_complete(dump, fixture, frame),
         "the manifest exactly identifies every published stage and plane");

   char complete_path[2048];
   CHECK(make_path(complete_path, sizeof(complete_path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                   TEST_PATH_SEPARATOR "complete",
                   dump->session_path, frame) &&
            vl_mpeg12_dump_default_io()->mkdir(complete_path, 0500) == -EEXIST,
         "the complete directory atomically marks an admissible frame");
}

static void
remove_frame(const struct vl_mpeg12_dump *dump,
             const struct dump_fixture *fixture,
             uint64_t frame)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   char path[2048];

   for (unsigned stage = 0; stage < FIXTURE_STAGE_COUNT; ++stage) {
      for (unsigned plane = 0; plane < FIXTURE_PLANE_COUNT; ++plane) {
         if (payload_path(path, sizeof(path), dump, frame,
                          &fixture->stages[stage], plane))
            io->remove_file(path);
      }
   }
   if (make_path(path, sizeof(path),
                 "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                 TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR
                 "manifest.tsv",
                 dump->session_path, frame))
      io->remove_file(path);
   if (make_path(path, sizeof(path),
                 "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                 TEST_PATH_SEPARATOR "complete",
                 dump->session_path, frame))
      io->remove_directory(path);
   if (make_path(path, sizeof(path),
                 "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                 TEST_PATH_SEPARATOR "payload",
                 dump->session_path, frame))
      io->remove_directory(path);
   if (make_path(path, sizeof(path),
                 "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64,
                 dump->session_path, frame))
      io->remove_directory(path);
}

static void
remove_session(struct vl_mpeg12_dump *dump)
{
   CHECK(vl_mpeg12_dump_default_io()->remove_directory(dump->session_path) == 0,
         "the session contains no unclassified residue");
   vl_mpeg12_dump_cleanup(dump);
}

enum fault_mode {
   FAULT_NONE,
   FAULT_RECOVERABLE_SHORT_WRITE,
   FAULT_TERMINAL_SHORT_WRITE,
   FAULT_CLOSE,
   FAULT_SYNC_FILE,
   FAULT_SYNC_DIRECTORY,
   FAULT_MKDIR_FAILURE,
   FAULT_MKDIR_COLLISION,
   FAULT_OPEN_COLLISION,
   FAULT_STREAM_CREATION,
};

static enum fault_mode active_fault;
static unsigned fault_target;
static unsigned fault_open_calls;
static unsigned fault_current_file;
static unsigned fault_current_write_calls;
static unsigned fault_sync_directory_calls;
static unsigned fault_remove_file_calls;
static unsigned fault_remove_file_target;
static unsigned fault_remove_directory_calls;
static unsigned fault_remove_directory_target;
static unsigned fault_mkdir_calls;
static char fault_last_mkdir_path[2048];
static char fault_collision_path[2048];
static bool fault_triggered;
static const uint8_t collision_sentinel[] = {
   0x63, 0x6f, 0x6e, 0x74, 0x65, 0x73, 0x74, 0x65, 0x64,
};

static int
fault_open_unique(const char *path, int mode, FILE **stream_out,
                  bool *created_out)
{
   ++fault_open_calls;
   fault_current_file = fault_open_calls;
   fault_current_write_calls = 0;

   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   if (active_fault == FAULT_OPEN_COLLISION &&
       fault_current_file == fault_target && !fault_triggered) {
      int length = snprintf(fault_collision_path,
                            sizeof(fault_collision_path), "%s", path);
      if (length < 0 || (size_t)length >= sizeof(fault_collision_path)) {
         errno = ENAMETOOLONG;
         return -ENAMETOOLONG;
      }

      FILE *collision_stream = NULL;
      bool collision_created = false;
      int collision_result = io->open_unique(
         path, mode, &collision_stream, &collision_created);
      if (collision_result)
         return collision_result;
      if (!collision_stream || !collision_created)
         return -EIO;

      collision_result = fwrite(collision_sentinel, 1,
                                sizeof(collision_sentinel),
                                collision_stream) == sizeof(collision_sentinel)
         ? io->sync_file(collision_stream)
         : -EIO;
      int close_result = io->close(collision_stream);
      if (!collision_result && close_result != 0)
         collision_result = -EIO;
      if (collision_result)
         return collision_result;
      fault_triggered = true;
   }

   if (active_fault == FAULT_STREAM_CREATION &&
       fault_current_file == fault_target && !fault_triggered) {
      int result = io->open_unique(path, mode, stream_out, created_out);
      if (result)
         return result;
      if (!*stream_out || !*created_out)
         return -EIO;
      if (io->close(*stream_out) != 0)
         return -EIO;
      *stream_out = NULL;
      fault_triggered = true;
      return -ENOMEM;
   }

   return io->open_unique(path, mode, stream_out, created_out);
}

static size_t
fault_write(const void *data, size_t size, size_t count, FILE *stream)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   ++fault_current_write_calls;

   if ((active_fault == FAULT_RECOVERABLE_SHORT_WRITE ||
        active_fault == FAULT_TERMINAL_SHORT_WRITE) &&
       fault_current_file == fault_target && fault_current_write_calls == 1 &&
       count > 1)
      return io->write(data, size, count / 2, stream);

   if (active_fault == FAULT_TERMINAL_SHORT_WRITE &&
       fault_current_file == fault_target && fault_current_write_calls == 2) {
      errno = ENOSPC;
      return 0;
   }

   return io->write(data, size, count, stream);
}

static int
fault_sync_file(FILE *stream)
{
   if (active_fault == FAULT_SYNC_FILE &&
       fault_current_file == fault_target && !fault_triggered) {
      fault_triggered = true;
      return -ENOSPC;
   }
   return vl_mpeg12_dump_default_io()->sync_file(stream);
}

static int
fault_close(FILE *stream)
{
   int result = vl_mpeg12_dump_default_io()->close(stream);
   if (active_fault == FAULT_CLOSE && fault_current_file == fault_target &&
       !fault_triggered) {
      fault_triggered = true;
      errno = ENOSPC;
      return EOF;
   }
   return result;
}

static int
fault_sync_directory(const char *path)
{
   ++fault_sync_directory_calls;
   if (active_fault == FAULT_SYNC_DIRECTORY &&
       fault_sync_directory_calls == fault_target && !fault_triggered) {
      fault_triggered = true;
      return -EIO;
   }
   return vl_mpeg12_dump_default_io()->sync_directory(path);
}

static int
fault_remove_file(const char *path)
{
   ++fault_remove_file_calls;
   if (fault_remove_file_target &&
       fault_remove_file_calls == fault_remove_file_target)
      return -EACCES;
   return vl_mpeg12_dump_default_io()->remove_file(path);
}

static int
fault_remove_directory(const char *path)
{
   ++fault_remove_directory_calls;
   if (fault_remove_directory_target &&
       fault_remove_directory_calls == fault_remove_directory_target)
      return -EACCES;
   return vl_mpeg12_dump_default_io()->remove_directory(path);
}

static int
fault_mkdir(const char *path, int mode)
{
   ++fault_mkdir_calls;
   int length = snprintf(fault_last_mkdir_path,
                         sizeof(fault_last_mkdir_path), "%s", path);
   if (length < 0 || (size_t)length >= sizeof(fault_last_mkdir_path))
      return -ENAMETOOLONG;

   if (active_fault == FAULT_MKDIR_FAILURE &&
       fault_mkdir_calls == fault_target && !fault_triggered) {
      fault_triggered = true;
      return -ENOSPC;
   }

   if (active_fault == FAULT_MKDIR_COLLISION &&
       fault_mkdir_calls == fault_target && !fault_triggered) {
      int result = vl_mpeg12_dump_default_io()->mkdir(path, mode);
      if (result)
         return result;
      fault_triggered = true;
   }
   return vl_mpeg12_dump_default_io()->mkdir(path, mode);
}

static struct vl_mpeg12_dump_io
fault_io(void)
{
   struct vl_mpeg12_dump_io io = *vl_mpeg12_dump_default_io();
   io.open_unique = fault_open_unique;
   io.write = fault_write;
   io.sync_file = fault_sync_file;
   io.close = fault_close;
   io.sync_directory = fault_sync_directory;
   io.remove_file = fault_remove_file;
   io.remove_directory = fault_remove_directory;
   io.mkdir = fault_mkdir;
   return io;
}

static void
reset_fault(enum fault_mode mode, unsigned target)
{
   active_fault = mode;
   fault_target = target;
   fault_open_calls = 0;
   fault_current_file = 0;
   fault_current_write_calls = 0;
   fault_sync_directory_calls = 0;
   fault_remove_file_calls = 0;
   fault_remove_file_target = 0;
   fault_remove_directory_calls = 0;
   fault_remove_directory_target = 0;
   fault_mkdir_calls = 0;
   fault_last_mkdir_path[0] = '\0';
   fault_collision_path[0] = '\0';
   fault_triggered = false;
}

static void
check_failure_leaves_no_frame(const char *root,
                              struct dump_fixture *fixture,
                              enum fault_mode mode,
                              unsigned target,
                              int expected_result,
                              const char *message)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "a fault-calibration session is created");
   reset_fault(mode, target);
   enum vl_mpeg12_dump_frame_state frame_state;
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == expected_result,
         message);
   CHECK(frame_state == VL_MPEG12_DUMP_FRAME_CLEANED,
         "a pre-admission failure removes every publisher-owned frame path");
   CHECK(dump.frame == 1,
         "a failed capture reserves its decoded-frame identity");

   reset_fault(FAULT_NONE, 0);
   CHECK(dump_next_frame(&dump, &fixture->pipe, fixture->stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "the decoded frame after a failed capture remains publishable");
   check_complete_frame(&dump, fixture, 1);
   remove_frame(&dump, fixture, 1);
   remove_session(&dump);
   reset_fault(FAULT_NONE, 0);
}

static void
check_stream_creation_cleanup_failure_is_explicit(
   const char *root, struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the stream-creation-failure session is created");

   reset_fault(FAULT_STREAM_CREATION, 1);
   fault_remove_file_target = 1;
   enum vl_mpeg12_dump_frame_state frame_state;
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EACCES,
         "a created-file cleanup failure supersedes stream creation failure");
   CHECK(frame_state == VL_MPEG12_DUMP_FRAME_RETAINED && fault_triggered,
         "a created file that cannot be removed is reported as retained");

   char path[2048];
   CHECK(payload_path(path, sizeof(path), &dump, 0, &fixture->stages[0], 0) &&
            vl_mpeg12_dump_default_io()->remove_file(path) == 0,
         "the retained empty payload is classified and removed");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "payload",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the retained payload directory is removed");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the retained frame directory is removed");
   remove_session(&dump);
   reset_fault(FAULT_NONE, 0);
}

static unsigned session_collision_mkdir_calls;
static unsigned session_identity_calls;
static unsigned session_exhaustion_mkdir_calls;
static char first_session_collision_path[2048];

static void
session_identity_for_serial(void *data, size_t size, uint64_t serial)
{
   uint8_t *bytes = data;
   for (unsigned index = 0; index < size; ++index) {
      unsigned shift = (index % sizeof(serial)) * 8;
      uint8_t byte = (uint8_t)(serial >> shift);
      bytes[index] = index < sizeof(serial) ? byte : (uint8_t)~byte;
   }
}

static int
session_identity_random_bytes(void *data, size_t size)
{
   if (!data || size != 16)
      return -EINVAL;
   ++session_identity_calls;
   session_identity_for_serial(data, size, session_identity_calls);
   return 0;
}

static int
session_alternating_random_bytes(void *data, size_t size)
{
   if (!data || size != 16)
      return -EINVAL;
   ++session_identity_calls;
   session_identity_for_serial(data, size, 1 + session_identity_calls % 2);
   return 0;
}

static int
session_exhaustion_mkdir(const char *path, int mode)
{
   (void)path;
   (void)mode;
   ++session_exhaustion_mkdir_calls;
   return -EEXIST;
}

static int
session_collision_mkdir(const char *path, int mode)
{
   ++session_collision_mkdir_calls;
   if (session_collision_mkdir_calls == 1) {
      int length = snprintf(first_session_collision_path,
                            sizeof(first_session_collision_path), "%s", path);
      if (length < 0 || (size_t)length >= sizeof(first_session_collision_path))
         return -ENAMETOOLONG;
      return -EEXIST;
   }
   CHECK(strcmp(first_session_collision_path, path) != 0,
         "a session collision regenerates the random identity");
   return vl_mpeg12_dump_default_io()->mkdir(path, mode);
}

static int
init_with_temporary_io(struct vl_mpeg12_dump *dump, const char *root)
{
   struct vl_mpeg12_dump_io io = *vl_mpeg12_dump_default_io();
   return vl_mpeg12_dump_init_with_io(dump, root, &io);
}

static char *
copy_absolute_path_with_malloc(const char *path)
{
   char *default_path = vl_mpeg12_dump_default_io()->absolute_path(path);
   if (!default_path)
      return NULL;

   size_t size = strlen(default_path) + 1;
   char *copy = malloc(size);
   if (copy)
      memcpy(copy, default_path, size);
   else
      errno = ENOMEM;
   free(default_path);
   return copy;
}

static void
check_absolute_path_allocator_contract(const char *root)
{
   struct vl_mpeg12_dump_io io = *vl_mpeg12_dump_default_io();
   io.absolute_path = copy_absolute_path_with_malloc;

   struct vl_mpeg12_dump dump;
   int result = vl_mpeg12_dump_init_with_io(&dump, root, &io);
   CHECK(result == 0,
         "an injected absolute path uses the documented allocator contract");
   if (!result)
      remove_session(&dump);
}

static void
check_cleanup_failure_is_explicit(const char *root,
                                  struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the cleanup-failure session is created");
   reset_fault(FAULT_TERMINAL_SHORT_WRITE, 5);
   fault_remove_file_target = 1;
   enum vl_mpeg12_dump_frame_state frame_state;
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EACCES,
         "a cleanup failure supersedes the payload error explicitly");
   CHECK(frame_state == VL_MPEG12_DUMP_FRAME_RETAINED,
         "a failed cleanup reports retained non-admitted data");
   CHECK(dump.frame == 1,
         "cleanup failure preserves the attempted decoded-frame identity");

   char path[2048];
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "complete",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == -ENOENT,
         "an incomplete frame never gains the completion marker");

   reset_fault(FAULT_NONE, 0);
   CHECK(dump_next_frame(&dump, &fixture->pipe, fixture->stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "cleanup residue cannot block the next decoded frame");
   check_complete_frame(&dump, fixture, 1);
   remove_frame(&dump, fixture, 1);

   char filename[1024];
   CHECK(expected_payload_filename(filename, sizeof(filename),
                                   &fixture->stages[1], 1),
         "the retained failed-payload name is representable");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR "%s",
                   dump.session_path, 0, filename) &&
            vl_mpeg12_dump_default_io()->remove_file(path) == 0,
         "the deliberately retained failed payload is classified and removed");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "payload",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the deliberately retained payload directory is removed");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the deliberately retained incomplete frame is removed");
   remove_session(&dump);
}

static void
check_directory_removal_failure_is_explicit(const char *root,
                                             struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the directory-cleanup session is created");

   reset_fault(FAULT_TERMINAL_SHORT_WRITE, 5);
   fault_remove_directory_target = 1;
   enum vl_mpeg12_dump_frame_state frame_state;
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EACCES,
         "a directory-removal failure supersedes the payload error");
   CHECK(frame_state == VL_MPEG12_DUMP_FRAME_RETAINED,
         "a failed directory cleanup reports retained non-admitted data");
   CHECK(fault_remove_directory_calls == 1,
         "cleanup attempts the payload-directory removal once");

   char path[2048];
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "complete",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == -ENOENT,
         "directory-cleanup failure never admits the incomplete frame");

   reset_fault(FAULT_NONE, 0);
   CHECK(dump_next_frame(&dump, &fixture->pipe, fixture->stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "directory-cleanup residue cannot reuse the failed frame identity");
   check_complete_frame(&dump, fixture, 1);
   remove_frame(&dump, fixture, 1);

   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "payload",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the retained empty payload directory is classified and removed");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the retained incomplete frame directory is classified and removed");
   remove_session(&dump);
}

static void
check_leaf_collision_preserves_namespace(const char *root,
                                         struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the leaf-collision session is created");

   enum vl_mpeg12_dump_frame_state frame_state;
   reset_fault(FAULT_MKDIR_COLLISION, 2);
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EEXIST &&
            frame_state == VL_MPEG12_DUMP_FRAME_RETAINED && fault_triggered,
         "a colliding payload directory is retained without replacement");

   char path[2048];
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u"
                   TEST_PATH_SEPARATOR "payload",
                   dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the injected payload collision remains empty and removable");
   CHECK(make_path(path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u", dump.session_path,
                   0) &&
            vl_mpeg12_dump_default_io()->remove_directory(path) == 0,
         "the frame containing the payload collision remains classified");

   reset_fault(FAULT_MKDIR_COLLISION, 3);
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EEXIST &&
            frame_state == VL_MPEG12_DUMP_FRAME_RETAINED && fault_triggered,
         "a colliding completion marker preserves the published payload");
   check_complete_frame(&dump, fixture, 1);

   reset_fault(FAULT_NONE, 0);
   remove_frame(&dump, fixture, 1);
   remove_session(&dump);
}

static void
check_file_collision_preserves_namespace(const char *root,
                                         struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the file-collision session is created");

   enum vl_mpeg12_dump_frame_state frame_state;
   reset_fault(FAULT_OPEN_COLLISION, 1);
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EEXIST &&
            frame_state == VL_MPEG12_DUMP_FRAME_RETAINED && fault_triggered,
         "a colliding raw payload retains the contested frame");
   CHECK(file_matches(fault_collision_path, collision_sentinel,
                      sizeof(collision_sentinel)),
         "the colliding raw payload remains byte-exact");
   remove_frame(&dump, fixture, 0);

   reset_fault(FAULT_OPEN_COLLISION, 10);
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EEXIST &&
            frame_state == VL_MPEG12_DUMP_FRAME_RETAINED && fault_triggered,
         "a colliding manifest retains every completed raw payload");
   CHECK(file_matches(fault_collision_path, collision_sentinel,
                      sizeof(collision_sentinel)),
         "the colliding manifest remains byte-exact");
   CHECK(payload_matches(&dump, fixture, 1, 0, 0) &&
            payload_matches(&dump, fixture, 1, 2, 2),
         "manifest collision retains the complete raw payload set");
   remove_frame(&dump, fixture, 1);
   remove_session(&dump);
   reset_fault(FAULT_NONE, 0);
}

static void
check_postcommit_sync_failure_preserves_frame(const char *root,
                                              struct dump_fixture *fixture)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == 0,
         "the postcommit-failure session is created");

   reset_fault(FAULT_SYNC_DIRECTORY, 4);
   fault_remove_directory_target = 1;
   enum vl_mpeg12_dump_frame_state frame_state;
   CHECK(dump_next_frame_with_state(
            &dump, &fixture->pipe, fixture->stages, FIXTURE_STAGE_COUNT,
            &frame_state) == -EIO,
         "a completion-marker sync failure reports uncertain durability");
   CHECK(frame_state == VL_MPEG12_DUMP_FRAME_ADMITTED,
         "a post-admission error reports the irreversible frame state");
   CHECK(fault_triggered && fault_remove_directory_calls == 0,
         "postcommit failure never retracts the completion marker");
   CHECK(dump.frame == 1,
         "the committed frame retains its decoded-frame identity");
   check_complete_frame(&dump, fixture, 0);

   reset_fault(FAULT_NONE, 0);
   remove_frame(&dump, fixture, 0);
   remove_session(&dump);
}

static void
check_session_root_sync_failure(const char *root)
{
   struct vl_mpeg12_dump_io io = fault_io();
   struct vl_mpeg12_dump dump;

   reset_fault(FAULT_SYNC_DIRECTORY, 1);
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == -EIO,
         "session initialization reports a dump-root sync failure");
   CHECK(!vl_mpeg12_dump_enabled(&dump),
         "a root-sync failure never enables the dump session");
   CHECK(fault_last_mkdir_path[0] &&
            vl_mpeg12_dump_default_io()->remove_directory(
               fault_last_mkdir_path) == -ENOENT,
         "root-sync failure removes the uncommitted session directory");

   reset_fault(FAULT_SYNC_DIRECTORY, 1);
   fault_remove_directory_target = 1;
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == -EACCES,
         "session cleanup failure supersedes the root-sync error");
   CHECK(fault_last_mkdir_path[0] &&
            vl_mpeg12_dump_default_io()->mkdir(
               fault_last_mkdir_path, 0700) == -EEXIST,
         "failed session cleanup preserves the exact classified directory");
   CHECK(vl_mpeg12_dump_default_io()->remove_directory(
            fault_last_mkdir_path) == 0 &&
            vl_mpeg12_dump_default_io()->sync_directory(root) == 0,
         "the retained failed-session directory is removed durably");
   reset_fault(FAULT_NONE, 0);
}

static void
check_frame_reservation_overflow(const char *root)
{
   struct vl_mpeg12_dump dump;
   CHECK(vl_mpeg12_dump_init(&dump, root) == 0,
         "the frame-overflow session is created");
   dump.frame = UINT64_MAX;
   uint64_t frame = 0;
   CHECK(vl_mpeg12_dump_reserve_frame(&dump, &frame) == -EOVERFLOW &&
            dump.frame == UINT64_MAX,
         "frame reservation fails closed at the identity limit");
   remove_session(&dump);
}

static void
check_source_span_validation(void)
{
   CHECK(vl_mpeg12_dump_validate_source_span(2, 29, 3, 7, 4) == 0,
         "mapped source offsets cover the calibrated fixture span");
   CHECK(vl_mpeg12_dump_validate_source_span(
            3, UINT64_MAX, 1, 0, 1) == -EOVERFLOW,
         "layer-offset multiplication overflow fails before dereference");
   CHECK(vl_mpeg12_dump_validate_source_span(
            1, 0, 3, UINT64_MAX, 1) == -EOVERFLOW,
         "row-offset multiplication overflow fails before dereference");
   CHECK(vl_mpeg12_dump_validate_source_span(
            2, UINT64_MAX - 1, 2, 2, 1) == -EOVERFLOW,
         "combined layer and row offsets fail closed on overflow");
   CHECK(vl_mpeg12_dump_validate_source_span(
            2, SIZE_MAX, 1, 0, 1) == -EOVERFLOW,
         "the final mapped byte must fit the host address space");
}

#if DETECT_OS_WINDOWS
static void
check_windows_absolute_path_contract(void)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   static const struct {
      const char *path;
      const char *expected;
      const char *message;
   } accepted_paths[] = {
      {"C:\\dump", "\\\\?\\C:\\dump",
       "an absolute drive path gains the extended prefix"},
      {"C:/dump/../evidence", "\\\\?\\C:\\evidence",
       "a drive path uses native separators and canonical components"},
      {"C:\\", "\\\\?\\C:\\",
       "a drive root retains its root separator"},
      {"\\\\server\\share\\dump",
       "\\\\?\\UNC\\server\\share\\dump",
       "a complete UNC path gains the extended UNC prefix"},
      {"//server/share/dump/../evidence",
       "\\\\?\\UNC\\server\\share\\evidence",
       "a UNC path uses native separators and canonical components"},
      {"\\\\server\\share\\", "\\\\?\\UNC\\server\\share",
       "a UNC share root has one canonical spelling"},
      {"\\\\?\\C:\\dump", "\\\\?\\C:\\dump",
       "an extended drive path remains absolute"},
      {"\\\\?\\C:\\dump\\..\\evidence",
       "\\\\?\\C:\\evidence",
       "an extended drive path is canonicalized"},
      {"\\\\?\\C:\\a\\..", "\\\\?\\C:\\",
       "an extended drive path cannot lose its root separator"},
      {"\\\\?\\UNC\\server\\share\\dump",
       "\\\\?\\UNC\\server\\share\\dump",
       "an extended UNC path preserves its complete share root"},
      {"\\\\?\\unc\\server\\share\\dump",
       "\\\\?\\UNC\\server\\share\\dump",
       "the extended UNC namespace token has one canonical spelling"},
   };

   for (unsigned index = 0; index < ARRAY_SIZE(accepted_paths); ++index) {
      char *absolute = io->absolute_path(accepted_paths[index].path);
      CHECK(absolute &&
               strcmp(absolute, accepted_paths[index].expected) == 0,
            accepted_paths[index].message);
      free(absolute);
   }

   static const char invalid_utf8[] = {(char)0xc3, 0x28, 0};

   static const struct {
      const char *path;
      const char *message;
   } rejected_paths[] = {
      {"", "an empty path is rejected"},
      {"C:", "a drive designator without a root is rejected"},
      {"C:relative", "an ordinary drive-relative path is rejected"},
      {"\\dump", "a drive-root-relative path is rejected"},
      {"/dump", "a slash-root-relative path is rejected"},
      {"1:\\dump", "a nonalphabetic drive designator is rejected"},
      {"\\\\server", "an ordinary UNC path requires a share"},
      {"\\\\server\\",
       "an ordinary UNC path requires a nonempty share"},
      {"\\\\?\\C:relative",
       "an extended drive-relative path is rejected"},
      {"\\\\?\\1:\\dump",
       "an extended drive requires an alphabetic designator"},
      {"\\\\?\\", "an empty extended namespace is rejected"},
      {"\\\\?\\UNC\\server",
       "an extended UNC path requires a share"},
      {"\\\\?\\UNC\\server\\",
       "an extended UNC path requires a nonempty share"},
      {"\\\\?\\UNC\\\\share",
       "an extended UNC path requires a nonempty server"},
      {"\\\\.\\C:\\dump",
       "a Win32 device path is rejected"},
      {"\\??\\C:\\dump", "an NT object-manager path is rejected"},
      {"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\dump",
       "the extended GLOBALROOT device namespace is rejected"},
      {"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\dump",
       "an extended volume device namespace is rejected"},
      {"\\\\?\\PIPE\\dump",
       "an extended named-pipe device namespace is rejected"},
      {invalid_utf8, "invalid UTF-8 is rejected"},
   };

   for (unsigned index = 0; index < ARRAY_SIZE(rejected_paths); ++index) {
      errno = 0;
      char *absolute = io->absolute_path(rejected_paths[index].path);
      CHECK(!absolute && errno == EINVAL, rejected_paths[index].message);
      free(absolute);
   }

   errno = 0;
   char *absolute = io->absolute_path(NULL);
   CHECK(!absolute && errno == EINVAL, "a null path is rejected");
   free(absolute);
}
#endif

static bool
make_session_identity_path(char *path, size_t capacity, const char *root,
                           uint64_t serial)
{
   uint8_t identity[16];
   session_identity_for_serial(identity, sizeof(identity), serial);
   static const char hex[] = "0123456789abcdef";
   char identity_hex[sizeof(identity) * 2 + 1];
   for (unsigned index = 0; index < sizeof(identity); ++index) {
      identity_hex[index * 2] = hex[identity[index] >> 4];
      identity_hex[index * 2 + 1] = hex[identity[index] & 0xf];
   }
   identity_hex[sizeof(identity_hex) - 1] = '\0';

   return make_path(path, capacity,
                    "%s" TEST_PATH_SEPARATOR "mpeg12-dump-session-%s",
                    root, identity_hex);
}

static void
check_retained_random_sessions_do_not_exhaust_namespace(const char *root)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   unsigned created_count = 0;
   char path[2048];

   for (unsigned serial = 1; serial <= 1024; ++serial) {
      if (!make_session_identity_path(path, sizeof(path), root, serial)) {
         CHECK(false, "the retained random session path is representable");
         break;
      }
      int result = io->mkdir(path, 0700);
      CHECK(result == 0, "the retained sequential session is created");
      if (result)
         break;
      ++created_count;
   }

   struct vl_mpeg12_dump_io collision_io = *io;
   collision_io.random_bytes = session_identity_random_bytes;
   session_identity_calls = 0;
   struct vl_mpeg12_dump dump;
   int init_result = vl_mpeg12_dump_init_with_io(
      &dump, root, &collision_io);
   CHECK(init_result == 0 && session_identity_calls == 1025,
         "1,024 retained random sessions advance to a free identity");
   if (!init_result)
      remove_session(&dump);

   for (unsigned serial = created_count; serial > 0; --serial) {
      CHECK(make_session_identity_path(path, sizeof(path), root, serial) &&
               io->remove_directory(path) == 0,
            "the retained random session fixture is removed");
   }
}

static void
check_session_collision_exhaustion_is_bounded(const char *root)
{
   struct vl_mpeg12_dump_io io = *vl_mpeg12_dump_default_io();
   io.random_bytes = session_alternating_random_bytes;
   io.mkdir = session_exhaustion_mkdir;
   session_identity_calls = 0;
   session_exhaustion_mkdir_calls = 0;

   struct vl_mpeg12_dump dump;
   CHECK(vl_mpeg12_dump_init_with_io(&dump, root, &io) == -EEXIST,
         "a fully colliding session namespace returns EEXIST");
   CHECK(session_identity_calls == VL_MPEG12_DUMP_SESSION_ATTEMPTS &&
            session_exhaustion_mkdir_calls == VL_MPEG12_DUMP_SESSION_ATTEMPTS,
         "alternating colliding identities stop at the declared bound");
   CHECK(!vl_mpeg12_dump_enabled(&dump),
         "session exhaustion never enables a dump namespace");
}

static void
check_relative_root_is_anchored(const char *root,
                                struct dump_fixture *fixture)
{
   char original_directory[4096] = {0};
   CHECK(TEST_GET_CURRENT_DIRECTORY(original_directory,
                                    sizeof(original_directory)) != NULL,
         "the original working directory is recorded");
   if (!original_directory[0])
      return;

   struct vl_mpeg12_dump dump;
   int init_result = vl_mpeg12_dump_init(&dump, root);
   CHECK(init_result == 0, "the relative-root session is created");
   if (init_result)
      return;

   int change_result = TEST_CHANGE_DIRECTORY(root);
   CHECK(change_result == 0,
         "the process working directory changes after session creation");
   int dump_result = change_result == 0
      ? dump_next_frame(&dump, &fixture->pipe, fixture->stages,
                        FIXTURE_STAGE_COUNT)
      : -EIO;
   int restore_result = TEST_CHANGE_DIRECTORY(original_directory);
   CHECK(restore_result == 0, "the original working directory is restored");
   if (restore_result)
      return;

   CHECK(dump_result == 0,
         "the anchored session remains usable after a working-directory change");
   if (!dump_result) {
      check_complete_frame(&dump, fixture, 0);
      remove_frame(&dump, fixture, 0);
   }
   remove_session(&dump);
}

int
main(void)
{
   struct dump_fixture fixture;
   struct vl_mpeg12_dump first;
   struct vl_mpeg12_dump second;
   char root[128];

   init_fixture(&fixture);
   if (!create_test_root(root, sizeof(root))) {
      fputs("FAIL: the test root is created\n", stderr);
      return 1;
   }

   check_session_root_sync_failure(root);
   check_absolute_path_allocator_contract(root);
   check_frame_reservation_overflow(root);
   check_source_span_validation();
#if DETECT_OS_WINDOWS
   check_windows_absolute_path_contract();
#endif
   check_retained_random_sessions_do_not_exhaust_namespace(root);
   check_session_collision_exhaustion_is_bounded(root);
   check_relative_root_is_anchored(root, &fixture);

   if (vl_mpeg12_dump_init(&first, root) != 0) {
      fputs("FAIL: the first decoder session is created\n", stderr);
      vl_mpeg12_dump_default_io()->remove_directory(root);
      return 1;
   }
   if (vl_mpeg12_dump_init(&second, root) != 0) {
      fputs("FAIL: the second decoder session is created\n", stderr);
      remove_session(&first);
      vl_mpeg12_dump_default_io()->remove_directory(root);
      return 1;
   }
   CHECK(strcmp(first.session_path, second.session_path) != 0,
         "decoder sessions reserve different exclusive namespaces");

   struct vl_mpeg12_dump_io collision_io = *vl_mpeg12_dump_default_io();
   struct vl_mpeg12_dump collision_retry_dump;
   collision_io.random_bytes = session_identity_random_bytes;
   collision_io.mkdir = session_collision_mkdir;
   session_collision_mkdir_calls = 0;
   session_identity_calls = 0;
   first_session_collision_path[0] = '\0';
   CHECK(vl_mpeg12_dump_init_with_io(&collision_retry_dump, root,
                                     &collision_io) == 0 &&
            session_collision_mkdir_calls == 2 &&
            session_identity_calls == 2,
         "session creation retries an existing cross-process namespace");
   remove_session(&collision_retry_dump);

   struct vl_mpeg12_dump copied_io_dump;
   CHECK(init_with_temporary_io(&copied_io_dump, root) == 0,
         "dump initialization copies its callback table by value");
   CHECK(dump_next_frame(&copied_io_dump, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "a copied callback table remains valid after its caller returns");
   check_complete_frame(&copied_io_dump, &fixture, 0);
   remove_frame(&copied_io_dump, &fixture, 0);
   remove_session(&copied_io_dump);

   CHECK(dump_next_frame(&first, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "the known-good frame publishes successfully");
   check_complete_frame(&first, &fixture, 0);

   CHECK(dump_next_frame(&second, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "a second decoder publishes into its own namespace");
   check_complete_frame(&second, &fixture, 0);
   check_complete_frame(&first, &fixture, 0);

   struct vl_mpeg12_dump empty_collision_dump;
   CHECK(vl_mpeg12_dump_init(&empty_collision_dump, root) == 0,
         "the empty-collision session is created");
   char empty_frame_path[2048];
   CHECK(make_path(empty_frame_path, sizeof(empty_frame_path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020u",
                   empty_collision_dump.session_path, 0) &&
            vl_mpeg12_dump_default_io()->mkdir(empty_frame_path, 0700) == 0,
         "the empty destination frame is reserved by an earlier producer");
   CHECK(dump_next_frame(&empty_collision_dump, &fixture.pipe,
                         fixture.stages, FIXTURE_STAGE_COUNT) == -EEXIST,
         "an empty destination frame is never replaced");
   CHECK(empty_collision_dump.frame == 1,
         "an empty collision still reserves its decoded-frame identity");
   CHECK(vl_mpeg12_dump_default_io()->remove_directory(empty_frame_path) == 0,
         "the original empty destination remains intact");
   remove_session(&empty_collision_dump);

   first.frame = 0;
   CHECK(dump_next_frame(&first, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) != 0,
         "an existing final frame directory refuses replacement");
   CHECK(first.frame == 1,
         "a collision leaves a gap instead of reusing a decoded-frame identity");
   check_complete_frame(&first, &fixture, 0);

   struct vl_mpeg12_dump_io short_io = fault_io();
   struct vl_mpeg12_dump short_dump;
   reset_fault(FAULT_NONE, 0);
   CHECK(vl_mpeg12_dump_init_with_io(&short_dump, root, &short_io) == 0,
         "the recoverable short-write session is created");
   reset_fault(FAULT_RECOVERABLE_SHORT_WRITE, 1);
   CHECK(dump_next_frame(&short_dump, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "a positive short write is completed without data loss");
   check_complete_frame(&short_dump, &fixture, 0);
   reset_fault(FAULT_NONE, 0);

   check_failure_leaves_no_frame(
      root, &fixture, FAULT_TERMINAL_SHORT_WRITE, 1, -ENOSPC,
      "an early terminal short write returns ENOSPC");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_TERMINAL_SHORT_WRITE, 5, -ENOSPC,
      "a late terminal short write removes earlier payloads");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_TERMINAL_SHORT_WRITE, 10, -ENOSPC,
      "a manifest short write removes every payload");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_CLOSE, 1, -ENOSPC,
      "an early payload close failure prevents admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_CLOSE, 5, -ENOSPC,
      "a late payload close failure removes earlier payloads");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_CLOSE, 10, -ENOSPC,
      "a manifest close failure removes every payload");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_FILE, 1, -ENOSPC,
      "an early payload synchronization failure prevents admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_FILE, 9, -ENOSPC,
      "a late payload synchronization failure removes earlier payloads");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_FILE, 10, -ENOSPC,
      "a manifest synchronization failure prevents admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_STREAM_CREATION, 1, -ENOMEM,
      "a created payload without a stream is removed before admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_DIRECTORY, 1, -EIO,
      "a payload-directory synchronization failure prevents publication");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_DIRECTORY, 2, -EIO,
      "a frame-directory synchronization failure prevents admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_SYNC_DIRECTORY, 3, -EIO,
      "a session-directory synchronization failure prevents admission");
   check_failure_leaves_no_frame(
      root, &fixture, FAULT_MKDIR_FAILURE, 3, -ENOSPC,
      "a completion-marker creation failure removes the unadmitted frame");
   check_postcommit_sync_failure_preserves_frame(root, &fixture);
   check_leaf_collision_preserves_namespace(root, &fixture);
   check_file_collision_preserves_namespace(root, &fixture);
   check_stream_creation_cleanup_failure_is_explicit(root, &fixture);
   check_cleanup_failure_is_explicit(root, &fixture);
   check_directory_removal_failure_is_explicit(root, &fixture);

   struct vl_mpeg12_dump map_dump;
   CHECK(vl_mpeg12_dump_init(&map_dump, root) == 0,
         "the map-failure session is created");
   map_calls = 0;
   fail_map_call = 5;
   CHECK(dump_next_frame(&map_dump, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == -EIO,
         "a late failed map removes every earlier payload");
   CHECK(map_dump.frame == 1,
         "a failed map leaves a gap instead of shifting later frame identity");
   fail_map_call = 0;
   map_calls = 0;
   CHECK(dump_next_frame(&map_dump, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "the decoded frame after a map failure remains publishable");
   check_complete_frame(&map_dump, &fixture, 1);
   remove_frame(&map_dump, &fixture, 1);
   remove_session(&map_dump);

   struct vl_mpeg12_dump malformed_map_dump;
   CHECK(vl_mpeg12_dump_init(&malformed_map_dump, root) == 0,
         "the malformed-map session is created");
   unsigned malformed_map_count = fixture.resources[0][0].map_count;
   unsigned malformed_unmap_count = fixture.resources[0][0].unmap_count;
   map_calls = 0;
   fail_map_call = 1;
   fail_map_with_transfer = true;
   CHECK(dump_next_frame(&malformed_map_dump, &fixture.pipe,
                         fixture.stages, FIXTURE_STAGE_COUNT) == -EIO,
         "a null map with a returned transfer fails explicitly");
   CHECK(fixture.resources[0][0].map_count == malformed_map_count + 1 &&
            fixture.resources[0][0].unmap_count == malformed_unmap_count + 1,
         "a returned transfer is unmapped even when the map pointer is null");
   fail_map_call = 0;
   fail_map_with_transfer = false;
   remove_session(&malformed_map_dump);

   struct vl_mpeg12_dump missing_map_dump;
   struct pipe_context missing_map_pipe = fixture.pipe;
   missing_map_pipe.texture_map = NULL;
   CHECK(vl_mpeg12_dump_init(&missing_map_dump, root) == 0,
         "the missing-map-callback session is created");
   CHECK(dump_next_frame(&missing_map_dump, &missing_map_pipe,
                         fixture.stages, FIXTURE_STAGE_COUNT) == -EINVAL,
         "a missing texture-map callback fails validation without a crash");
   CHECK(missing_map_dump.frame == 1,
         "invalid callback state consumes its decoded-frame identity");
   remove_session(&missing_map_dump);

   struct vl_mpeg12_dump invalid_stage_dump;
   CHECK(vl_mpeg12_dump_init(&invalid_stage_dump, root) == 0,
         "the invalid-stage session is created");
   const char *saved_stage_name = fixture.stages[0].name;
   fixture.stages[0].name = "../escape";
   CHECK(dump_next_frame(&invalid_stage_dump, &fixture.pipe,
                         fixture.stages, FIXTURE_STAGE_COUNT) == -EINVAL,
         "a stage name cannot escape the exclusive frame directory");
   CHECK(invalid_stage_dump.frame == 1,
         "rejected path data consumes its decoded-frame identity");
   fixture.stages[0].name = saved_stage_name;
   remove_session(&invalid_stage_dump);

   struct vl_mpeg12_dump swapped_dump;
   CHECK(vl_mpeg12_dump_init(&swapped_dump, root) == 0,
         "the plane-mutation session is created");
   struct pipe_sampler_view *saved_plane = fixture.plane_views[1][0];
   fixture.plane_views[1][0] = fixture.plane_views[1][1];
   fixture.plane_views[1][1] = saved_plane;
   CHECK(dump_next_frame(&swapped_dump, &fixture.pipe, fixture.stages,
                         FIXTURE_STAGE_COUNT) == 0,
         "the swapped-plane specimen is captured");
   CHECK(!payload_matches(&swapped_dump, &fixture, 0, 1, 0),
         "the independent payload oracle rejects a swapped plane");
   remove_frame(&swapped_dump, &fixture, 0);
   remove_session(&swapped_dump);
   saved_plane = fixture.plane_views[1][0];
   fixture.plane_views[1][0] = fixture.plane_views[1][1];
   fixture.plane_views[1][1] = saved_plane;

   char corrupt_path[2048];
   CHECK(payload_path(corrupt_path, sizeof(corrupt_path), &first, 0,
                      &fixture.stages[0], 0),
         "the corruption specimen path is representable");
   FILE *corrupt = fopen(corrupt_path, "ab");
   CHECK(corrupt && fputc(0xee, corrupt) != EOF && fclose(corrupt) == 0,
         "the known-bad stride-padding byte is appended");
   CHECK(!payload_matches(&first, &fixture, 0, 0, 0),
         "the independent payload oracle rejects trailing stride padding");

   for (unsigned stage = 0; stage < FIXTURE_STAGE_COUNT; ++stage) {
      for (unsigned plane = 0; plane < FIXTURE_PLANE_COUNT; ++plane) {
         CHECK(fixture.resources[stage][plane].map_count ==
                  fixture.resources[stage][plane].unmap_count,
               "every successful map is paired with one unmap");
      }
   }

   remove_frame(&first, &fixture, 0);
   remove_session(&first);
   remove_frame(&second, &fixture, 0);
   remove_session(&second);
   remove_frame(&short_dump, &fixture, 0);
   remove_session(&short_dump);
   CHECK(vl_mpeg12_dump_default_io()->remove_directory(root) == 0,
         "the test root contains no residue");

   if (failures)
      return 1;

   puts("vl_mpeg12 dump transaction and calibration PASS");
   return 0;
}
