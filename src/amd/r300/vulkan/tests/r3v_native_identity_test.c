/*
 * SPDX-License-Identifier: MIT
 *
 * Host-model calibration for native R3V issuer identity and JSON strings.
 */

#undef NDEBUG

#include "r3v_native_identity.h"

#include "util/mesa-blake3.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void
test_json_escape(void)
{
   const char input[] = "quote\" slash\\ line\n\t\r\b\f\x01";
   const char expected[] =
      "quote\\\" slash\\\\ line\\n\\t\\r\\b\\f\\u0001";
   char output[128];

   assert(r3v_native_json_escape(output, sizeof(output), input) ==
          (int)strlen(expected));
   assert(strcmp(output, expected) == 0);

   char too_small[4];
   assert(r3v_native_json_escape(too_small, sizeof(too_small), input) ==
          -ENOSPC);

   const char malformed_utf8[] = {
      'p', 'a', 't', 'h', (char)0xff, (char)0xc3, 'x', '\0',
   };
   const char malformed_expected[] = "path\\udcff\\udcc3x";
   assert(r3v_native_json_escape(output, sizeof(output), malformed_utf8) ==
          (int)strlen(malformed_expected));
   assert(strcmp(output, malformed_expected) == 0);

   const char valid_utf8[] = "caf\xc3\xa9";
   assert(r3v_native_json_escape(output, sizeof(output), valid_utf8) ==
          (int)strlen(valid_utf8));
   assert(strcmp(output, valid_utf8) == 0);
}

static void
test_metadata_identity(void)
{
   struct stat before = {0};
   before.st_dev = 11;
   before.st_ino = 22;
   before.st_size = 33;
   before.st_mtim.tv_sec = 44;
   before.st_mtim.tv_nsec = 55;
   before.st_ctim.tv_sec = 66;
   before.st_ctim.tv_nsec = 77;

   struct stat after = before;
   assert(r3v_native_identity_metadata_unchanged(&before, &after));

   after.st_size++;
   assert(!r3v_native_identity_metadata_unchanged(&before, &after));
   after = before;
   after.st_mtim.tv_nsec++;
   assert(!r3v_native_identity_metadata_unchanged(&before, &after));
   after = before;
   after.st_ctim.tv_nsec++;
   assert(!r3v_native_identity_metadata_unchanged(&before, &after));
   after = before;
   after.st_ino++;
   assert(!r3v_native_identity_metadata_unchanged(&before, &after));
}

static void
test_mapped_identity(void)
{
   char path[2048];
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   assert(r3v_native_identity_collect(path, sizeof(path), digest,
                                      sizeof(digest)) == 0);
   assert(path[0] == '/');
   assert(strcmp(path, "unresolved") != 0);
   assert(strlen(digest) == BLAKE3_OUT_LEN * 2);

   FILE *file = fopen(path, "rb");
   assert(file != NULL);
   struct mesa_blake3 context;
   uint8_t bytes[4096];
   uint8_t expected_digest[BLAKE3_OUT_LEN];
   _mesa_blake3_init(&context);
   size_t got;
   while ((got = fread(bytes, 1, sizeof(bytes), file)) > 0)
      _mesa_blake3_update(&context, bytes, got);
   assert(!ferror(file));
   fclose(file);
   _mesa_blake3_final(&context, expected_digest);
   char expected_hex[BLAKE3_OUT_LEN * 2 + 1];
   _mesa_blake3_format(expected_hex, expected_digest);
   assert(strcmp(digest, expected_hex) == 0);
}

int
main(void)
{
   test_json_escape();
   test_metadata_identity();
   test_mapped_identity();
   puts("r3v_native_identity_test: calibrated identity and JSON checks pass");
   return 0;
}
