/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_descriptor_dump.h -- env-gated descriptor-object capture.
 *
 * Emits the SQ_TEX_RESOURCE / SQ_TEX_RESOURCE_GATHER / sampler dword
 * arrays that Terakan computes at VkImageView / VkSampler creation
 * time as JSON Lines to /tmp/terakan_descriptor_<pid>.jsonl.  These are the
 * descriptor-object (A0) bytes in the descriptor-object/final-IB/
 * libdrm-envelope/post-validator-IB byte-path ontology used to
 * localise wrong-result-vs-byte-preserved verdicts in
 * dEQP-VK.glsl.texture_gather cube int gather cases on Palm.
 *
 * Gate: env TERAKAN_DEBUG_DUMP_DESCRIPTOR with value "1" via the
 * strict gate helper terakan_env_gate_enabled().  Default off.
 *
 * Output schema (one JSON object per line per descriptor):
 *   {
 *     "event":          "descriptor_image_view" | "descriptor_sampler",
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

#ifndef TERAKAN_DESCRIPTOR_DUMP_H
#define TERAKAN_DESCRIPTOR_DUMP_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct terakan_image_view;
struct terakan_sampler;

/* Cheap enable check.  Inlines to one getenv() on first call and a
 * cached static on subsequent calls so the disabled-path overhead is
 * essentially zero. */
bool terakan_descriptor_dump_active(void);

/* Emit one a0_image_view JSONL row.  Caller already constructed
 * view->resource[] and view->resource_gather[]; this function just
 * formats + writes.  Safe to call unconditionally (re-checks
 * a0_dump_active() internally). */
void terakan_descriptor_dump_image_view(VkImageView view_handle,
                                struct terakan_image_view const *view,
                                VkImageViewCreateInfo const *create_info);

/* Emit one a0_sampler JSONL row.  Caller already constructed
 * sampler->descriptor (3 dwords per Evergreen ISA Ch.7). */
void terakan_descriptor_dump_sampler(VkSampler sampler_handle,
                             struct terakan_sampler const *sampler);

#endif /* TERAKAN_DESCRIPTOR_DUMP_H */
