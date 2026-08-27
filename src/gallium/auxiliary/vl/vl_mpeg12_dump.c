/*
 * SPDX-License-Identifier: MIT
 */

#include "vl_mpeg12_dump.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_state.h"

#include "util/box.h"
#include "util/detect_os.h"
#include "util/format/u_format.h"
#include "util/ralloc.h"
#include "util/u_atomic.h"
#include "util/u_math.h"

#if DETECT_OS_WINDOWS
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

struct vl_mpeg12_dump_entry {
   const char *stage;
   enum pipe_format buffer_format;
   enum pipe_format storage_format;
   enum pipe_format view_format;
   unsigned storage_plane;
   unsigned level;
   unsigned first_layer;
   unsigned last_layer;
   unsigned width;
   unsigned height;
   unsigned layer_count;
   size_t row_bytes;
   unsigned rows_per_layer;
   uint64_t total_bytes;
   char *filename;
   char *path;
   bool created;
};

static uint32_t dump_session_serial;

static int
io_error(void)
{
   return errno ? -errno : -EIO;
}

#if DETECT_OS_WINDOWS
static int
windows_error(void)
{
   switch (GetLastError()) {
   case ERROR_FILE_EXISTS:
   case ERROR_ALREADY_EXISTS:
      return -EEXIST;
   case ERROR_FILE_NOT_FOUND:
   case ERROR_PATH_NOT_FOUND:
      return -ENOENT;
   case ERROR_ACCESS_DENIED:
   case ERROR_SHARING_VIOLATION:
      return -EACCES;
   case ERROR_DISK_FULL:
      return -ENOSPC;
   case ERROR_INVALID_NAME:
   case ERROR_NO_UNICODE_TRANSLATION:
      return -EINVAL;
   case ERROR_FILENAME_EXCED_RANGE:
      return -ENAMETOOLONG;
   case ERROR_NOT_SUPPORTED:
      return -ENOTSUP;
   default:
      return -EIO;
   }
}

static wchar_t *
windows_path_from_utf8(const char *path)
{
   int converted_count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
   if (!converted_count) {
      errno = EINVAL;
      return NULL;
   }

   wchar_t *converted =
      ralloc_array(NULL, wchar_t, (size_t)converted_count);
   if (!converted) {
      errno = ENOMEM;
      return NULL;
   }
   if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            converted, converted_count)) {
      ralloc_free(converted);
      errno = EINVAL;
      return NULL;
   }

   bool extended = converted_count > 4 && converted[0] == L'\\' &&
                   converted[1] == L'\\' && converted[2] == L'?' &&
                   converted[3] == L'\\';
   wchar_t *absolute = converted;
   if (!extended) {
      DWORD absolute_count = GetFullPathNameW(converted, 0, NULL, NULL);
      if (!absolute_count) {
         int result = windows_error();
         ralloc_free(converted);
         errno = -result;
         return NULL;
      }

      absolute = ralloc_array(NULL, wchar_t, (size_t)absolute_count);
      if (!absolute) {
         ralloc_free(converted);
         errno = ENOMEM;
         return NULL;
      }
      DWORD absolute_length =
         GetFullPathNameW(converted, absolute_count, absolute, NULL);
      ralloc_free(converted);
      if (!absolute_length || absolute_length >= absolute_count) {
         int result = windows_error();
         ralloc_free(absolute);
         errno = -result;
         return NULL;
      }
   }

   if (extended)
      return absolute;

   bool unc = absolute[0] == L'\\' && absolute[1] == L'\\';
   const wchar_t *prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
   size_t prefix_count = unc ? 8 : 4;
   size_t source_skip = unc ? 2 : 0;
   size_t source_count = wcslen(absolute + source_skip) + 1;
   if (source_count > SIZE_MAX - prefix_count) {
      ralloc_free(absolute);
      errno = ENAMETOOLONG;
      return NULL;
   }

   wchar_t *extended_path =
      ralloc_array(NULL, wchar_t, prefix_count + source_count);
   if (!extended_path) {
      ralloc_free(absolute);
      errno = ENOMEM;
      return NULL;
   }
   memcpy(extended_path, prefix, prefix_count * sizeof(*extended_path));
   memcpy(extended_path + prefix_count, absolute + source_skip,
          source_count * sizeof(*extended_path));
   ralloc_free(absolute);
   return extended_path;
}
#endif

