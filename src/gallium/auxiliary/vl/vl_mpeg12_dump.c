/*
 * SPDX-License-Identifier: MIT
 */

#include "vl_mpeg12_dump.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_video_codec.h"

#include "util/box.h"
#include "util/detect_os.h"
#include "util/format/u_format.h"
#include "util/ralloc.h"
#include "util/u_math.h"

#if DETECT_OS_WINDOWS
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
#include <bcrypt.h>
#define VL_MPEG12_DUMP_PATH_SEPARATOR "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define VL_MPEG12_DUMP_PATH_SEPARATOR "/"
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
   case ERROR_INVALID_DRIVE:
      return -ENOENT;
   case ERROR_ACCESS_DENIED:
   case ERROR_SHARING_VIOLATION:
      return -EACCES;
   case ERROR_DISK_FULL:
      return -ENOSPC;
   case ERROR_INVALID_NAME:
   case ERROR_INVALID_PARAMETER:
   case ERROR_NO_UNICODE_TRANSLATION:
      return -EINVAL;
   case ERROR_FILENAME_EXCED_RANGE:
   case ERROR_INSUFFICIENT_BUFFER:
      return -ENAMETOOLONG;
   case ERROR_NOT_ENOUGH_MEMORY:
   case ERROR_OUTOFMEMORY:
      return -ENOMEM;
   case ERROR_NOT_SUPPORTED:
      return -ENOTSUP;
   default:
      return -EIO;
   }
}

static bool
windows_ascii_equal_case_insensitive(wchar_t left, wchar_t right)
{
   if (left >= L'a' && left <= L'z')
      left -= L'a' - L'A';
   if (right >= L'a' && right <= L'z')
      right -= L'a' - L'A';
   return left == right;
}

static bool
windows_prefix_equal_case_insensitive(const wchar_t *path,
                                      const wchar_t *prefix)
{
   while (*prefix) {
      if (!*path || !windows_ascii_equal_case_insensitive(*path, *prefix))
         return false;
      ++path;
      ++prefix;
   }
   return true;
}

enum windows_path_kind {
   WINDOWS_PATH_INVALID,
   WINDOWS_PATH_RELATIVE,
   WINDOWS_PATH_DRIVE,
   WINDOWS_PATH_UNC,
   WINDOWS_PATH_EXTENDED_DRIVE,
   WINDOWS_PATH_EXTENDED_UNC,
};

static bool
windows_component_is_dot_reference(const wchar_t *start, const wchar_t *end)
{
   size_t length = (size_t)(end - start);
   return (length == 1 && start[0] == L'.') ||
          (length == 2 && start[0] == L'.' && start[1] == L'.');
}

static bool
windows_drive_path_is_absolute(const wchar_t *path)
{
   bool drive_letter = (path[0] >= L'A' && path[0] <= L'Z') ||
                       (path[0] >= L'a' && path[0] <= L'z');
   return drive_letter && path[1] == L':' && path[2] == L'\\';
}

static const wchar_t *
windows_unc_root_end(const wchar_t *components)
{
   const wchar_t *server_end = wcschr(components, L'\\');
   if (!components[0] || !server_end || server_end == components ||
       windows_component_is_dot_reference(components, server_end))
      return NULL;

   const wchar_t *share = server_end + 1;
   if (!share[0] || share[0] == L'\\')
      return NULL;

   const wchar_t *share_end = wcschr(share, L'\\');
   if (!share_end)
      share_end = share + wcslen(share);
   if (windows_component_is_dot_reference(share, share_end))
      return NULL;
   return share_end;
}

static enum windows_path_kind
windows_extended_path_kind(const wchar_t *path)
{
   if (wcsncmp(path, L"\\\\?\\", 4) != 0)
      return WINDOWS_PATH_INVALID;

   const wchar_t *components = path + 4;
   if (windows_drive_path_is_absolute(components))
      return WINDOWS_PATH_EXTENDED_DRIVE;

   if (windows_prefix_equal_case_insensitive(components, L"UNC\\") &&
       windows_unc_root_end(components + 4))
      return WINDOWS_PATH_EXTENDED_UNC;

   return WINDOWS_PATH_INVALID;
}

static enum windows_path_kind
windows_standard_path_kind(const wchar_t *path)
{
   if (windows_drive_path_is_absolute(path))
      return WINDOWS_PATH_DRIVE;

   if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' &&
       path[2] != L'.' && windows_unc_root_end(path + 2))
      return WINDOWS_PATH_UNC;

   return WINDOWS_PATH_INVALID;
}

