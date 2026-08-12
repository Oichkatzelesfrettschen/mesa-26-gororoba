/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V issuer identity and JSON string helpers.
 */

#ifndef R3V_NATIVE_IDENTITY_H
#define R3V_NATIVE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

static inline bool
r3v_native_identity_file_matches(const struct stat *expected,
                                 const struct stat *actual)
{
   return expected->st_dev == actual->st_dev &&
          expected->st_ino == actual->st_ino;
}

static inline bool
r3v_native_identity_metadata_unchanged(const struct stat *before,
                                       const struct stat *after)
{
   return r3v_native_identity_file_matches(before, after) &&
          before->st_size == after->st_size &&
          before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
          before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
          before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
          before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

/* Resolves the object containing this helper's code.  The returned path names
 * the mapped DSO or executable that carries the native implementation, and
 * the digest covers that file after its mapping identity and executable bytes
 * have been checked.  Returns 0 or a negative errno-style value. */
int r3v_native_identity_collect(char *path_out, size_t path_size,
                                char *digest_out, size_t digest_size);

/* Encodes one JSON string value without surrounding quotes.  Valid UTF-8
 * remains UTF-8; malformed bytes become \udcXX surrogate-escape sequences
 * that os.fsencode maps back to the original bytes.  Returns the encoded byte
 * count or a negative errno-style value. */
int r3v_native_json_escape(char *out, size_t out_size, const char *input);

#endif /* R3V_NATIVE_IDENTITY_H */