static FILE *
default_open_unique(const char *path, int mode)
{
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return NULL;
   int descriptor = _wopen(wide_path,
                           _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY |
                              _O_NOINHERIT,
                           mode);
#else
   int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
   int descriptor = open(path, flags, mode);
#endif
   if (descriptor < 0) {
#if DETECT_OS_WINDOWS
      ralloc_free(wide_path);
#endif
      return NULL;
   }

#if DETECT_OS_WINDOWS
   FILE *stream = _fdopen(descriptor, "wb");
#else
   FILE *stream = fdopen(descriptor, "wb");
#endif
   if (!stream) {
      int saved_errno = errno;
#if DETECT_OS_WINDOWS
      _close(descriptor);
      _wremove(wide_path);
#else
      close(descriptor);
      remove(path);
#endif
      errno = saved_errno;
   }
#if DETECT_OS_WINDOWS
   ralloc_free(wide_path);
#endif
   return stream;
}

static int
default_sync_file(FILE *stream)
{
   errno = 0;
   if (fflush(stream) != 0)
      return io_error();

#if DETECT_OS_WINDOWS
   if (_commit(_fileno(stream)) != 0)
#else
   if (fsync(fileno(stream)) != 0)
#endif
      return io_error();
   return 0;
}

static int
default_remove_file(const char *path)
{
   errno = 0;
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return io_error();
   int result = _wremove(wide_path) == 0 ? 0 : io_error();
   ralloc_free(wide_path);
   return result;
#else
   return remove(path) == 0 ? 0 : io_error();
#endif
}

static int
default_remove_directory(const char *path)
{
   errno = 0;
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return io_error();
   int result = _wrmdir(wide_path) == 0 ? 0 : io_error();
   ralloc_free(wide_path);
   return result;
#else
   return rmdir(path) == 0 ? 0 : io_error();
#endif
}

static int
default_mkdir(const char *path, int mode)
{
   errno = 0;
#if DETECT_OS_WINDOWS
   (void)mode;
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return io_error();
   int result = _wmkdir(wide_path) == 0 ? 0 : io_error();
   ralloc_free(wide_path);
   return result;
#else
   return mkdir(path, mode) == 0 ? 0 : io_error();
#endif
}

static int
default_sync_directory(const char *path)
{
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return io_error();
   HANDLE directory =
      CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
   ralloc_free(wide_path);
   if (directory == INVALID_HANDLE_VALUE)
      return windows_error();

   int result = FlushFileBuffers(directory) ? 0 : windows_error();
   if (!CloseHandle(directory) && !result)
      result = windows_error();
   return result;
#else
   int flags = O_RDONLY;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
   flags |= O_DIRECTORY;
#endif
   errno = 0;
   int descriptor = open(path, flags);
   if (descriptor < 0)
      return io_error();

   int result = fsync(descriptor) == 0 ? 0 : io_error();
   errno = 0;
   if (close(descriptor) != 0 && !result)
      result = io_error();
   return result;
#endif
}

static const struct vl_mpeg12_dump_io default_io = {
   .open_unique = default_open_unique,
   .write = fwrite,
   .sync_file = default_sync_file,
   .close = fclose,
   .remove_file = default_remove_file,
   .remove_directory = default_remove_directory,
   .mkdir = default_mkdir,
   .sync_directory = default_sync_directory,
};

const struct vl_mpeg12_dump_io *
vl_mpeg12_dump_default_io(void)
{
   return &default_io;
}

static bool
path_component_is_valid(const char *component)
{
   if (!component || !component[0])
      return false;

   for (const unsigned char *character = (const unsigned char *)component;
        *character; ++character) {
      if (!isalnum(*character) && *character != '-' && *character != '_')
         return false;
   }
   return true;
}

static bool
io_is_complete(const struct vl_mpeg12_dump_io *io)
{
   return io && io->open_unique && io->write && io->sync_file && io->close &&
          io->remove_file && io->remove_directory && io->mkdir &&
          io->sync_directory;
}