static enum windows_path_kind
windows_input_path_kind(const wchar_t *path)
{
   if (!path[0])
      return WINDOWS_PATH_INVALID;
   if (wcsncmp(path, L"\\\\?\\", 4) == 0)
      return windows_extended_path_kind(path);
   if (wcsncmp(path, L"\\\\.\\", 4) == 0 ||
       wcsncmp(path, L"\\??\\", 4) == 0)
      return WINDOWS_PATH_INVALID;
   if (path[0] == L'\\' && path[1] == L'\\')
      return windows_standard_path_kind(path);
   if (path[0] && path[1] == L':')
      return windows_drive_path_is_absolute(path) ? WINDOWS_PATH_DRIVE
                                                  : WINDOWS_PATH_INVALID;
   if (path[0] == L'\\')
      return WINDOWS_PATH_INVALID;
   return WINDOWS_PATH_RELATIVE;
}

static const wchar_t *
windows_path_components(const wchar_t *path, enum windows_path_kind kind)
{
   switch (kind) {
   case WINDOWS_PATH_DRIVE:
      return path + 3;
   case WINDOWS_PATH_EXTENDED_DRIVE:
      return path + 7;
   case WINDOWS_PATH_UNC: {
      const wchar_t *root_end = windows_unc_root_end(path + 2);
      return root_end && *root_end == L'\\' ? root_end + 1 : root_end;
   }
   case WINDOWS_PATH_EXTENDED_UNC: {
      const wchar_t *root_end = windows_unc_root_end(path + 8);
      return root_end && *root_end == L'\\' ? root_end + 1 : root_end;
   }
   default:
      return NULL;
   }
}

static size_t
windows_path_root_length(const wchar_t *path, enum windows_path_kind kind)
{
   switch (kind) {
   case WINDOWS_PATH_DRIVE:
      return 3;
   case WINDOWS_PATH_EXTENDED_DRIVE:
      return 7;
   case WINDOWS_PATH_UNC: {
      const wchar_t *root_end = windows_unc_root_end(path + 2);
      return root_end ? (size_t)(root_end - path) : 0;
   }
   case WINDOWS_PATH_EXTENDED_UNC: {
      const wchar_t *root_end = windows_unc_root_end(path + 8);
      return root_end ? (size_t)(root_end - path) : 0;
   }
   default:
      return 0;
   }
}

static void
windows_trim_trailing_separators(wchar_t *path, enum windows_path_kind kind)
{
   size_t root_length = windows_path_root_length(path, kind);
   size_t length = wcslen(path);
   while (length > root_length && path[length - 1] == L'\\')
      path[--length] = L'\0';
}

static bool
windows_path_is_canonical(const wchar_t *path, enum windows_path_kind kind)
{
   enum windows_path_kind actual_kind =
      kind == WINDOWS_PATH_EXTENDED_DRIVE || kind == WINDOWS_PATH_EXTENDED_UNC
         ? windows_extended_path_kind(path)
         : windows_standard_path_kind(path);
   if (actual_kind != kind)
      return false;

   const wchar_t *component = windows_path_components(path, kind);
   if (!component)
      return false;

   while (*component) {
      const wchar_t *component_end = wcschr(component, L'\\');
      if (!component_end)
         component_end = component + wcslen(component);
      if (component_end == component ||
          windows_component_is_dot_reference(component, component_end))
         return false;
      component = *component_end ? component_end + 1 : component_end;
   }
   return true;
}

static wchar_t *
windows_get_full_path(const wchar_t *path)
{
   DWORD required_count = GetFullPathNameW(path, 0, NULL, NULL);
   if (!required_count) {
      int result = windows_error();
      errno = -result;
      return NULL;
   }

   for (;;) {
      if (required_count == UINT32_MAX ||
          (size_t)required_count + 1 > SIZE_MAX / sizeof(wchar_t)) {
         errno = ENAMETOOLONG;
         return NULL;
      }
      size_t capacity = (size_t)required_count + 1;
      wchar_t *absolute = ralloc_array(NULL, wchar_t, capacity);
      if (!absolute) {
         errno = ENOMEM;
         return NULL;
      }

      DWORD absolute_length =
         GetFullPathNameW(path, (DWORD)capacity, absolute, NULL);
      if (!absolute_length) {
         int result = windows_error();
         ralloc_free(absolute);
         errno = -result;
         return NULL;
      }
      if ((size_t)absolute_length < capacity)
         return absolute;

      ralloc_free(absolute);
      required_count = absolute_length;
   }
}

