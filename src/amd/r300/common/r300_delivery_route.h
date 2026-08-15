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
   /* The device-side producer: the carrier is written by the R2VB
    * producer pass instead of a host copy.  Selecting it takes both
    * experimental gates at their exact values, and only F32_4 -- the
    * format whose identity delivery and re-ingest hold on RS482
    * silicon -- resolves to it; the FLOAT_2 tuple mechanism reaches
    * silicon through the operator-armed attended surface, outside this
    * resolver.  The route names the mechanism for the deferred draw;
    * live dispatch stays with the attended surface, so a caller that
    * cannot execute a producer submission refuses the draw by name
    * rather than downgrading silently.
    */
   R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER = 2,
};

/* The coordinate space the selected route leaves in the carrier.  Both
 * present routes copy application values, so the carrier holds
 * clip-volume positions and the consumer owns the one viewport
 * transform.  A route whose producer transforms on the device declares
 * WINDOW, and the consumer then binds the carrier untransformed; the
 * declaration is what keeps the transform single when both kinds of
 * route exist.
 */
enum r300_carrier_position_space {
   R300_CARRIER_POSITION_CLIP = 0,
   R300_CARRIER_POSITION_WINDOW = 1,
};

struct r300_delivery_route_decision {
   enum r300_delivery_route route;
   enum r300_carrier_position_space position_space;
   /* The clause that selected the route, for refusal reports and
    * evidence records.
    */
   const char *reason;
};

/* Resolves the route from the experimental gates' values and the bound
 * stream's format.  Each gate value is the raw environment value or
 * NULL and opens on the exact string "1" alone -- unset, empty, "0",
 * and every other value stay closed, so variable presence is not
 * consent.  The R2VB host model covers F32_4, F32_3, and F32_2; any
 * other format keeps the CPU route whatever the gates say.  The GPU
 * producer route takes both gates open and F32_4; with the base gate
 * closed the GPU gate alone selects nothing.
 */
void r300_delivery_route_resolve(const char *gate_value,
                                 const char *gpu_gate_value, int format_id,
                                 struct r300_delivery_route_decision *out);

#endif /* R300_DELIVERY_ROUTE_H */