int
vl_mpeg12_dump_init_with_io(struct vl_mpeg12_dump *dump,
                            const char *root_path,
                            const struct vl_mpeg12_dump_io *io)
{
   if (!dump || !io_is_complete(io))
      return -EINVAL;

   memset(dump, 0, sizeof(*dump));
   dump->io = *io;

   if (!root_path || !root_path[0])
      return 0;

   for (unsigned attempt = 0; attempt < 1024; ++attempt) {
      uint32_t serial = p_atomic_inc_return(&dump_session_serial);
      char *session_path = ralloc_asprintf(
         NULL, "%s/mpeg12-dump-session-%08" PRIu32, root_path, serial);
      if (!session_path)
         return -ENOMEM;

      int result = dump->io.mkdir(session_path, 0700);
      if (result == 0) {
         result = dump->io.sync_directory(root_path);
         if (!result) {
            dump->session_path = session_path;
            return 0;
         }

         int cleanup_result = dump->io.remove_directory(session_path);
         if (!cleanup_result)
            cleanup_result = dump->io.sync_directory(root_path);
         ralloc_free(session_path);
         return cleanup_result ? cleanup_result : result;
      }

      ralloc_free(session_path);
      if (result != -EEXIST)
         return result;
   }

   return -EEXIST;
}

int
vl_mpeg12_dump_init(struct vl_mpeg12_dump *dump, const char *root_path)
{
   return vl_mpeg12_dump_init_with_io(dump, root_path, &default_io);
}

void
vl_mpeg12_dump_cleanup(struct vl_mpeg12_dump *dump)
{
   if (!dump)
      return;

   ralloc_free(dump->session_path);
   memset(dump, 0, sizeof(*dump));
}

bool
vl_mpeg12_dump_enabled(const struct vl_mpeg12_dump *dump)
{
   return dump && dump->session_path;
}

int
vl_mpeg12_dump_reserve_frame(struct vl_mpeg12_dump *dump,
                             uint64_t *frame_out)
{
   if (!vl_mpeg12_dump_enabled(dump) || !frame_out)
      return -EINVAL;
   if (dump->frame == UINT64_MAX)
      return -EOVERFLOW;

   *frame_out = dump->frame++;
   return 0;
}

static int
write_all(const struct vl_mpeg12_dump_io *io, FILE *stream,
          const void *data, size_t size)
{
   const uint8_t *bytes = data;
   size_t offset = 0;

   while (offset < size) {
      errno = 0;
      size_t written = io->write(bytes + offset, 1, size - offset, stream);
      if (!written)
         return io_error();
      if (written > size - offset)
         return -EIO;
      offset += written;
   }

   return 0;
}

static void
free_entry(struct vl_mpeg12_dump_entry *entry)
{
   ralloc_free(entry->path);
   ralloc_free(entry->filename);
   memset(entry, 0, sizeof(*entry));
}

int
vl_mpeg12_dump_validate_source_span(unsigned layer_count,
                                    uint64_t layer_stride,
                                    unsigned rows_per_layer,
                                    uint64_t row_stride,
                                    size_t row_bytes)
{
   if (!layer_count || !rows_per_layer)
      return -EINVAL;

   uint64_t last_layer = layer_count - 1;
   uint64_t last_row = rows_per_layer - 1;
   if ((last_layer && layer_stride > UINT64_MAX / last_layer) ||
       (last_row && row_stride > UINT64_MAX / last_row))
      return -EOVERFLOW;

   uint64_t last_layer_offset = last_layer * layer_stride;
   uint64_t last_row_offset = last_row * row_stride;
   if (last_layer_offset > UINT64_MAX - last_row_offset ||
       last_layer_offset + last_row_offset > UINT64_MAX - row_bytes ||
       last_layer_offset + last_row_offset + row_bytes > SIZE_MAX)
      return -EOVERFLOW;

   return 0;
}

static int
dump_plane(const struct vl_mpeg12_dump *dump,
           struct pipe_context *pipe,
           const char *payload_path,
           const struct vl_mpeg12_dump_stage *stage,
           unsigned storage_plane,
           struct vl_mpeg12_dump_entry *entry)
{
   struct pipe_sampler_view *view = stage->planes[storage_plane];
   if (!view || !view->texture || view->texture->target == PIPE_BUFFER ||
       view->format == PIPE_FORMAT_NONE)
      return -EINVAL;

   if (view->u.tex.first_level != view->u.tex.last_level ||
       view->u.tex.first_level > view->texture->last_level ||
       view->u.tex.last_layer < view->u.tex.first_layer)
      return -EINVAL;

