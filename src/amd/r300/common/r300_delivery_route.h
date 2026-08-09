/*
 * SPDX-License-Identifier: MIT
 *
 * Vertex delivery route selection for the native TCL-bypass draw.
 */

#ifndef R300_DELIVERY_ROUTE_H
#define R300_DELIVERY_ROUTE_H

/* The two delivery routes the deferred draw owns.  The CPU gather is
 * the default and the semantic oracle; its lane dispatch (portable
 * baseline or SSE2) is decided inside r300_cpu_vertex_gather by the
 * qualified K8 measurement, so the route selector carries no lane
 * choice.  The R2VB host model engages only by exact experimental
 * opt-in.  A production R2VB promotion is a measurement decision the
 * selector holds closed: it requires total draw latency -- producer
 * submission, cache publication, and the re-ingest stall included, not
 * gather time alone -- measured on silicon with live delivery, and no
 * such measurement exists while live R2VB submission remains outside
 * the landed surface.
 */
enum r300_delivery_route {
   R300_DELIVERY_ROUTE_CPU = 0,
   R300_DELIVERY_ROUTE_R2VB_HOST_MODEL = 1,
};

struct r300_delivery_route_decision {
   enum r300_delivery_route route;
   /* The clause that selected the route, for refusal reports and
    * evidence records.
    */
   const char *reason;
};

/* Resolves the route from the experimental gate's value and the bound
 * stream's format.  gate_value is the raw environment value or NULL;
 * the gate opens on the exact string "1" alone -- unset, empty, "0",
 * and every other value keep the CPU route, so variable presence is
 * not consent.  The R2VB host model covers F32_4, F32_3, and F32_2;
 * any other format keeps the CPU route whatever the gate says.
 */
void r300_delivery_route_resolve(const char *gate_value, int format_id,
                                 struct r300_delivery_route_decision *out);

#endif /* R300_DELIVERY_ROUTE_H */