static wchar_t *
windows_path_from_utf8(const char *path)
{
   if (!path || !path[0]) {
      errno = EINVAL;
      return NULL;
   }

   int converted_count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
   if (!converted_count) {
      errno = EINVAL;
      return NULL;
   }

   if ((size_t)converted_count > SIZE_MAX / sizeof(wchar_t)) {
      errno = ENAMETOOLONG;
      return NULL;
   }
   wchar_t *converted = ralloc_array(NULL, wchar_t,
                                     (size_t)converted_count);
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

   for (wchar_t *character = converted; *character; ++character) {
      if (*character == L'/')
         *character = L'\\';
   }

   enum windows_path_kind input_kind = windows_input_path_kind(converted);
   if (input_kind == WINDOWS_PATH_INVALID) {
      ralloc_free(converted);
      errno = EINVAL;
      return NULL;
   }

   wchar_t *absolute = windows_get_full_path(converted);
   ralloc_free(converted);
   if (!absolute)
      return NULL;

   if (input_kind == WINDOWS_PATH_EXTENDED_DRIVE &&
       wcsncmp(absolute, L"\\\\?\\", 4) == 0 &&
       ((absolute[4] >= L'A' && absolute[4] <= L'Z') ||
        (absolute[4] >= L'a' && absolute[4] <= L'z')) &&
       absolute[5] == L':' && absolute[6] == L'\0') {
      absolute[6] = L'\\';
      absolute[7] = L'\0';
   }

   bool input_is_extended = input_kind == WINDOWS_PATH_EXTENDED_DRIVE ||
                            input_kind == WINDOWS_PATH_EXTENDED_UNC;
   enum windows_path_kind absolute_kind = input_is_extended
      ? windows_extended_path_kind(absolute)
      : windows_standard_path_kind(absolute);
   bool absolute_preserves_explicit_root =
      input_kind == WINDOWS_PATH_RELATIVE || absolute_kind == input_kind;
   if (absolute_kind == WINDOWS_PATH_INVALID ||
       !absolute_preserves_explicit_root) {
      ralloc_free(absolute);
      errno = EINVAL;
      return NULL;
   }

   windows_trim_trailing_separators(absolute, absolute_kind);
   if (absolute_kind == WINDOWS_PATH_EXTENDED_UNC) {
      absolute[4] = L'U';
      absolute[5] = L'N';
      absolute[6] = L'C';
   }
   if (!windows_path_is_canonical(absolute, absolute_kind)) {
      ralloc_free(absolute);
      errno = EINVAL;
      return NULL;
   }

   if (input_is_extended)
      return absolute;

   bool unc = absolute_kind == WINDOWS_PATH_UNC;
   const wchar_t *prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
   size_t prefix_count = unc ? 8 : 4;
   size_t source_skip = unc ? 2 : 0;
   size_t source_count = wcslen(absolute + source_skip) + 1;
   if (source_count > SIZE_MAX - prefix_count ||
       prefix_count + source_count > SIZE_MAX / sizeof(wchar_t)) {
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
   enum windows_path_kind extended_kind = unc ? WINDOWS_PATH_EXTENDED_UNC
                                               : WINDOWS_PATH_EXTENDED_DRIVE;
   if (!windows_path_is_canonical(extended_path, extended_kind)) {
      ralloc_free(extended_path);
      errno = EINVAL;
      return NULL;
   }
   return extended_path;
}
#endif

static char *
default_absolute_path(const char *path)
{
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return NULL;

   int utf8_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        wide_path, -1, NULL, 0, NULL, NULL);
   if (!utf8_count) {
      ralloc_free(wide_path);
      errno = EINVAL;
      return NULL;
   }

   char *absolute_path = malloc((size_t)utf8_count);
   if (!absolute_path) {
      ralloc_free(wide_path);
      errno = ENOMEM;
      return NULL;
   }
   int written_count = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, wide_path, -1, absolute_path, utf8_count,
      NULL, NULL);
   if (written_count != utf8_count) {
      ralloc_free(wide_path);
      free(absolute_path);
      errno = written_count ? EIO : EINVAL;
      return NULL;
   }
   ralloc_free(wide_path);
   return absolute_path;
