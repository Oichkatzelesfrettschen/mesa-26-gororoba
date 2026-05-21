/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_a0_dump.h -- env-gated descriptor-object capture (Y.3a).
 *
 * Emits the SQ_TEX_RESOURCE / SQ_TEX_RESOURCE_GATHER / sampler dword
 * arrays that Terakan computes at VkImageView / VkSampler creation
 * time as JSON Lines to /tmp/terakan_a0_<pid>.jsonl.  This is the A0
 * (descriptor object) side of the master-plan A0/A1/B/C byte-path
 * comparison; A1 (final PM4 IB) is Y.3b, B (libdrm uprobe envelope)
 * is the existing Phase W.1 bundle, C (post-validator IB) is the
 * Y.2 radeon-palm-gate DKMS observer.
 *
 * Gate: env TERAKAN_DEBUG_DUMP_DESCRIPTOR with value "1" via the
 * strict gate helper terakan_env_gate_enabled().  Default off.
 *
 * Output schema (one JSON object per line per descriptor):
 *   {
 *     "event":          "a0_image_view" | "a0_sampler",
 *     "ts_nsec":        <CLOCK_MONOTONIC>,
 *     "pid":            <getpid>,
 *     "tid":            <gettid>,
 *     "handle":         "0x<view or sampler handle hex>",
 *     "image_handle":   "0x<image handle hex>"        (image_view only),
 *     "format":         <Vk format enum, decimal>     (image_view only),
 *     "resource":       ["0x<dword>", ...]            (image_view: 8 dwords),
 *     "resource_gather":["0x<dword>", ...]            (image_view: 8 dwords),
 *     "sampler":        ["0x<dword>", ...]            (sampler: 3 dwords)
 *   }
 *
 * NO writes happen unless the env gate is exactly "1".  No
 * production-path side effects.
 */

#ifndef TERAKAN_A0_DUMP_H
#define TERAKAN_A0_DUMP_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct terakan_image_view;
struct terakan_sampler;

/* Cheap enable check.  Inlines to one getenv() on first call and a
 * cached static on subsequent calls so the disabled-path overhead is
 * essentially zero. */
bool terakan_a0_dump_active(void);

/* Emit one a0_image_view JSONL row.  Caller already constructed
 * view->resource[] and view->resource_gather[]; this function just
 * formats + writes.  Safe to call unconditionally (re-checks
 * a0_dump_active() internally). */
void terakan_a0_dump_image_view(VkImageView view_handle,
                                struct terakan_image_view const *view,
                                VkImageViewCreateInfo const *create_info);

/* Emit one a0_sampler JSONL row.  Caller already constructed
 * sampler->descriptor (3 dwords per Evergreen ISA Ch.7). */
void terakan_a0_dump_sampler(VkSampler sampler_handle,
                             struct terakan_sampler const *sampler);

#endif /* TERAKAN_A0_DUMP_H */
