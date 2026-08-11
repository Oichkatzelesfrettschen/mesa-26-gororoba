/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V issuer identity and JSON string helpers.
 */

#include "r3v_native_identity.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#define R3V_NATIVE_MAP_LINE_SIZE 4096
#define R3V_NATIVE_MAP_PATH_SIZE 2048
#define R3V_NATIVE_HASH_CHUNK_SIZE 4096

struct r3v_native_map_entry {
   uintptr_t start;
   uintptr_t end;
   uint64_t offset;
   dev_t device;
   ino_t inode;
   char permissions[5];
   char path[R3V_NATIVE_MAP_PATH_SIZE];
};

static int
parse_map_line(const char *line, struct r3v_native_map_entry *entry)
{
   unsigned long long start;
   unsigned long long end;
   unsigned long long offset;
   unsigned long long inode;
   unsigned int major_number;
   unsigned int minor_number;
   char device[32];
   char path[R3V_NATIVE_MAP_PATH_SIZE];
   int fields;

   path[0] = '\0';
   fields = sscanf(line, "%llx-%llx %4s %llx %31s %llu %2047[^\n]",
                   &start, &end, entry->permissions, &offset, device, &inode,
                   path);
   if (fields < 6)
      return -ENOENT;
   if (sscanf(device, "%x:%x", &major_number, &minor_number) != 2)
      return -EINVAL;

   entry->start = (uintptr_t)start;
   entry->end = (uintptr_t)end;
   entry->offset = offset;
   entry->device = makedev(major_number, minor_number);
   entry->inode = (ino_t)inode;
   entry->path[0] = '\0';
   if (fields == 7) {
      char *name = path;
      while (*name == ' ')
         name++;
      if (strlen(name) >= sizeof(entry->path))
         return -ENAMETOOLONG;
      snprintf(entry->path, sizeof(entry->path), "%s", name);
   }
   return 0;
}

static bool
path_is_deleted(const char *path)
{
   static const char suffix[] = " (deleted)";
   size_t path_length = strlen(path);
   size_t suffix_length = sizeof(suffix) - 1;

   return path_length >= suffix_length &&
          strcmp(path + path_length - suffix_length, suffix) == 0;
}

static int
find_anchor_mapping(struct r3v_native_map_entry *entry)
{
   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return -errno;

   const uintptr_t anchor = (uintptr_t)(void *)&r3v_native_identity_collect;
   char line[R3V_NATIVE_MAP_LINE_SIZE];
   int result = -ENOENT;
   while (fgets(line, sizeof(line), maps) != NULL) {
      struct r3v_native_map_entry candidate;
      if (parse_map_line(line, &candidate) != 0)
         continue;
      if (anchor < candidate.start || anchor >= candidate.end)
         continue;
      if (candidate.path[0] == '\0') {
         result = -ENOENT;
         break;
      }
      if (path_is_deleted(candidate.path)) {
         result = -ESTALE;
         break;
      }
      *entry = candidate;
      result = 0;
      break;
   }
   if (ferror(maps) != 0)
      result = -EIO;
   fclose(maps);
   return result;
}

static int
hash_file(int fd, char digest_out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct mesa_blake3 context;
   uint8_t bytes[R3V_NATIVE_HASH_CHUNK_SIZE];
   uint8_t digest[BLAKE3_OUT_LEN];
   off_t offset = 0;

   if (lseek(fd, 0, SEEK_SET) < 0)
      return -errno;
   _mesa_blake3_init(&context);
   for (;;) {
      ssize_t got = pread(fd, bytes, sizeof(bytes), offset);
      if (got < 0) {
         if (errno == EINTR)
            continue;
         return -errno;
      }
      if (got == 0)
         break;
      _mesa_blake3_update(&context, bytes, (size_t)got);
      offset += got;
   }
   _mesa_blake3_final(&context, digest);
   _mesa_blake3_format(digest_out, digest);
   return 0;
}