#else
   char *resolved_path = realpath(path, NULL);
   if (!resolved_path)
      return NULL;
   return resolved_path;
#endif
}

static int
default_random_bytes(void *data, size_t size)
{
   if (!data && size)
      return -EINVAL;

#if DETECT_OS_WINDOWS
   if (size > ULONG_MAX)
      return -EOVERFLOW;
   return BCryptGenRandom(NULL, data, (ULONG)size,
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0
      ? 0
      : -EIO;
#else
   int flags = O_RDONLY;
#ifdef O_CLOEXEC
   flags |= O_CLOEXEC;
#endif
   errno = 0;
   int descriptor = open("/dev/urandom", flags);
   if (descriptor < 0)
      return io_error();

   uint8_t *bytes = data;
   size_t offset = 0;
   int result = 0;
   while (offset < size) {
      ssize_t count = read(descriptor, bytes + offset, size - offset);
      if (count < 0 && errno == EINTR)
         continue;
      if (count <= 0) {
         result = count < 0 ? io_error() : -EIO;
         break;
      }
      offset += (size_t)count;
   }

   errno = 0;
   if (close(descriptor) != 0 && !result)
      result = io_error();
   return result;
#endif
}

static int
default_open_unique(const char *path, int mode, FILE **stream_out,
                    bool *created_out)
{
   if (!path || !stream_out || !created_out)
      return -EINVAL;

   *stream_out = NULL;
   *created_out = false;
   errno = 0;
#if DETECT_OS_WINDOWS
   wchar_t *wide_path = windows_path_from_utf8(path);
   if (!wide_path)
      return io_error();
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
      int result = io_error();
#if DETECT_OS_WINDOWS
      ralloc_free(wide_path);
#endif
      return result;
   }
   *created_out = true;

#if DETECT_OS_WINDOWS
   FILE *stream = _fdopen(descriptor, "wb");
#else
   FILE *stream = fdopen(descriptor, "wb");
#endif
   if (!stream) {
      int result = io_error();
      errno = 0;
#if DETECT_OS_WINDOWS
      if (_close(descriptor) != 0)
#else
      if (close(descriptor) != 0)
#endif
         result = io_error();
#if DETECT_OS_WINDOWS
      ralloc_free(wide_path);
#endif
      return result;
   }
#if DETECT_OS_WINDOWS
   ralloc_free(wide_path);
#endif
   *stream_out = stream;
   return 0;
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
   .absolute_path = default_absolute_path,
   .random_bytes = default_random_bytes,
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
   return io && io->absolute_path && io->random_bytes && io->open_unique &&
          io->write && io->sync_file && io->close && io->remove_file &&
          io->remove_directory && io->mkdir && io->sync_directory;
}