   unsigned level = view->u.tex.first_level;
   unsigned width = u_minify(view->texture->width0, level);
   unsigned height = u_minify(view->texture->height0, level);
   unsigned available_layers = view->texture->target == PIPE_TEXTURE_3D
      ? u_minify(view->texture->depth0, level)
      : view->texture->array_size;
   unsigned first_layer = view->u.tex.first_layer;
   unsigned last_layer = view->u.tex.last_layer;
   unsigned layer_count = last_layer - first_layer + 1;

   if (!available_layers || last_layer >= available_layers ||
       width > INT_MAX || height > INT_MAX || first_layer > INT_MAX ||
       layer_count > INT_MAX)
      return -EINVAL;

   enum pipe_format storage_format = view->texture->format;
   unsigned block_size = util_format_get_blocksize(storage_format);
   uint64_t row_bytes_64 =
      (uint64_t)util_format_get_nblocksx(storage_format, width) * block_size;
   unsigned rows_per_layer = util_format_get_nblocksy(storage_format, height);
   if (!block_size || !rows_per_layer || row_bytes_64 > SIZE_MAX)
      return -EOVERFLOW;

   size_t row_bytes = (size_t)row_bytes_64;
   if (row_bytes > UINT64_MAX / rows_per_layer ||
       row_bytes * (uint64_t)rows_per_layer > UINT64_MAX / layer_count)
      return -EOVERFLOW;
   uint64_t total_bytes = row_bytes * (uint64_t)rows_per_layer * layer_count;

   struct pipe_box box;
   u_box_3d(0, 0, first_layer, width, height, layer_count, &box);

   struct pipe_transfer *transfer = NULL;
   uint8_t *map = pipe->texture_map(pipe, view->texture, level,
                                    PIPE_MAP_READ, &box, &transfer);
   if (!map || !transfer) {
      if (transfer)
         pipe->texture_unmap(pipe, transfer);
      return -EIO;
   }

   if (transfer->stride < row_bytes ||
       (layer_count > 1 &&
        transfer->layer_stride <
           (uint64_t)transfer->stride * rows_per_layer)) {
      pipe->texture_unmap(pipe, transfer);
      return -EIO;
   }

   int span_result = vl_mpeg12_dump_validate_source_span(
      layer_count, transfer->layer_stride, rows_per_layer, transfer->stride,
      row_bytes);
   if (span_result) {
      pipe->texture_unmap(pipe, transfer);
      return span_result;
   }

   const char *view_format_name = util_format_short_name(view->format);
   const char *storage_format_name = util_format_short_name(storage_format);
   const char *buffer_format_name = util_format_short_name(stage->buffer_format);
   if (!view_format_name || !storage_format_name || !buffer_format_name) {
      pipe->texture_unmap(pipe, transfer);
      return -EINVAL;
   }

   entry->filename = ralloc_asprintf(
      NULL,
      "%s_storage%u_level%u_layers%u-%u_%ux%ux%u_storage-%s_view-%s.raw",
      stage->name, storage_plane, level, first_layer, last_layer,
      width, height, layer_count, storage_format_name, view_format_name);
   if (entry->filename)
      entry->path = ralloc_asprintf(NULL, "%s/%s", payload_path,
                                    entry->filename);
   if (!entry->filename || !entry->path) {
      pipe->texture_unmap(pipe, transfer);
      free_entry(entry);
      return -ENOMEM;
   }

   errno = 0;
   FILE *stream = dump->io.open_unique(entry->path, 0600);
   if (!stream) {
      int result = io_error();
      pipe->texture_unmap(pipe, transfer);
      return result;
   }
   entry->created = true;

   int result = 0;
   for (unsigned layer = 0; !result && layer < layer_count; ++layer) {
      for (unsigned row = 0; !result && row < rows_per_layer; ++row) {
         const uint8_t *source = map +
            (size_t)layer * transfer->layer_stride +
            (size_t)row * transfer->stride;
         result = write_all(&dump->io, stream, source, row_bytes);
      }
   }

   if (!result)
      result = dump->io.sync_file(stream);

   errno = 0;
   int close_result = dump->io.close(stream);
   if (!result && close_result != 0)
      result = io_error();

   pipe->texture_unmap(pipe, transfer);

   if (result)
      return result;

   entry->stage = stage->name;
   entry->buffer_format = stage->buffer_format;
   entry->storage_format = storage_format;
   entry->view_format = view->format;
   entry->storage_plane = storage_plane;
   entry->level = level;
   entry->first_layer = first_layer;
   entry->last_layer = last_layer;
   entry->width = width;
   entry->height = height;
   entry->layer_count = layer_count;
   entry->row_bytes = row_bytes;
   entry->rows_per_layer = rows_per_layer;
   entry->total_bytes = total_bytes;
   return 0;
}