static int
verify_executable_mappings(const struct r3v_native_map_entry *identity,
                           int file_fd, off_t file_size)
{
   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return -errno;
   int memory_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
   if (memory_fd < 0) {
      int result = -errno;
      fclose(maps);
      return result;
   }

   bool found_executable = false;
   char line[R3V_NATIVE_MAP_LINE_SIZE];
   int result = 0;
   while (fgets(line, sizeof(line), maps) != NULL) {
      struct r3v_native_map_entry candidate;
      if (parse_map_line(line, &candidate) != 0 ||
          candidate.device != identity->device ||
          candidate.inode != identity->inode ||
          strcmp(candidate.path, identity->path) != 0 ||
          candidate.permissions[2] != 'x')
         continue;

      found_executable = true;
      if (candidate.offset >= (uint64_t)file_size)
         continue;
      uint64_t available = (uint64_t)file_size - candidate.offset;
      uint64_t mapped = (uint64_t)(candidate.end - candidate.start);
      if (mapped > available)
         mapped = available;

      uint8_t file_bytes[R3V_NATIVE_HASH_CHUNK_SIZE];
      uint8_t memory_bytes[R3V_NATIVE_HASH_CHUNK_SIZE];
      uint64_t compared = 0;
      while (compared < mapped) {
         uint64_t remaining = mapped - compared;
         size_t chunk = remaining > sizeof(file_bytes)
                           ? sizeof(file_bytes)
                           : (size_t)remaining;
         ssize_t file_read = pread(
            file_fd, file_bytes, chunk,
            (off_t)(candidate.offset + compared));
         ssize_t memory_read = pread(
            memory_fd, memory_bytes, chunk,
            (off_t)(candidate.start + compared));
         if (file_read != (ssize_t)chunk ||
             memory_read != (ssize_t)chunk ||
             memcmp(file_bytes, memory_bytes, chunk) != 0) {
            result = -ESTALE;
            goto out;
         }
         compared += chunk;
      }
   }
   if (ferror(maps) != 0)
      result = -EIO;
   else if (!found_executable)
      result = -ENOENT;

out:
   close(memory_fd);
   fclose(maps);
   return result;
}

int
r3v_native_identity_collect(char *path_out, size_t path_size,
                            char *digest_out, size_t digest_size)
{
   if (path_out == NULL || digest_out == NULL || path_size == 0 ||
       digest_size < BLAKE3_OUT_LEN * 2 + 1)
      return -EINVAL;

   struct r3v_native_map_entry identity;
   int result = find_anchor_mapping(&identity);
   if (result != 0)
      return result;
   if (strlen(identity.path) >= path_size)
      return -ENAMETOOLONG;

   int fd = open(identity.path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return -errno;
   struct stat status;
   if (fstat(fd, &status) != 0) {
      result = -errno;
      goto close_file;
   }
   if (!S_ISREG(status.st_mode) || status.st_size <= 0 ||
       status.st_dev != identity.device || status.st_ino != identity.inode) {
      result = -ESTALE;
      goto close_file;
   }

   result = verify_executable_mappings(&identity, fd, status.st_size);
   if (result != 0)
      goto close_file;
   result = hash_file(fd, digest_out);
   if (result == 0)
      snprintf(path_out, path_size, "%s", identity.path);

close_file:
   close(fd);
   return result;
}

static int
append_json_bytes(char **cursor, size_t *remaining, const char *bytes,
                  size_t count)
{
   if (*remaining <= count)
      return -ENOSPC;
   memcpy(*cursor, bytes, count);
   *cursor += count;
   *remaining -= count;
   **cursor = '\0';
   return 0;
}

int
r3v_native_json_escape(char *out, size_t out_size, const char *input)
{
   if (out == NULL || input == NULL || out_size == 0)
      return -EINVAL;
   out[0] = '\0';
   char *cursor = out;
   size_t remaining = out_size;
   static const char hex[] = "0123456789abcdef";
   for (const unsigned char *source = (const unsigned char *)input;
        *source != '\0'; source++) {
      char escaped[6];
      const char *replacement = NULL;
      size_t replacement_size = 1;
      switch (*source) {
      case '"':
         replacement = "\\\"";
         replacement_size = 2;
         break;
      case '\\':
         replacement = "\\\\";
         replacement_size = 2;
         break;
      case '\b':
         replacement = "\\b";
         replacement_size = 2;
         break;
      case '\f':
         replacement = "\\f";
         replacement_size = 2;
         break;
      case '\n':
         replacement = "\\n";
         replacement_size = 2;
         break;
      case '\r':
         replacement = "\\r";
         replacement_size = 2;
         break;
      case '\t':
         replacement = "\\t";
         replacement_size = 2;
         break;
      default:
         if (*source < 0x20) {
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = '0';
            escaped[3] = '0';
            escaped[4] = hex[*source >> 4];
            escaped[5] = hex[*source & 0xf];
            replacement = escaped;
            replacement_size = sizeof(escaped);
         } else {
            escaped[0] = (char)*source;
            replacement = escaped;
         }
         break;
      }
      if (append_json_bytes(&cursor, &remaining, replacement,
                            replacement_size) != 0)
         return -ENOSPC;
   }
   return (int)(cursor - out);
}
