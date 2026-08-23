/*
 * SPDX-License-Identifier: MIT
 *
 * No-submit composition identities of the fetched R2VB routes: the
 * reference fetched producer over a one-page source at offset zero ahead
 * of the reference TCL-bypass consumer at its maximum extent, roles bound
 * CARRIER 0, COLOR 1, SLOT 2, SOURCE 3.  Each pin is the host
 * composition through r300_r2vb_fetched_route_reference_compose and the
 * silicon identity of a retained RS482 delivery (steinmarder-r300
 * bundles r3v-native-fetched-gpu-producer-route-first-delivery-rs482
 * for F32_4, -f32-3-delivery-rs482, and -f32-2-delivery-rs482): the
 * armed digest, the offline composition, this pin, and the retained
 * ib.bin are one value per width, so a drift here is a drift from the
 * bytes the silicon delivered.
 */

#ifndef R300_FETCHED_ROUTE_DIGESTS_H
#define R300_FETCHED_ROUTE_DIGESTS_H

#define R300_FETCHED_F32_4_ROUTE_IB_DWORDS 547u
#define R300_FETCHED_F32_4_ROUTE_CONSUMER_START_DWORDS 316u
#define R300_FETCHED_F32_4_ROUTE_IB_BLAKE3 "597b762d5acc075a13053cf0842ae14fea9e2891cae88295762b332015a39598"

#define R300_FETCHED_F32_3_ROUTE_IB_DWORDS 547u
#define R300_FETCHED_F32_3_ROUTE_CONSUMER_START_DWORDS 316u
#define R300_FETCHED_F32_3_ROUTE_IB_BLAKE3 "bd0194dc1804f14bd6465b449b3e2333284d7d96ad82255084e37a23d748fb26"

#define R300_FETCHED_F32_2_ROUTE_IB_DWORDS 547u
#define R300_FETCHED_F32_2_ROUTE_CONSUMER_START_DWORDS 316u
#define R300_FETCHED_F32_2_ROUTE_IB_BLAKE3 "354dd14eeaa72b53a210e0231ae74c48ab50e7f479decfca850c65542bc8b419"

#endif /* R300_FETCHED_ROUTE_DIGESTS_H */