static int
write_manifest(const struct vl_mpeg12_dump *dump,
               const char *payload_path,
               const struct vl_mpeg12_dump_entry *entries,
               unsigned entry_count,
               char **manifest_path_out,
               bool *manifest_created_out)
{
   char *manifest_path = ralloc_asprintf(NULL, "%s/manifest.tsv", payload_path);
   if (!manifest_path)
      return -ENOMEM;

   errno = 0;
   FILE *stream = dump->io.open_unique(manifest_path, 0600);
   if (!stream) {
      int result = io_error();
      ralloc_free(manifest_path);
      return result;
   }
   *manifest_created_out = true;

   static const char header[] =
      "stage\tbuffer_format\tstorage_format\tview_format\tstorage_plane\t"
      "level\tfirst_layer\tlast_layer\t"
      "width\theight\tlayer_count\trow_bytes\trows_per_layer\t"
      "total_bytes\tfile\n";
   int result = write_all(&dump->io, stream, header, sizeof(header) - 1);

   for (unsigned index = 0; !result && index < entry_count; ++index) {
      const struct vl_mpeg12_dump_entry *entry = &entries[index];
      char *line = ralloc_asprintf(
         NULL,
         "%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%zu\t%u\t%" PRIu64
         "\t%s\n",
         entry->stage, util_format_short_name(entry->buffer_format),
         util_format_short_name(entry->storage_format),
         util_format_short_name(entry->view_format), entry->storage_plane,
         entry->level, entry->first_layer,
         entry->last_layer, entry->width, entry->height, entry->layer_count,
         entry->row_bytes, entry->rows_per_layer, entry->total_bytes,
         entry->filename);
      if (!line)
         result = -ENOMEM;
      else
         result = write_all(&dump->io, stream, line, strlen(line));
      ralloc_free(line);
   }

   if (!result)
      result = dump->io.sync_file(stream);

   errno = 0;
   int close_result = dump->io.close(stream);
   if (!result && close_result != 0)
      result = io_error();

   if (result) {
      ralloc_free(manifest_path);
      return result;
   }

   *manifest_path_out = manifest_path;
   return 0;
}

static void
record_cleanup_result(int *result, int candidate)
{
   if (!*result && candidate && candidate != -ENOENT)
      *result = candidate;
}

static int
cleanup_frame(const struct vl_mpeg12_dump *dump,
              const char *frame_path,
              const char *payload_path,
              const struct vl_mpeg12_dump_entry *entries,
              unsigned entry_count,
              bool manifest_created,
              bool frame_created,
              bool payload_created)
{
   if (!frame_created)
      return 0;

   if (payload_created) {
      int result = 0;

      if (manifest_created) {
         char *manifest_path =
            ralloc_asprintf(NULL, "%s/manifest.tsv", payload_path);
         if (!manifest_path)
            record_cleanup_result(&result, -ENOMEM);
         else
            record_cleanup_result(&result,
                                  dump->io.remove_file(manifest_path));
         ralloc_free(manifest_path);
      }

      for (unsigned index = entry_count; index > 0; --index) {
         const char *filename = entries[index - 1].filename;
         if (!filename || !entries[index - 1].created)
            continue;

         char *path = ralloc_asprintf(NULL, "%s/%s", payload_path, filename);
         if (!path)
            record_cleanup_result(&result, -ENOMEM);
         else
            record_cleanup_result(&result, dump->io.remove_file(path));
         ralloc_free(path);
      }

      if (result)
         return result;

      result = dump->io.sync_directory(payload_path);
      if (result)
         return result;
      result = dump->io.remove_directory(payload_path);
      if (result && result != -ENOENT)
         return result;
      result = dump->io.sync_directory(frame_path);
      if (result)
         return result;
   }

   int result = dump->io.remove_directory(frame_path);
   if (result && result != -ENOENT)
      return result;
   return dump->io.sync_directory(dump->session_path);
}

