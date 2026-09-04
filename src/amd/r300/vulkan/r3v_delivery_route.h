/*
 * SPDX-License-Identifier: MIT
 *
 * Vertex delivery route selection for the native TCL-bypass draw.
 */

#ifndef R3V_DELIVERY_ROUTE_H
#define R3V_DELIVERY_ROUTE_H

/* The delivery routes share one coordinate-space contract.  The CPU gather
 * and R2VB host-model routes select CLIP: the native draw expands host
 * records through clipping and viewport setup before the TCL-bypass consumer.
 * The immediate and fetched GPU-producer routes select WINDOW: the producer
 * writes pretransformed slot records and the consumer binds them through the
 * VTE passthrough form.
 *
 * AMD's R3xx 3D Registers define `VAP_VTE_CNTL` at MMReg 0x20b0 as the
 * coordinate interpretation control.  The checked-in register identity is
 * found with `(rg --fixed-strings R300_VAP_VTE_CNTL
 * src/amd/r300/common/r300_reg.h)`.  The r300g implementation selects the
 * two encodings at `(rg --fixed-strings r2vb_source_window
 * src/gallium/drivers/r300/r300_render.c)`: window sources set
 * `R300_VTX_XY_FMT | R300_VTX_Z_FMT | R300_VTX_W0_FMT`, while clip sources
 * set `R300_VTX_W0_FMT` with the viewport enable bits.  The neutral re-ingest
 * emitter carries the same distinction at `(rg --fixed-strings
 * r300_r2vb_reingest_draw_emit src/amd/r300/common)`.  The native producer
 * prologue is the producer-side authority at `(rg --fixed-strings
 * r300_r2vb_producer_prologue_emit src/amd/r300/common)`, and the native
 * admission path binds its recorded window consumer at `(rg --fixed-strings
 * r3v_native_deferred_draw_admit_gpu_producer src/amd/r300/vulkan)`.
 *
 * The fetched producer -- the VAP reading the application's records
 * from the bound vertex BO instead of the CP reading them as packet
 * dwords -- was measured the same way on RS485M silicon as a third arm
 * beside the two: over twelve three-route rounds the CPU route's
 * 101.7 us median led the fetched route's 114.2 us with all twelve
 * paired differences agreeing in sign, a 0.0913 lead of the CPU
 * median, and the fetched route did not separate from the immediate
 * producer.  The CPU default therefore stands measured against both
 * producer routes.
 *
 * Both results are bounded to the admitted geometry: three F32_4
 * vertices at the consumer's maximum extent.  The host gather scales
 * with vertex count while neither producer's command overhead does, so
 * a wider admitted set is a separate measurement and does not inherit
 * these decisions.
 */
enum r3v_delivery_route {
   R3V_DELIVERY_ROUTE_CPU = 0,
   R3V_DELIVERY_ROUTE_R2VB_HOST_MODEL = 1,
   /* The device-side producer: the carrier is written by the R2VB
    * producer pass instead of a host copy.  Selecting it takes both
    * experimental gates at their exact values, and only F32_4 -- the
    * format whose identity delivery and re-ingest hold on RS485M
    * silicon -- resolves to it; the FLOAT_2 tuple mechanism reaches
    * silicon through the operator-armed attended surface, outside this
    * resolver.  The route names the mechanism for the deferred draw;
    * live dispatch stays with the attended surface, so a caller that
    * cannot execute a producer submission refuses the draw by name
    * rather than downgrading silently.
    */
   R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER = 2,
   /* The device-side producer fetching the application's vertex BO
    * through the two-array fetched body, with a driver-owned slot BO as
    * the first array, instead of embedding the gathered records as
    * DRAW_IMMD_2 dwords.  Selecting it takes the two producer gates plus
    * the fetched gate at their exact values and one of F32_4, F32_3,
    * F32_2: the one emitter fills a narrower record's missing lanes
    * through the fetch swizzle, so each width is its own cell with its
    * own composition identity, retained on silicon per width.  The third
    * gate keeps the qualified immediate route reachable beside it.
    */
   R3V_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED = 3,
};

/* The coordinate-space selector names the consumer contract.  CLIP routes
 * run the host clipping and viewport expansion before the clip-space cell
 * emitter.  WINDOW routes carry producer-transformed records and use the
 * untransformed consumer.  Keeping the declaration beside the route resolver
 * prevents a producer's viewport result from receiving a second transform.
 */
enum r300_carrier_position_space {
   R300_CARRIER_POSITION_CLIP = 0,
   R300_CARRIER_POSITION_WINDOW = 1,
};

struct r3v_delivery_route_decision {
   enum r3v_delivery_route route;
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
 * producer route takes both producer gates open and F32_4; with the
 * base gate closed the GPU gate alone selects nothing.  The fetched
 * producer route takes all three gates open and any of the three host
 * model widths; the fetched gate alone, or with only one producer gate,
 * selects nothing.
 */
void r3v_delivery_route_resolve(const char *gate_value,
                                 const char *gpu_gate_value,
                                 const char *fetched_gate_value,
                                 int format_id,
                                 struct r3v_delivery_route_decision *out);

#endif /* R3V_DELIVERY_ROUTE_H */
