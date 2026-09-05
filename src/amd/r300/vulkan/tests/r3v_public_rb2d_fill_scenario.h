/*
 * SPDX-License-Identifier: MIT
 *
 * Public-API scenario for the RB2D constant-fill cell: the object graph
 * and the one submission an application makes, expressed through the
 * Vulkan loader alone.  A wrapper binary owns the environmental
 * refusals and the verdict; this file owns instance through cleanup.
 */

#ifndef R3V_PUBLIC_RB2D_FILL_SCENARIO_H
#define R3V_PUBLIC_RB2D_FILL_SCENARIO_H

#include "r3v_public_rb2d_fill_oracle.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct r3v_public_rb2d_fill_scenario_config {
   const struct r3v_public_rb2d_fill_cell *cell;
   /* mprotect(PROT_READ) over the mapping before the submit, so a host
    * store into the destination faults instead of completing. */
   bool protect_destination;
   uint64_t wait_bound_ns;
   /* Required physical-device identity; 0 admits any. */
   uint32_t required_vendor_id;
   uint32_t required_device_id;
};

struct r3v_public_rb2d_fill_scenario {
   struct r3v_public_rb2d_fill_scenario_config config;
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkPhysicalDeviceProperties properties;
   VkDevice device;
   VkQueue queue;
   VkBuffer buffer;
   VkDeviceMemory memory;
   uint32_t memory_type_index;
   bool host_coherent;
   uint8_t *map;
   bool map_protected;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   VkResult submit_result;
   VkResult wait_result;
   /* The first failure the scenario met, empty while none. */
   char failure[256];
};

/* Instance, physical device, device, queue, buffer, memory selection and
 * binding, map, destination initialization, flush when the memory type
 * is not host-coherent, and the read-only protection.  Returns false
 * with the failure named; the scenario stays closable. */
bool r3v_public_rb2d_fill_scenario_open(
   struct r3v_public_rb2d_fill_scenario *s,
   const struct r3v_public_rb2d_fill_scenario_config *config);

/* Command pool, one command buffer, exactly one vkCmdFillBuffer, and the
 * fence. */
bool r3v_public_rb2d_fill_scenario_record(struct r3v_public_rb2d_fill_scenario *s);

/* Exactly one vkQueueSubmit; submit_result carries the loader's answer. */
bool r3v_public_rb2d_fill_scenario_submit(struct r3v_public_rb2d_fill_scenario *s);

/* One bounded fence wait, then the invalidate the memory contract asks
 * of a non-coherent type; wait_result carries the answer. */
bool r3v_public_rb2d_fill_scenario_wait(struct r3v_public_rb2d_fill_scenario *s);

/* The mapped destination for inspection; the scenario never writes it
 * after initialization. */
const uint8_t *r3v_public_rb2d_fill_scenario_image(
   const struct r3v_public_rb2d_fill_scenario *s);

void r3v_public_rb2d_fill_scenario_close(struct r3v_public_rb2d_fill_scenario *s);

const char *r3v_public_rb2d_fill_result_name(VkResult r);

/* Whether a DSO path appears in this process's mappings; the loader
 * resolves the ICD through its manifest, so the mapped DSO is the proof
 * of which driver answered. */
bool r3v_public_rb2d_fill_dso_mapped(const char *path);

#endif
