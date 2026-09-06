/*
 * SPDX-License-Identifier: MIT
 *
 * The declaration load: one file, one snapshot, one session.
 */

#include "r3v_measurement_declaration.h"

#include "util/mesa-blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads the whole declaration out of one open file, or nothing.  The
 * buffer holds one byte more than the parser's text bound, so a read that
 * reaches that byte establishes the file is above the bound rather than
 * truncating it into a shorter declaration that parses. */
static bool
read_whole_declaration(FILE *f, char *text, size_t *length_out)
{
   const size_t got =
      fread(text, 1, R3V_MEASUREMENT_SESSION_TEXT_MAX + 1u, f);
   if (got > R3V_MEASUREMENT_SESSION_TEXT_MAX || ferror(f) != 0)
      return false;
   *length_out = got;
   return true;
}

static void
digest_bytes(const char *text, size_t length,
             struct r3v_measurement_digest *out)
{
   struct mesa_blake3 context;
   blake3_hash digest;
   _mesa_blake3_init(&context);
   _mesa_blake3_update(&context, text, length);
   _mesa_blake3_final(&context, digest);
   _mesa_blake3_format(out->hex, digest);
}

/* The declared executor, held to what this build carries and what this
 * device already selected.  Six facts decide it and each refuses on its
 * own: the route exists, it runs on the GPU, it executes, its evidence
 * reaches this route's own delivery rather than a unit its family shares,
 * it realizes the constant fill the declared cases are, and it serves a
 * transfer destination.  Where it carries an opt-in, that opt-in already
 * stands open; reading a declaration never opens one.
 *
 * The executor and the evidence scope are separate fields, so a row
 * carrying HOST beside NATIVE_GPU_ROUTE_CELL would answer a GPU campaign
 * through the scope alone.  Both are read. */
static bool
route_admits_declaration(const struct r3v_measurement_deployment *deployment,
                         const char *name, const char **reason)
{
   const struct r300_operation_route_row *row = NULL;
   for (uint32_t i = 0; deployment->routes != NULL &&
                        i < deployment->route_count && row == NULL;
        i++) {
      if (deployment->routes[i].name != NULL &&
          strcmp(deployment->routes[i].name, name) == 0)
         row = &deployment->routes[i];
   }
   if (row == NULL) {
      *reason = "the declaration names a route this build does not carry";
      return false;
   }
   if (row->executor != R300_OPERATION_ROUTE_EXECUTOR_GPU) {
      *reason = "the declaration names a route that runs on the host";
      return false;
   }
   if (row->state != R300_OPERATION_ROUTE_EXECUTING) {
      *reason = "the declaration names a route that does not execute";
      return false;
   }
   if (row->evidence_scope !=
       R300_COMPUTE_VERB_EVIDENCE_SCOPE_NATIVE_GPU_ROUTE_CELL) {
      *reason = "the declaration names a route with no delivery of its own";
      return false;
   }
   if (row->operation_id != R300_OPERATION_ID_CONSTFILL) {
      *reason = "the declaration names a route that fills no constant";
      return false;
   }
   if ((row->uses & R300_ROUTE_USE_TRANSFER_BUFFER) == 0) {
      *reason = "the declaration names a route that serves no transfer "
                "destination";
      return false;
   }
   if (row->gate != NULL) {
      if (deployment->route_gate_open == NULL ||
          (uint32_t)row->route_id >= deployment->route_gate_count ||
          !deployment->route_gate_open[row->route_id]) {
         *reason = "the declaration names a route whose opt-in stands closed";
         return false;
      }
   }
   return true;
}

enum r3v_measurement_declaration_status
r3v_measurement_declaration_open(
   struct r3v_measurement_session *session, const char *path,
   const struct r3v_measurement_deployment *deployment, const char **reason)
{
   const char *unused = NULL;
   if (reason == NULL)
      reason = &unused;
   *reason = NULL;
   /* The session is not touched before it decides.  Bringing it to its
    * initial state here would clear the bindings and restore the
    * allowance a live session already spent, which is the bypass the
    * predicate exists to refuse -- and an absent or refused declaration
    * would erase a standing campaign on its way out.  The caller
    * initializes once, and r3v_measurement_session_open below refuses a
    * reopen. */
   if (session == NULL || deployment == NULL) {
      *reason = "the declaration loads into no session";
      return R3V_MEASUREMENT_DECLARATION_REFUSED;
   }
   if (path == NULL || path[0] == '\0')
      return R3V_MEASUREMENT_DECLARATION_ABSENT;

   /* One open, one read.  Opening the path again to read what a first
    * open established would let the file change between the two, so the
    * open here is the one the bytes come out of, and it closes on every
    * arm below. */
   FILE *f = fopen(path, "rb");
   if (f == NULL) {
      *reason = "the declaration does not open";
      return R3V_MEASUREMENT_DECLARATION_REFUSED;
   }

   /* The bytes live in one buffer for the whole load: hashed, parsed, and
    * discarded together, so no second read can disagree with the first. */
   char *text = deployment->refuse_text_allocation
                   ? NULL
                   : malloc(R3V_MEASUREMENT_SESSION_TEXT_MAX + 1u);
   if (text == NULL) {
      fclose(f);
      *reason = "the declaration has no memory to be read into";
      return R3V_MEASUREMENT_DECLARATION_NO_MEMORY;
   }
   size_t length = 0;
   const bool read = read_whole_declaration(f, text, &length);
   fclose(f);
   if (!read) {
      free(text);
      *reason = "the declaration does not read, or is above the text bound";
      return R3V_MEASUREMENT_DECLARATION_REFUSED;
   }

   struct r3v_measurement_digest digest;
   digest_bytes(text, length, &digest);

   struct r3v_measurement_manifest manifest;
   enum r3v_measurement_session_refusal r =
      r3v_measurement_manifest_parse(text, length, &manifest, reason);
   /* Every reason this interface publishes is a string literal, so the
    * text is released here and the refusal below still names its rule. */
   free(text);
   if (r != R3V_MEASUREMENT_SESSION_ADMITTED)
      return R3V_MEASUREMENT_DECLARATION_REFUSED;

   /* The deployment, then the board, then the executor.  The epoch check
    * compares the PCI pair and the running kernel and module; the
    * platform check resolves the declared board name and requires it to
    * be the one underneath, because a PCI id names a die class several
    * boards share. */
   r = r3v_measurement_manifest_epoch_check(
      &manifest, deployment->pci_vendor_id, deployment->pci_device_id,
      deployment->kernel_release, deployment->module_srcversion, reason);
   if (r != R3V_MEASUREMENT_SESSION_ADMITTED)
      return R3V_MEASUREMENT_DECLARATION_REFUSED;

   r = r3v_measurement_manifest_platform_check(
      &manifest, deployment->platform_id, reason);
   if (r != R3V_MEASUREMENT_SESSION_ADMITTED)
      return R3V_MEASUREMENT_DECLARATION_REFUSED;

   if (!route_admits_declaration(deployment, manifest.route, reason))
      return R3V_MEASUREMENT_DECLARATION_REFUSED;

   if (r3v_measurement_session_open(session, &manifest, &digest, reason) !=
       R3V_MEASUREMENT_SESSION_ADMITTED)
      return R3V_MEASUREMENT_DECLARATION_REFUSED;
   *reason = NULL;
   return R3V_MEASUREMENT_DECLARATION_OPENED;
}
