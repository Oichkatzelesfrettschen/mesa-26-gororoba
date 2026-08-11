/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V issuer identity and JSON string helpers.
 */

#ifndef R3V_NATIVE_IDENTITY_H
#define R3V_NATIVE_IDENTITY_H

#include <stddef.h>

/* Resolves the object containing this helper's code.  The returned path names
 * the mapped DSO or executable that carries the native implementation, and
 * the digest covers that file after its mapping identity and executable bytes
 * have been checked.  Returns 0 or a negative errno-style value. */
int r3v_native_identity_collect(char *path_out, size_t path_size,
                                char *digest_out, size_t digest_size);

/* Encodes one JSON string value without surrounding quotes.  Returns
 * the encoded byte count or a negative errno-style value. */
int r3v_native_json_escape(char *out, size_t out_size, const char *input);

#endif /* R3V_NATIVE_IDENTITY_H */