int
vl_mpeg12_dump_frame(struct vl_mpeg12_dump *dump,
                     uint64_t frame,
                     struct pipe_context *pipe,
                     const struct vl_mpeg12_dump_stage *stages,
                     unsigned stage_count,
                     enum vl_mpeg12_dump_frame_state *frame_state_out)
{
   if (!frame_state_out)
      return -EINVAL;
   *frame_state_out = VL_MPEG12_DUMP_FRAME_CLEANED;

   if (!dump || !pipe || !pipe->texture_map || !pipe->texture_unmap ||
       !stages || !vl_mpeg12_dump_enabled(dump) ||
       !io_is_complete(&dump->io) || !stage_count ||
       stage_count > VL_MPEG12_DUMP_MAX_STAGES)
      return -EINVAL;
   for (unsigned stage_index = 0; stage_index < stage_count; ++stage_index) {
      const struct vl_mpeg12_dump_stage *stage = &stages[stage_index];
      if (!path_component_is_valid(stage->name) || !stage->planes ||
          !stage->plane_count ||
          stage->plane_count > VL_MPEG12_DUMP_MAX_PLANES ||
          stage->buffer_format == PIPE_FORMAT_NONE)
         return -EINVAL;
   }

   char *frame_path = ralloc_asprintf(
      NULL, "%s/frame-%020" PRIu64, dump->session_path, frame);
   char *payload_path = ralloc_asprintf(NULL, "%s/payload", frame_path);
   char *complete_path = ralloc_asprintf(NULL, "%s/complete", frame_path);
   if (!frame_path || !payload_path || !complete_path) {
      ralloc_free(frame_path);
      ralloc_free(payload_path);
      ralloc_free(complete_path);
      return -ENOMEM;
   }

   bool frame_created = false;
   bool payload_created = false;
   bool complete_created = false;
   bool complete_collision = false;

   int result = dump->io.mkdir(frame_path, 0700);
   if (result) {
      ralloc_free(frame_path);
      ralloc_free(payload_path);
      ralloc_free(complete_path);
      return result;
   }
   frame_created = true;

   result = dump->io.mkdir(payload_path, 0700);
   if (!result)
      payload_created = true;

   struct vl_mpeg12_dump_entry
      entries[VL_MPEG12_DUMP_MAX_STAGES * VL_MPEG12_DUMP_MAX_PLANES] = {0};
   unsigned entry_count = 0;
   char *manifest_path = NULL;
   bool manifest_created = false;

   for (unsigned stage_index = 0; !result && stage_index < stage_count;
        ++stage_index) {
      const struct vl_mpeg12_dump_stage *stage = &stages[stage_index];
      for (unsigned storage_plane = 0;
           !result && storage_plane < stage->plane_count; ++storage_plane) {
         struct vl_mpeg12_dump_entry *entry = &entries[entry_count++];
         result = dump_plane(dump, pipe, payload_path, stage, storage_plane,
                             entry);
      }
   }

   if (!result)
      result = write_manifest(dump, payload_path, entries, entry_count,
                              &manifest_path, &manifest_created);

   if (!result)
      result = dump->io.sync_directory(payload_path);

   if (!result)
      result = dump->io.sync_directory(frame_path);
   if (!result)
      result = dump->io.sync_directory(dump->session_path);

   /* The completion directory is the observer admission point.  Every
    * fallible publication and parent-link synchronization precedes it.  A
    * frame-directory synchronization error after admission preserves the
    * admitted payload and reports that its crash durability remains uncertain. */
   if (!result) {
      result = dump->io.mkdir(complete_path, 0500);
      complete_collision = result == -EEXIST;
   }
   if (!result)
      complete_created = true;
   if (!result)
      result = dump->io.sync_directory(frame_path);

   if (result && !complete_created && !complete_collision) {
      int cleanup_result = cleanup_frame(
         dump, frame_path, payload_path, entries, entry_count, manifest_created,
         frame_created, payload_created);
      if (cleanup_result) {
         result = cleanup_result;
         *frame_state_out = VL_MPEG12_DUMP_FRAME_RETAINED;
      }
   } else if (complete_created) {
      *frame_state_out = VL_MPEG12_DUMP_FRAME_ADMITTED;
   } else if (complete_collision) {
      *frame_state_out = VL_MPEG12_DUMP_FRAME_RETAINED;
   }

   for (unsigned index = 0; index < entry_count; ++index)
      free_entry(&entries[index]);
   ralloc_free(manifest_path);
   ralloc_free(frame_path);
   ralloc_free(payload_path);
   ralloc_free(complete_path);
   return result;
}