static const char *
path_separator_after(const char *path)
{
   size_t length = strlen(path);
   bool trailing_separator =
      length && path[length - 1] == VL_MPEG12_DUMP_PATH_SEPARATOR[0];
#if DETECT_OS_WINDOWS
   trailing_separator = trailing_separator ||
                        (length && path[length - 1] == '/');
#endif
   return trailing_separator ? "" : VL_MPEG12_DUMP_PATH_SEPARATOR;
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

   errno = 0;
   char *absolute_root = dump->io.absolute_path(root_path);
   if (!absolute_root)
      return io_error();

   for (unsigned attempt = 0; attempt < VL_MPEG12_DUMP_SESSION_ATTEMPTS;
        ++attempt) {
      uint8_t identity[16];
      int result = dump->io.random_bytes(identity, sizeof(identity));
      if (result) {
         free(absolute_root);
         return result < 0 ? result : -EIO;
      }

      static const char hex[] = "0123456789abcdef";
      char identity_hex[sizeof(identity) * 2 + 1];
      for (unsigned index = 0; index < sizeof(identity); ++index) {
         identity_hex[index * 2] = hex[identity[index] >> 4];
         identity_hex[index * 2 + 1] = hex[identity[index] & 0xf];
      }
      identity_hex[sizeof(identity_hex) - 1] = '\0';

      char *session_path = ralloc_asprintf(
         NULL, "%s%smpeg12-dump-session-%s", absolute_root,
         path_separator_after(absolute_root), identity_hex);
      if (!session_path)
         result = -ENOMEM;
      else
         result = dump->io.mkdir(session_path, 0700);

      if (result == 0) {
         result = dump->io.sync_directory(absolute_root);
         if (!result) {
            dump->session_path = session_path;
            free(absolute_root);
            return 0;
         }

         int cleanup_result = dump->io.remove_directory(session_path);
         if (!cleanup_result)
            cleanup_result = dump->io.sync_directory(absolute_root);
         ralloc_free(session_path);
         free(absolute_root);
         return cleanup_result ? cleanup_result : result;
      }

      ralloc_free(session_path);
      if (result != -EEXIST) {
         free(absolute_root);
         return result;
      }
   }

   free(absolute_root);
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
vl_mpeg12_dump_stage_from_video_buffer(struct vl_mpeg12_dump_stage *stage,
                                       const char *name,
                                       struct pipe_video_buffer *buffer)
{
   if (!stage || !name || !buffer || !buffer->get_sampler_view_planes ||
       buffer->buffer_format == PIPE_FORMAT_NONE)
      return -EINVAL;

   unsigned plane_count = util_format_get_num_planes(buffer->buffer_format);
   struct pipe_sampler_view **planes =
      buffer->get_sampler_view_planes(buffer);
   if (!planes || !plane_count || plane_count > VL_MPEG12_DUMP_MAX_PLANES)
      return -EIO;

   stage->name = name;
   stage->buffer_format = buffer->buffer_format;
   stage->planes = planes;
   stage->plane_count = plane_count;
   return 0;
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
      entry->path = ralloc_asprintf(
         NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "%s", payload_path,
         entry->filename);
   if (!entry->filename || !entry->path) {
      pipe->texture_unmap(pipe, transfer);
      free_entry(entry);
      return -ENOMEM;
   }

   FILE *stream = NULL;
   int result = dump->io.open_unique(entry->path, 0600, &stream,
                                     &entry->created);
   if (result) {
      pipe->texture_unmap(pipe, transfer);
      return result;
   }
   if (!stream || !entry->created) {
      if (stream)
         dump->io.close(stream);
      pipe->texture_unmap(pipe, transfer);
      return -EIO;
   }

   result = 0;
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
   char *manifest_path = ralloc_asprintf(
      NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "manifest.tsv", payload_path);
   if (!manifest_path)
      return -ENOMEM;

   FILE *stream = NULL;
   int result = dump->io.open_unique(manifest_path, 0600, &stream,
                                     manifest_created_out);
   if (result) {
      ralloc_free(manifest_path);
      return result;
   }
   if (!stream || !*manifest_created_out) {
      if (stream)
         dump->io.close(stream);
      ralloc_free(manifest_path);
      return -EIO;
   }

   static const char header[] =
      "stage\tbuffer_format\tstorage_format\tview_format\tstorage_plane\t"
      "level\tfirst_layer\tlast_layer\t"
      "width\theight\tlayer_count\trow_bytes\trows_per_layer\t"
      "total_bytes\tfile\n";
   result = write_all(&dump->io, stream, header, sizeof(header) - 1);

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
            ralloc_asprintf(NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR
                                  "manifest.tsv", payload_path);
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

         char *path = ralloc_asprintf(
            NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "%s", payload_path,
            filename);
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
      NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "frame-%020" PRIu64,
      dump->session_path, frame);
   char *payload_path = ralloc_asprintf(
      NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "payload", frame_path);
   char *complete_path = ralloc_asprintf(
      NULL, "%s" VL_MPEG12_DUMP_PATH_SEPARATOR "complete", frame_path);
   if (!frame_path || !payload_path || !complete_path) {
      ralloc_free(frame_path);
      ralloc_free(payload_path);
      ralloc_free(complete_path);
      return -ENOMEM;
   }

   bool frame_created = false;
   bool payload_created = false;
   bool complete_created = false;
   bool publication_collision = false;

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

   publication_collision = result == -EEXIST;

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
      publication_collision = result == -EEXIST;
   }
   if (!result)
      complete_created = true;
   if (!result)
      result = dump->io.sync_directory(frame_path);

   if (result && !complete_created && !publication_collision) {
      int cleanup_result = cleanup_frame(
         dump, frame_path, payload_path, entries, entry_count, manifest_created,
         frame_created, payload_created);
      if (cleanup_result) {
         result = cleanup_result;
         *frame_state_out = VL_MPEG12_DUMP_FRAME_RETAINED;
      }
   } else if (complete_created) {
      *frame_state_out = VL_MPEG12_DUMP_FRAME_ADMITTED;
   } else if (publication_collision) {
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
