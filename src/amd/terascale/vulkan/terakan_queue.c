/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_queue.h"
#include "terakan_profile.h"
#include "terakan_instance.h"

#include "terakan_barrier.h"
#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_physical_device.h"
#include "terakan_shader.h"
#include "terakan_sync_completion.h"

#include "c11/threads.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "gallium/drivers/r600/r600d_common.h"
#include "util/macros.h"
#include "c99_alloca.h"
#include "vk_alloc.h"
#include "vk_enum_to_str.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "vk_sync_dummy.h"
#include "vk_synchronization.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WAIT_REG_MEM_PFP
#define WAIT_REG_MEM_PFP (1 << 8)
#endif

static int
terakan_queue_completion_thread_func(void * queue_ptr)
{
   struct terakan_queue * const queue = queue_ptr;

   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);

   mtx_lock(&device->completion_mutex);
   while (true) {
      if (device->completion_lost) {
         mtx_unlock(&device->completion_mutex);
         return 1;
      }

      if (queue->shutdown_competion_thread) {
         /* Graceful device shutdown. */
         break;
      }

      if (list_is_empty(&queue->completion_submissions_pending)) {
         if (cnd_wait(&device->completion_condition, &device->completion_mutex) != thrd_success) {
            vk_device_set_lost(&device->vk, "Failed to await the submission condition variable");
            device->completion_lost = true;
            mtx_unlock(&device->completion_mutex);
            cnd_broadcast(&device->completion_condition);
            return 1;
         }
         continue;
      }

      /* Await the BO not to be potentially accessed by the GPU anymore without holding the
       * mutex.
       */
      struct terakan_queue_completion_submission * submission = list_first_entry(
         &queue->completion_submissions_pending, struct terakan_queue_completion_submission, link);
      list_del(&submission->link);
      mtx_unlock(&device->completion_mutex);
      bool const awaited = device->winsys_fn->queue->completion_submission_await(submission);
      mtx_lock(&device->completion_mutex);
      if (unlikely(!awaited)) {
         vk_device_set_lost(&device->vk, "Failed to await for the submission completion fence");
         device->completion_lost = true;
      } else {
         struct terakan_queue_completion_signal * signal;
         LIST_FOR_EACH_ENTRY (signal, &submission->signals, link) {
            assert(signal->value <= signal->sync->pending_value);
            assert(signal->value > signal->sync->current_value);
            signal->sync->current_value = signal->value;
         }
      }

      /* Recycle the submission. */
      list_splice(&submission->signals, &queue->completion_signals_free);
      list_inithead(&submission->signals);
      list_add(&submission->link, &queue->completion_submissions_free);

      /* Notify signal waits of new semaphore values or the failure. */
      cnd_broadcast(&device->completion_condition);
   }
   mtx_unlock(&device->completion_mutex);

   return 0;
}

static void
terakan_queue_replace_relocation_offset_for_32_bits(
   enum terakan_queue_relocation_type const relocation_type, void * const relocations,
   uint32_t const relocation_handle, uint32_t const wddm_allocation_offset)
{
   switch (relocation_type) {
   case TERAKAN_QUEUE_RELOCATION_TYPE_WDDM_PATCH: {
      struct terakan_queue_relocation_wddm_patch * const patch =
         &((struct terakan_queue_relocation_wddm_patch *)relocations)[relocation_handle];
      patch->allocation_offset = wddm_allocation_offset;
   } break;

   default:
      break;
   }
}

struct terakan_queue_wsi_wait {
   struct terakan_bo * bo;
   uint32_t value;
};

static VkResult
terakan_queue_handle_wait_result(struct terakan_device * const device,
                                 VkResult const wait_result,
                                 char const * const description)
{
   if (wait_result == VK_SUCCESS) {
      return VK_SUCCESS;
   }
   if (wait_result == VK_ERROR_OUT_OF_HOST_MEMORY ||
       wait_result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
      return vk_error(device, wait_result);
   }

   vk_device_set_lost(&device->vk, "Failed to await %s with result %s", description,
                      vk_Result_to_str(wait_result));
   mtx_lock(&device->completion_mutex);
   device->completion_lost = true;
   mtx_unlock(&device->completion_mutex);
   cnd_broadcast(&device->completion_condition);
   return VK_ERROR_DEVICE_LOST;
}

static VkResult
terakan_queue_submit_wsi_wait_indirect_buffer(
   struct terakan_queue * const queue, uint32_t const wait_count,
   struct terakan_queue_wsi_wait const * const waits, uint32_t const poll_interval)
{
   if (wait_count == 0) {
      return VK_SUCCESS;
   }

   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);
   if (physical_device->submission_info_gfx.base.relocation_type !=
       TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP) {
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   size_t const bo_references_storage_size =
      (size_t)device->bo_reference_size * wait_count + device->bo_reference_alignment - 1;
   void * const bo_references_storage = alloca(bo_references_storage_size);
   void * const bo_references =
      (void *)ALIGN_POT((uintptr_t)bo_references_storage, device->bo_reference_alignment);

   uint32_t const indirect_buffer_max_dwords =
      wait_count * 9 + TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1;
   uint32_t * const indirect_buffer = alloca(sizeof(*indirect_buffer) * indirect_buffer_max_dwords);
   uint32_t indirect_buffer_size_dwords = 0;

   for (uint32_t wait_index = 0; wait_index < wait_count; ++wait_index) {
      void * const bo_reference =
         (char *)bo_references + (size_t)device->bo_reference_size * wait_index;
      device->winsys_fn->queue->create_bo_reference(bo_reference, waits[wait_index].bo, true, false,
                                                    TERAKAN_BO_PRIORITY_SYNC);

      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_WAIT_REG_MEM, 5, 0);
      indirect_buffer[indirect_buffer_size_dwords++] =
         WAIT_REG_MEM_GEQUAL | WAIT_REG_MEM_MEMORY | WAIT_REG_MEM_PFP;
      indirect_buffer[indirect_buffer_size_dwords++] = 0;
      indirect_buffer[indirect_buffer_size_dwords++] = 0;
      indirect_buffer[indirect_buffer_size_dwords++] = waits[wait_index].value;
      indirect_buffer[indirect_buffer_size_dwords++] = UINT32_MAX;
      indirect_buffer[indirect_buffer_size_dwords++] = poll_interval;
      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_NOP, 0, 0);
      indirect_buffer[indirect_buffer_size_dwords++] = 4 * wait_index;
   }

   while (indirect_buffer_size_dwords &
          (TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1)) {
      indirect_buffer[indirect_buffer_size_dwords++] = PKT_TYPE_S(2);
   }

   return device->winsys_fn->queue->submit(queue->submission_context, wait_count, bo_references,
                                           indirect_buffer_size_dwords, indirect_buffer, 0, NULL);
}

static VkResult
terakan_queue_ensure_wsi_hw_wait_supported(struct terakan_queue * const queue,
                                           bool * const supported_out)
{
   if (queue->wsi_hw_wait_probe_state > 0) {
      *supported_out = true;
      return VK_SUCCESS;
   }
   if (queue->wsi_hw_wait_probe_state < 0) {
      *supported_out = false;
      return VK_SUCCESS;
   }

   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);
   if (physical_device->submission_info_gfx.base.relocation_type !=
       TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP) {
      queue->wsi_hw_wait_probe_state = -1;
      *supported_out = false;
      return VK_SUCCESS;
   }

   struct terakan_bo * probe_bo;
   VkResult result = device->winsys_fn->bo->allocate_device_memory(
      device, 16, 16, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, NULL,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE, &probe_bo);
   if (result != VK_SUCCESS) {
      return vk_error(device, result);
   }

   uint32_t * const probe_mapping = terakan_bo_map(probe_bo);
   if (probe_mapping == NULL) {
      terakan_bo_free(probe_bo, NULL);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   __atomic_store_n(probe_mapping, 1u, __ATOMIC_RELEASE);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);

   struct terakan_queue_wsi_wait probe_wait = {
      .bo = probe_bo,
      .value = 1,
   };
   result = terakan_queue_submit_wsi_wait_indirect_buffer(queue, 1, &probe_wait, 0xFF);

   terakan_bo_free(probe_bo, NULL);

   queue->wsi_hw_wait_probe_state = result == VK_SUCCESS ? 1 : -1;
   *supported_out = result == VK_SUCCESS;
   return VK_SUCCESS;
}

#define TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS ((uint32_t)1 << 5)
static_assert(
   TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS >=
      TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX,
   "Signal indirect buffer size upper bound must be high enough to fit all GFX indirect buffer "
   "size alignment padding.");

static uint32_t
terakan_queue_get_graphics_signal_indirect_buffer(
   struct terakan_device const * const device, VkPipelineStageFlags2 const expanded_signal_stages,
   VkPipelineStageFlags2 const expanded_shader_ring_signal_stages,
   uint32_t const sx_surface_sync_mask,
   uint32_t indirect_buffer[TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS])
{
   uint32_t indirect_buffer_size_dwords = 0;

   /* Disable register shadowing before executing any packets that may set registers (not clear if
    * CP_COHER_CNTL setting in SURFACE_SYNC interacts with it, but for safety it's preferable to do
    * this for all submissions).
    */
   assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 3);
   indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_CONTEXT_CONTROL, 1, 0);
   /* CC0_UPDATE_LOAD_ENABLES(1) */
   indirect_buffer[indirect_buffer_size_dwords++] = (uint32_t)1 << 31;
   /* CC1_UPDATE_SHADOW_ENABLES(1) */
   indirect_buffer[indirect_buffer_size_dwords++] = (uint32_t)1 << 31;

   uint32_t cp_coher_cntl_cb_db_dest_base_ena = 0;
   uint32_t cp_coher_cntl = 0;

   bool const flush_uav =
      (expanded_signal_stages & ((device->vk.enabled_features.fragmentStoresAndAtomics
                                     ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                     : 0) |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)) != 0;
   bool const flush_rtv =
      (expanded_signal_stages &
       (VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT)) != 0;
   bool const flush_db =
      (expanded_signal_stages & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)) != 0;

   if (flush_uav) {
      cp_coher_cntl_cb_db_dest_base_ena |=
         S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
         S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
         S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
         S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
         S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
         S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1);
      cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1);
   }
   if (flush_rtv) {
      cp_coher_cntl_cb_db_dest_base_ena |=
         S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
         S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
         S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
         S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1);
      cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1);
   }
   if (flush_db) {
      cp_coher_cntl_cb_db_dest_base_ena |= S_0085F0_DB_DEST_BASE_ENA(1);
      cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1);
   }
   if (sx_surface_sync_mask) {
      assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 3);
      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_SET_CONTEXT_REG, 1, 0);
      indirect_buffer[indirect_buffer_size_dwords++] =
         TERAKAN_CONTEXT_REG_OFFSET(R_028354_SX_SURFACE_SYNC);
      indirect_buffer[indirect_buffer_size_dwords++] =
         S_028354_SURFACE_SYNC_MASK(sx_surface_sync_mask);
      cp_coher_cntl |= S_0085F0_SH_ACTION_ENA(1);
   }

   if (flush_uav || sx_surface_sync_mask) {
      /* Perform a full destination cache flush if UAVs need to be flushed because
       * FLUSH_AND_INV_CB_DATA_TS writes a timestamp and thus needs a BO.
       * Fence signals result in a full flush anyway, more granularity may only be useful for
       * semaphores.
       */
      assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 2);
      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
      indirect_buffer[indirect_buffer_size_dwords++] =
         EVENT_TYPE(EVENT_TYPE_CACHE_FLUSH_AND_INV_EVENT) | EVENT_INDEX(0);
   } else {
      if (flush_rtv) {
         assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >=
                2 * 2);
         indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
         indirect_buffer[indirect_buffer_size_dwords++] =
            EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_PIXEL_DATA) | EVENT_INDEX(0);
         indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
         indirect_buffer[indirect_buffer_size_dwords++] =
            EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_META) | EVENT_INDEX(0);
      }
      if (flush_db) {
         assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 2);
         indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
         indirect_buffer[indirect_buffer_size_dwords++] =
            EVENT_TYPE(EVENT_TYPE_DB_CACHE_FLUSH_AND_INV) | EVENT_INDEX(0);
      }
   }

   VkPipelineStageFlags2 const partial_flush_stages =
      expanded_signal_stages | expanded_shader_ring_signal_stages;

   /* SURFACE_SYNC with any CB/DB_DEST_BASE_ENA implies PS_PARTIAL_FLUSH. */
   if (!cp_coher_cntl_cb_db_dest_base_ena) {
      if (partial_flush_stages &
          (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT)) {
         assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 2);
         indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
         indirect_buffer[indirect_buffer_size_dwords++] =
            EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4);
      } else if (partial_flush_stages &
                 (VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT)) {
         assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 2);
         indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
         indirect_buffer[indirect_buffer_size_dwords++] =
            EVENT_TYPE(EVENT_TYPE_VS_PARTIAL_FLUSH) | EVENT_INDEX(4);
      }
   }

   if (partial_flush_stages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
      assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 2);
      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
      indirect_buffer[indirect_buffer_size_dwords++] =
         EVENT_TYPE(EVENT_TYPE_CS_PARTIAL_FLUSH) | EVENT_INDEX(4);
   }

   /* VK_PIPELINE_STAGE_2_COPY_BIT is flushed in command buffer ending. */

   /* TODO(Triang3l): VK_PIPELINE_STAGE_2_CLEAR_BIT. */

   cp_coher_cntl |= cp_coher_cntl_cb_db_dest_base_ena;
   if (cp_coher_cntl) {
      assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 5);
      indirect_buffer[indirect_buffer_size_dwords++] = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
      indirect_buffer[indirect_buffer_size_dwords++] =
         cp_coher_cntl | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
      indirect_buffer[indirect_buffer_size_dwords++] = UINT32_MAX;
      indirect_buffer[indirect_buffer_size_dwords++] = 0;
      indirect_buffer[indirect_buffer_size_dwords++] = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
   }

   /* Pad the GFX ring indirect buffer to the size alignment requirement with NOPs, and also prevent
    * the submission from being empty as it's still needed for the completion signal, but an empty
    * one may be rejected by the winsys.
    */
   while ((indirect_buffer_size_dwords &
           (TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1)) ||
          indirect_buffer_size_dwords == 0) {
      assert(TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS - indirect_buffer_size_dwords >= 1);
      indirect_buffer[indirect_buffer_size_dwords++] = PKT_TYPE_S(2);
   }

   return indirect_buffer_size_dwords;
}

static VkResult
terakan_queue_submit(struct vk_queue * const queue_base, struct vk_queue_submit * const submit)
{
   struct terakan_queue * const queue = container_of(queue_base, struct terakan_queue, vk);

   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);
   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   uint64_t submit_t0 = 0;
   struct terakan_instance const * const inst =
      container_of(physical_device->vk.base.instance,
                   struct terakan_instance const, vk);
   bool const profiling = inst->debug_flags & TERAKAN_DEBUG_PROFILE;
   if (profiling)
      submit_t0 = terakan_profile_now_ns();

   if (submit->wait_count != 0) {
      struct vk_sync_wait * const cpu_waits = alloca(sizeof(*cpu_waits) * submit->wait_count);
      struct vk_sync_wait * const wsi_waits = alloca(sizeof(*wsi_waits) * submit->wait_count);
      uint32_t cpu_wait_count = 0;
      uint32_t wsi_wait_count = 0;

      for (uint32_t wait_index = 0; wait_index < submit->wait_count; ++wait_index) {
         struct vk_sync_wait const * const wait = &submit->waits[wait_index];
         bool is_wsi_wait = false;
         if (wait->sync->type == &terakan_sync_completion_type) {
            struct terakan_sync_completion const * const sync =
               container_of(wait->sync, struct terakan_sync_completion const, vk);
            is_wsi_wait = sync->presentation_wait_is_wsi;
         }
         if (is_wsi_wait) {
            wsi_waits[wsi_wait_count++] = *wait;
         } else {
            cpu_waits[cpu_wait_count++] = *wait;
         }
      }

      VkResult result = VK_SUCCESS;
      if (cpu_wait_count != 0) {
         result = vk_sync_wait_many(&device->vk, cpu_wait_count, cpu_waits,
                                    VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
         result = terakan_queue_handle_wait_result(device, result, "submission CPU waits");
         if (result != VK_SUCCESS) {
            return result;
         }
      }

      if (wsi_wait_count != 0) {
         bool wsi_hw_wait_supported;
         result = terakan_queue_ensure_wsi_hw_wait_supported(queue, &wsi_hw_wait_supported);
         if (result != VK_SUCCESS) {
            return result;
         }

         if (!wsi_hw_wait_supported) {
            result = vk_sync_wait_many(&device->vk, wsi_wait_count, wsi_waits,
                                       VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
            result = terakan_queue_handle_wait_result(device, result, "WSI acquire waits");
            if (result != VK_SUCCESS) {
               return result;
            }
         } else {
            struct terakan_queue_wsi_wait * const unique_wsi_waits =
               alloca(sizeof(*unique_wsi_waits) * wsi_wait_count);
            uint32_t unique_wsi_wait_count = 0;
            for (uint32_t wait_index = 0; wait_index < wsi_wait_count; ++wait_index) {
               struct terakan_sync_completion const * const sync =
                  container_of(wsi_waits[wait_index].sync, struct terakan_sync_completion const, vk);
               assert(sync->presentation_wait_bo != NULL);
               uint32_t unique_wait_index;
               for (unique_wait_index = 0; unique_wait_index < unique_wsi_wait_count;
                    ++unique_wait_index) {
                  if (unique_wsi_waits[unique_wait_index].bo == sync->presentation_wait_bo) {
                     unique_wsi_waits[unique_wait_index].value =
                        MAX2(unique_wsi_waits[unique_wait_index].value,
                             sync->presentation_wait_value);
                     break;
                  }
               }
               if (unique_wait_index == unique_wsi_wait_count) {
                  unique_wsi_waits[unique_wsi_wait_count++] = (struct terakan_queue_wsi_wait) {
                     .bo = sync->presentation_wait_bo,
                     .value = sync->presentation_wait_value,
                  };
               }
            }

            result = terakan_queue_submit_wsi_wait_indirect_buffer(
               queue, unique_wsi_wait_count, unique_wsi_waits, 0xFF);
            if (result != VK_SUCCESS) {
               vk_device_set_lost(&device->vk,
                                  "WSI hardware wait submission failed with result %s",
                                  vk_Result_to_str(result));
               mtx_lock(&device->completion_mutex);
               device->completion_lost = true;
               mtx_unlock(&device->completion_mutex);
               cnd_broadcast(&device->completion_condition);
               return VK_ERROR_DEVICE_LOST;
            }
         }
      }
   }

   /* Update submission-time allocations. */

   uint64_t await_internal_bo_timeline_value = 0;

   uint32_t shader_ring_bytes_needed_for_se_shr8[TERAKAN_SHADER_RING_INDEX_COUNT] = {};
   for (uint32_t command_buffer_index = 0; command_buffer_index < submit->command_buffer_count;
        ++command_buffer_index) {
      struct terakan_command_buffer const * const command_buffer = container_of(
         submit->command_buffers[command_buffer_index], struct terakan_command_buffer const, vk);
      for (size_t shader_ring_index = 0; shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT;
           ++shader_ring_index) {
         shader_ring_bytes_needed_for_se_shr8[shader_ring_index] =
            MAX2(command_buffer->shader_ring_bytes_needed_for_se_shr8[shader_ring_index],
                 shader_ring_bytes_needed_for_se_shr8[shader_ring_index]);
      }
   }
   uint32_t shader_ring_bytes_needed_total_shr8 = 0;
   uint32_t shader_ring_offsets_shr8[TERAKAN_SHADER_RING_INDEX_COUNT] = {};
   for (size_t shader_ring_index = 0; shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT;
        ++shader_ring_index) {
      shader_ring_offsets_shr8[shader_ring_index] = shader_ring_bytes_needed_total_shr8;
      shader_ring_bytes_needed_total_shr8 +=
         shader_ring_bytes_needed_for_se_shr8[shader_ring_index]
         << (unsigned)(physical_device->chip_info.two_shader_engines_max &&
                       (TERAKAN_SHADER_RINGS_PER_SHADER_ENGINE & BITFIELD_BIT(shader_ring_index)));
   }
   if (queue->shader_rings_bytes_shr8 < shader_ring_bytes_needed_total_shr8) {
      await_internal_bo_timeline_value =
         MAX2(queue->shader_rings_last_usage, await_internal_bo_timeline_value);
   }

   if (await_internal_bo_timeline_value != 0) {
      VkResult const internal_bo_timeline_wait_result =
         vk_sync_wait(&device->vk, queue->internal_bo_timeline, await_internal_bo_timeline_value,
                      VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (internal_bo_timeline_wait_result != VK_SUCCESS) {
         if (internal_bo_timeline_wait_result == VK_ERROR_OUT_OF_HOST_MEMORY ||
             internal_bo_timeline_wait_result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            return vk_error(device, internal_bo_timeline_wait_result);
         }
         vk_device_set_lost(
            &device->vk,
            "Failed to await completion of submissions referencing the queue's internal "
            "allocations with result %s",
            vk_Result_to_str(internal_bo_timeline_wait_result));
         mtx_lock(&device->completion_mutex);
         device->completion_lost = true;
         mtx_unlock(&device->completion_mutex);
         cnd_broadcast(&device->completion_condition);
         return VK_ERROR_DEVICE_LOST;
      }
   }

   if (queue->shader_rings_bytes_shr8 < shader_ring_bytes_needed_total_shr8) {
      queue->shader_rings_bytes_shr8 = 0;
      if (queue->shader_rings != NULL) {
         terakan_bo_free(queue->shader_rings, NULL);
         queue->shader_rings = NULL;
      }

      VkResult const shader_rings_allocate_result = device->winsys_fn->bo->allocate_device_memory(
         device, (VkDeviceSize)shader_ring_bytes_needed_total_shr8 << 8, 0x100,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
         &queue->shader_rings);
      if (shader_rings_allocate_result != VK_SUCCESS) {
         return vk_error(device, shader_rings_allocate_result);
      }
      queue->shader_rings_bytes_shr8 = shader_ring_bytes_needed_total_shr8;
      queue->shader_rings_last_usage = 0;
   }

   /* Insert submission-time allocations into the command buffers.
    *
    * VUID-vkQueueSubmit-pCommandBuffers-00070: "Each element of the pCommandBuffers member of each
    * element of pSubmits must be in the pending or executable state"
    *
    * However, in Terakan, there's only one GFX queue, for which vkQueueSubmit must be externally
    * synchronized, so it's safe to modify the command buffers here without additional
    * synchronization (such as locking before modifying, unlocking after submitting to the winsys)
    * or creating copies of them even if they are in the pending state.
    */

   if (shader_ring_bytes_needed_total_shr8 != 0) {
      uint32_t const shader_rings_va_shr8 = (uint32_t)(queue->shader_rings->va >> 8);
      for (uint32_t command_buffer_index = 0; command_buffer_index < submit->command_buffer_count;
           ++command_buffer_index) {
         struct terakan_command_buffer const * const command_buffer = container_of(
            submit->command_buffers[command_buffer_index], struct terakan_command_buffer const, vk);
         list_for_each_entry (struct terakan_command_buffer_indirect_buffer,
                              command_buffer_indirect_buffer, &command_buffer->indirect_buffers,
                              link) {
            if (command_buffer_indirect_buffer->shader_rings_bo_placeholder_reference !=
                UINT32_MAX) {
               device->winsys_fn->queue->create_bo_reference(
                  (char *)command_buffer_indirect_buffer->bo_references +
                     device->bo_reference_size *
                        command_buffer_indirect_buffer->shader_rings_bo_placeholder_reference,
                  queue->shader_rings, true, true, TERAKAN_BO_PRIORITY_SHADER_RINGS);
               for (size_t shader_ring_index = 0;
                    shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT; ++shader_ring_index) {
                  struct terakan_command_buffer_indirect_buffer_shader_ring * const
                     indirect_buffer_shader_ring =
                        &command_buffer_indirect_buffer->shader_rings[shader_ring_index];
                  if (indirect_buffer_shader_ring->set_base_argument_offsets_dwords[0] ==
                      UINT32_MAX) {
                     continue;
                  }
                  uint32_t const shader_ring_va_shr8 =
                     shader_rings_va_shr8 + shader_ring_offsets_shr8[shader_ring_index];
                  uint32_t const shader_ring_bytes_shr8 =
                     shader_ring_bytes_needed_for_se_shr8[shader_ring_index];
                  command_buffer_indirect_buffer->indirect_buffer
                     [indirect_buffer_shader_ring->set_base_argument_offsets_dwords[0]] =
                     shader_ring_va_shr8;
                  terakan_queue_replace_relocation_offset_for_32_bits(
                     physical_device->submission_info_gfx.base.relocation_type,
                     command_buffer_indirect_buffer->relocations,
                     indirect_buffer_shader_ring->set_base_relocation_handles[0],
                     shader_ring_va_shr8);
                  if (physical_device->chip_info.two_shader_engines_max &&
                      (TERAKAN_SHADER_RINGS_PER_SHADER_ENGINE & BITFIELD_BIT(shader_ring_index))) {
                     command_buffer_indirect_buffer->indirect_buffer
                        [indirect_buffer_shader_ring->set_base_argument_offsets_dwords[1]] =
                        shader_ring_va_shr8 + shader_ring_bytes_shr8;
                     terakan_queue_replace_relocation_offset_for_32_bits(
                        physical_device->submission_info_gfx.base.relocation_type,
                        command_buffer_indirect_buffer->relocations,
                        indirect_buffer_shader_ring->set_base_relocation_handles[1],
                        shader_ring_va_shr8 + shader_ring_bytes_shr8);
                  }
                  command_buffer_indirect_buffer
                     ->indirect_buffer[indirect_buffer_shader_ring->set_size_argument_offset_dwords] =
                     shader_ring_bytes_shr8;
               }
            }
         }
      }
   }

   /* Fence elision: ensure a pending completion exists so we can reference its BO in the last
    * draw IB, avoiding a separate signal ioctl later.
    */
   if (submit->command_buffer_count > 0 && queue->pending_completion == NULL) {
      mtx_lock(&device->completion_mutex);
      if (!list_is_empty(&queue->completion_submissions_free)) {
         queue->pending_completion = list_first_entry(
            &queue->completion_submissions_free,
            struct terakan_queue_completion_submission, link);
         list_del(&queue->pending_completion->link);
         mtx_unlock(&device->completion_mutex);
      } else {
         mtx_unlock(&device->completion_mutex);
         VkResult const alloc_result =
            device->winsys_fn->queue->completion_submission_alloc_and_init_winsys(
               queue, &queue->pending_completion);
         if (alloc_result != VK_SUCCESS) {
            queue->pending_completion = NULL;
            /* Non-fatal: fall back to separate signal submit. */
         } else {
            queue->pending_completion->queue = queue;
         }
      }
   }

   /* Find the last IB across all command buffers for fence elision merge. */
   struct terakan_command_buffer_indirect_buffer const * last_indirect_buffer = NULL;
   if (queue->pending_completion != NULL) {
      for (uint32_t cbi = 0; cbi < submit->command_buffer_count; ++cbi) {
         struct terakan_command_buffer const * const cb = container_of(
            submit->command_buffers[cbi], struct terakan_command_buffer const, vk);
         list_for_each_entry (struct terakan_command_buffer_indirect_buffer const, ib,
                              &cb->indirect_buffers, link) {
            last_indirect_buffer = ib;
         }
      }
   }

   /* Submit the command buffers. For the last IB, merge the completion BO reference and
    * conservative flush packets to enable fence elision.
    */
   for (uint32_t command_buffer_index = 0; command_buffer_index < submit->command_buffer_count;
        ++command_buffer_index) {
      struct terakan_command_buffer const * const command_buffer = container_of(
         submit->command_buffers[command_buffer_index], struct terakan_command_buffer const, vk);
      list_for_each_entry (struct terakan_command_buffer_indirect_buffer const,
                           command_buffer_indirect_buffer, &command_buffer->indirect_buffers,
                           link) {
         /* The winsys may not support empty indirect buffers. */
         assert(command_buffer_indirect_buffer->indirect_buffer_size_dwords != 0);

         VkResult command_buffer_submit_result;

         if (command_buffer_indirect_buffer == last_indirect_buffer) {
            /* Merge: append conservative flush packets and include completion BO reference.
             * This flush must be a superset of all possible signal IB flushes generated by
             * terakan_queue_get_graphics_signal_indirect_buffer() because we don't know the
             * signal stages at draw time. Single GFX ring assumption: all Terakan work is
             * serialized on one in-order graphics ring (no DMA/compute engines exposed).
             */
            uint32_t const orig_size = command_buffer_indirect_buffer->indirect_buffer_size_dwords;
            /* Conservative flush: CONTEXT_CONTROL(3) + CACHE_FLUSH(2) + PS_PARTIAL_FLUSH(2) +
             * VS_PARTIAL_FLUSH(2) + CS_PARTIAL_FLUSH(2) + SURFACE_SYNC(5) = 16 dw, padded to 16.
             */
            uint32_t const flush_dwords = 16;
            uint32_t const combined_size = orig_size + flush_dwords;
            /* Ensure 8-dword alignment. */
            uint32_t const aligned_size =
               (combined_size +
                (TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1)) &
               ~(TERAKAN_QUEUE_INDIRECT_BUFFER_SIZE_ALIGNMENT_DWORDS_GFX - 1);

            uint32_t * const combined_ib = alloca(aligned_size * sizeof(uint32_t));
            memcpy(combined_ib, command_buffer_indirect_buffer->indirect_buffer,
                   orig_size * sizeof(uint32_t));

            uint32_t n = orig_size;
            /* CONTEXT_CONTROL: disable register shadowing. */
            combined_ib[n++] = PKT3(PKT3_CONTEXT_CONTROL, 1, 0);
            combined_ib[n++] = (uint32_t)1 << 31;
            combined_ib[n++] = (uint32_t)1 << 31;
            /* Full cache flush + invalidate. */
            combined_ib[n++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
            combined_ib[n++] =
               EVENT_TYPE(EVENT_TYPE_CACHE_FLUSH_AND_INV_EVENT) | EVENT_INDEX(0);
            /* PS partial flush. */
            combined_ib[n++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
            combined_ib[n++] =
               EVENT_TYPE(EVENT_TYPE_PS_PARTIAL_FLUSH) | EVENT_INDEX(4);
            /* VS partial flush. */
            combined_ib[n++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
            combined_ib[n++] =
               EVENT_TYPE(EVENT_TYPE_VS_PARTIAL_FLUSH) | EVENT_INDEX(4);
            /* CS partial flush (compute stage signal coverage). */
            combined_ib[n++] = PKT3(PKT3_EVENT_WRITE, 1 - 1, 0);
            combined_ib[n++] =
               EVENT_TYPE(EVENT_TYPE_CS_PARTIAL_FLUSH) | EVENT_INDEX(4);
            /* SURFACE_SYNC: flush CB + DB + SH caches, full address range.
             * Includes CB0-CB11 DEST_BASE_ENA for full UAV coverage (matches signal IB superset).
             */
            combined_ib[n++] = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
            combined_ib[n++] =
               S_0085F0_CB_ACTION_ENA(1) | S_0085F0_DB_ACTION_ENA(1) |
               S_0085F0_SH_ACTION_ENA(1) |
               S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
               S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
               S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
               S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
               S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
               S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1) |
               S_0085F0_DB_DEST_BASE_ENA(1) |
               TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
            combined_ib[n++] = UINT32_MAX;
            combined_ib[n++] = 0;
            combined_ib[n++] = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
            /* NOP padding to alignment. */
            while (n < aligned_size) {
               combined_ib[n++] = PKT_TYPE_S(2);
            }

            /* Combined BO refs: original + completion BO. */
            uint32_t const combined_bo_count =
               command_buffer_indirect_buffer->bo_reference_count + 1;
            void * const combined_bo_refs =
               alloca(device->bo_reference_size * combined_bo_count);
            memcpy(combined_bo_refs, command_buffer_indirect_buffer->bo_references,
                   device->bo_reference_size *
                      command_buffer_indirect_buffer->bo_reference_count);
            device->winsys_fn->queue->completion_submission_create_bo_reference(
               queue->pending_completion,
               (char *)combined_bo_refs +
                  device->bo_reference_size *
                     command_buffer_indirect_buffer->bo_reference_count);

            command_buffer_submit_result = device->winsys_fn->queue->submit(
               queue->submission_context, combined_bo_count, combined_bo_refs,
               aligned_size, combined_ib,
               command_buffer_indirect_buffer->relocation_count,
               command_buffer_indirect_buffer->relocations);
            if (profiling) {
               device->profile.submit_ib_dwords += aligned_size;
               device->profile.submit_bo_refs += combined_bo_count;
            }
         } else {
            command_buffer_submit_result = device->winsys_fn->queue->submit(
               queue->submission_context,
               command_buffer_indirect_buffer->bo_reference_count,
               command_buffer_indirect_buffer->bo_references,
               command_buffer_indirect_buffer->indirect_buffer_size_dwords,
               command_buffer_indirect_buffer->indirect_buffer,
               command_buffer_indirect_buffer->relocation_count,
               command_buffer_indirect_buffer->relocations);
            if (profiling) {
               device->profile.submit_ib_dwords +=
                  command_buffer_indirect_buffer->indirect_buffer_size_dwords;
               device->profile.submit_bo_refs += command_buffer_indirect_buffer->bo_reference_count;
            }
         }

         if (command_buffer_submit_result != VK_SUCCESS) {
            /* Lose the device as the submission might have been done partially already, don't leave
             * it in an indeterminate state.
             */
            vk_device_set_lost(&device->vk, "Command buffer submission failed with result %s",
                               vk_Result_to_str(command_buffer_submit_result));
            /* Disarm fence elision: the pending_completion BO was never (or only partially)
             * referenced in a successful draw submit. Recycle it to the free list so it doesn't
             * get used for signal elision on a stale/never-submitted BO.
             */
            if (queue->pending_completion != NULL) {
               mtx_lock(&device->completion_mutex);
               list_add(&queue->pending_completion->link, &queue->completion_submissions_free);
               device->completion_lost = true;
               mtx_unlock(&device->completion_mutex);
               queue->pending_completion = NULL;
            } else {
               mtx_lock(&device->completion_mutex);
               device->completion_lost = true;
               mtx_unlock(&device->completion_mutex);
            }
            cnd_broadcast(&device->completion_condition);
            return VK_ERROR_DEVICE_LOST;
         }
      }
   }

   /* If there are semaphores to signal from this submission, signal the fence BO to await the
    * completion from the completion thread. */

   /* Construct the list of the timeline semaphores that need to be signaled, and gather the stages
    * for the dependency.
    */
   struct list_head completion_signals;
   list_inithead(&completion_signals);
   bool const internal_bo_timeline_signal_needed = shader_ring_bytes_needed_total_shr8 != 0;
   /* Handling both submission signals and the internal BO timeline semaphore signal in a similar
    * way, with the latter assumed to be the signal at the loop iteration `submit->signal_count`.
    */
   uint32_t const submit_and_internal_bo_timeline_signal_count =
      submit->signal_count + (uint32_t)internal_bo_timeline_signal_needed;
   for (uint32_t submit_signal_index = 0;
        submit_signal_index < submit_and_internal_bo_timeline_signal_count; ++submit_signal_index) {
      struct vk_sync * submit_signal_sync;
      uint64_t submit_signal_value;
      if (unlikely(submit_signal_index >= submit->signal_count)) {
         submit_signal_sync = queue->internal_bo_timeline;
         submit_signal_value = queue->internal_bo_timeline_next_value;
      } else {
         struct vk_sync_signal const * const submit_signal = &submit->signals[submit_signal_index];
         if (submit_signal->sync->type == &vk_sync_dummy_type) {
            continue;
         }
         submit_signal_sync = submit_signal->sync;
         submit_signal_value = submit_signal->signal_value;
      }
      assert(submit_signal_sync->type == &terakan_sync_completion_type);

      struct terakan_queue_completion_signal * completion_signal;
      mtx_lock(&device->completion_mutex);
      if (!list_is_empty(&queue->completion_signals_free)) {
         completion_signal = list_first_entry(&queue->completion_signals_free,
                                              struct terakan_queue_completion_signal, link);
         list_del(&completion_signal->link);
         mtx_unlock(&device->completion_mutex);
      } else {
         mtx_unlock(&device->completion_mutex);
         completion_signal = vk_alloc(
            &device->vk.alloc, sizeof(struct terakan_queue_completion_signal),
            alignof(struct terakan_queue_completion_signal), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
         if (completion_signal == NULL) {
            /* Lose the device as the submission has been done partially already, don't leave it in
             * an indeterminate state.
             */
            vk_device_set_lost(&device->vk,
                               "Failed to allocate memory for a submission completion signal");
            mtx_lock(&device->completion_mutex);
            list_splice(&completion_signals, &queue->completion_signals_free);
            device->completion_lost = true;
            mtx_unlock(&device->completion_mutex);
            cnd_broadcast(&device->completion_condition);
            return VK_ERROR_DEVICE_LOST;
         }
      }
      completion_signal->sync =
         container_of(submit_signal_sync, struct terakan_sync_completion, vk);
      completion_signal->value = submit_signal_value;
      list_add(&completion_signal->link, &completion_signals);
   }
   if (list_is_empty(&completion_signals)) {
      /* Nothing to signal. */
      return VK_SUCCESS;
   }

   /* Set up the completion fence. When fence elision is active (pending_completion exists),
    * we'll reuse it instead of allocating a new one. Only allocate for the fallback path.
    */
   struct terakan_queue_completion_submission * completion_submission = NULL;
   if (queue->pending_completion == NULL) {
      /* No pending completion from a draw IB — allocate for fallback signal submit. */
      mtx_lock(&device->completion_mutex);
      if (!list_is_empty(&queue->completion_submissions_free)) {
         completion_submission = list_first_entry(&queue->completion_submissions_free,
                                                  struct terakan_queue_completion_submission, link);
         list_del(&completion_submission->link);
         mtx_unlock(&device->completion_mutex);
      } else {
         mtx_unlock(&device->completion_mutex);
         VkResult const completion_submission_create_result =
            device->winsys_fn->queue->completion_submission_alloc_and_init_winsys(
               queue, &completion_submission);
         if (completion_submission_create_result != VK_SUCCESS) {
            /* Lose the device as the submission has been done partially already, don't leave it in
             * an indeterminate state.
             */
            vk_device_set_lost(&device->vk,
                               "Submission completion fence creation failed with result %s",
                               vk_Result_to_str(completion_submission_create_result));
            mtx_lock(&device->completion_mutex);
            list_splice(&completion_signals, &queue->completion_signals_free);
            device->completion_lost = true;
            mtx_unlock(&device->completion_mutex);
            cnd_broadcast(&device->completion_condition);
            return VK_ERROR_DEVICE_LOST;
         }
         completion_submission->queue = queue;
      }
      list_replace(&completion_signals, &completion_submission->signals);
   }

   /* Section 7.4.1. "Semaphore Signaling" of the Vulkan 1.3.277 specification says:
    *
    *     "When a batch is submitted to a queue via a queue submission, and it includes semaphores
    *     to be signaled, it defines a memory dependency on the batch, and defines semaphore signal
    *     operations which set the semaphores to the signaled state."
    *
    *     "The first synchronization scope includes every command submitted in the same batch. In
    *     the case of vkQueueSubmit2, the first synchronization scope is limited to the pipeline
    *     stage specified by VkSemaphoreSubmitInfo::stageMask. Semaphore signal operations that are
    *     defined by vkQueueSubmit or vkQueueSubmit2 additionally include all commands that occur
    *     earlier in submission order."
    *
    *     "The first access scope includes all memory access performed by the device."
    *
    * Make sure all writes and reads in the first synchronization scope are complete to prevent all
    * types of data hazards, and flush write caches to make written memory available.
    */

   VkPipelineStageFlags2 signal_stages = 0;
   for (uint32_t submit_signal_index = 0; submit_signal_index < submit->signal_count;
        ++submit_signal_index) {
      struct vk_sync_signal const * const submit_signal = &submit->signals[submit_signal_index];
      if (submit_signal->sync->type == &vk_sync_dummy_type) {
         continue;
      }
      signal_stages |= submit_signal->stage_mask;
   }
   signal_stages = vk_expand_src_stage_flags2(signal_stages);

   uint32_t sx_surface_sync_mask = 0b0;
   VkPipelineStageFlags2 shader_ring_signal_stages = 0;
   if (shader_ring_bytes_needed_total_shr8 != 0) {
      for (size_t shader_ring_index = 0; shader_ring_index < TERAKAN_SHADER_RING_INDEX_COUNT;
           ++shader_ring_index) {
         if (shader_ring_bytes_needed_for_se_shr8[shader_ring_index] == 0) {
            continue;
         }
         struct terakan_shader_ring const * const shader_ring_info =
            &terakan_shader_rings[shader_ring_index];
         sx_surface_sync_mask |= shader_ring_info->sx_surface_sync_mask;
         shader_ring_signal_stages |= shader_ring_info->stages;
      }
   }
   shader_ring_signal_stages = vk_expand_src_stage_flags2(shader_ring_signal_stages);

   /* Fence elision: if pending_completion's BO was already referenced in a draw IB (i.e., we
    * submitted command buffers in this call or a previous one), we can skip the separate signal
    * ioctl entirely. The completion thread will await the BO becoming idle from the draw submit.
    * For empty signal submits (no command buffers), use pending_completion from a prior draw call.
    */
   if (queue->pending_completion != NULL) {
      /* The completion BO was proactively included in a draw IB submission. Reuse it. */
      completion_submission = queue->pending_completion;
      queue->pending_completion = NULL;
      list_replace(&completion_signals, &completion_submission->signals);
      /* No separate GPU submission needed — the BO is already busy from the draw IB. */
   } else {
      /* No prior draw IB referenced a completion BO (e.g., first-ever signal-only submit).
       * Fall back to the legacy path: submit a signal IB to make the completion BO busy.
       */
      uint32_t signal_indirect_buffer[TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS];
      uint32_t const signal_indirect_buffer_size_dwords =
         terakan_queue_get_graphics_signal_indirect_buffer(
            device, signal_stages, shader_ring_signal_stages, sx_surface_sync_mask,
            signal_indirect_buffer);
      assert(signal_indirect_buffer_size_dwords != 0);

      VkResult const completion_submission_submit_result =
         device->winsys_fn->queue->completion_submission_submit(
            completion_submission, signal_indirect_buffer_size_dwords, signal_indirect_buffer);
      if (completion_submission_submit_result != VK_SUCCESS) {
         vk_device_set_lost(&device->vk,
                            "Submission completion fence signal submission failed with result %s",
                            vk_Result_to_str(completion_submission_submit_result));
         mtx_lock(&device->completion_mutex);
         list_splice(&completion_signals, &queue->completion_signals_free);
         list_inithead(&completion_submission->signals);
         list_add(&completion_submission->link, &queue->completion_submissions_free);
         device->completion_lost = true;
         mtx_unlock(&device->completion_mutex);
         cnd_broadcast(&device->completion_condition);
         return VK_ERROR_DEVICE_LOST;
      }
   }

   /* Update the pending values of the signaled semaphores once the kernel has received the
    * submission, make the submission completion awaiting thread await the completion of the new
    * submission, and wake pending signal waiting threads and the submission completion awaiting
    * thread.
    */

   mtx_lock(&device->completion_mutex);

   for (uint32_t submit_signal_index = 0; submit_signal_index < submit->signal_count;
        ++submit_signal_index) {
      struct vk_sync_signal const * const submit_signal = &submit->signals[submit_signal_index];
      if (submit_signal->sync->type == &vk_sync_dummy_type) {
         continue;
      }
      assert(submit_signal->sync->type == &terakan_sync_completion_type);
      struct terakan_sync_completion * const submit_signal_sync =
         container_of(submit_signal->sync, struct terakan_sync_completion, vk);
      assert(submit_signal_sync->pending_value < submit_signal->signal_value);
      submit_signal_sync->pending_value = submit_signal->signal_value;
   }

   if (internal_bo_timeline_signal_needed) {
      assert(queue->internal_bo_timeline->type == &terakan_sync_completion_type);
      struct terakan_sync_completion * const internal_bo_timeline_sync =
         container_of(queue->internal_bo_timeline, struct terakan_sync_completion, vk);
      assert(internal_bo_timeline_sync->pending_value < queue->internal_bo_timeline_next_value);
      internal_bo_timeline_sync->pending_value = queue->internal_bo_timeline_next_value;

      if (shader_ring_bytes_needed_total_shr8 != 0) {
         queue->shader_rings_last_usage = queue->internal_bo_timeline_next_value;
      }

      ++queue->internal_bo_timeline_next_value;
   }

   list_addtail(&completion_submission->link, &queue->completion_submissions_pending);

   mtx_unlock(&device->completion_mutex);
   cnd_broadcast(&device->completion_condition);

   if (profiling) {
      device->profile.submit_ns += terakan_profile_now_ns() - submit_t0;
      device->profile.submit_count++;
      terakan_profile_dump_and_reset(&device->profile);
   }

   return VK_SUCCESS;
}

void
terakan_queue_destroy(struct terakan_queue * const queue)
{
   struct terakan_device * const device =
      container_of(queue->vk.base.device, struct terakan_device, vk);

   if (queue->shader_rings != NULL) {
      terakan_bo_free(queue->shader_rings, NULL);
   }

   /* Clean up pending completion submission from fence elision. */
   if (queue->pending_completion != NULL) {
      device->winsys_fn->queue->completion_submission_finish_winsys_and_free(
         queue->pending_completion);
      queue->pending_completion = NULL;
   }

   vk_sync_destroy(&device->vk, queue->internal_bo_timeline);

   mtx_lock(&device->completion_mutex);
   queue->shutdown_competion_thread = true;
   mtx_unlock(&device->completion_mutex);
   cnd_broadcast(&device->completion_condition);
   thrd_join(queue->completion_thread, NULL);

   struct terakan_queue_completion_submission *completion_submission, *next_submission;
   struct terakan_queue_completion_signal *completion_signal, *next_signal;
   LIST_FOR_EACH_ENTRY_SAFE (completion_submission, next_submission,
                             &queue->completion_submissions_pending, link) {
      LIST_FOR_EACH_ENTRY_SAFE (completion_signal, next_signal, &completion_submission->signals,
                                link) {
         vk_free(&device->vk.alloc, completion_signal);
      }
      device->winsys_fn->queue->completion_submission_finish_winsys_and_free(completion_submission);
   }
   LIST_FOR_EACH_ENTRY_SAFE (completion_submission, next_submission,
                             &queue->completion_submissions_free, link) {
      device->winsys_fn->queue->completion_submission_finish_winsys_and_free(completion_submission);
   }
   LIST_FOR_EACH_ENTRY_SAFE (completion_signal, next_signal, &queue->completion_signals_free,
                             link) {
      vk_free(&device->vk.alloc, completion_signal);
   }

   device->winsys_fn->queue->release_submission_context(queue->submission_context);

   vk_queue_finish(&queue->vk);

   vk_free(&queue->vk.base.device->alloc, queue);
}

VkResult
terakan_queue_create(struct terakan_device * const device,
                     VkDeviceQueueCreateInfo const * const create_info,
                     uint32_t const index_in_family, struct terakan_queue ** const queue_out)
{
   VkResult result;

   struct terakan_queue * const queue =
      vk_alloc(&device->vk.alloc, sizeof(struct terakan_queue), alignof(struct terakan_queue),
               VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (queue == NULL) {
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result = vk_queue_init(&queue->vk, &device->vk, create_info, index_in_family);
   if (result != VK_SUCCESS) {
      goto fail_alloc;
   }

   struct terakan_physical_device const * const physical_device =
      terakan_device_physical_device(device);

   struct terakan_queue_submission_size desired_submission_size =
      terakan_command_buffer_optimal_submission_size_gfx(
         &physical_device->submission_info_gfx.base);
   desired_submission_size.indirect_buffer_dwords =
      MAX2(desired_submission_size.indirect_buffer_dwords,
           TERAKAN_QUEUE_SIGNAL_INDIRECT_BUFFER_MAX_DWORDS);

   result = device->winsys_fn->queue->acquire_submission_context(
      device, AMD_IP_GFX, desired_submission_size, &queue->submission_context);
   if (result != VK_SUCCESS) {
      result = vk_error(device, result);
      goto fail_queue;
   }

   list_inithead(&queue->completion_signals_free);
   list_inithead(&queue->completion_submissions_free);

   list_inithead(&queue->completion_submissions_pending);

   queue->shutdown_competion_thread = false;

   if (thrd_create(&queue->completion_thread, terakan_queue_completion_thread_func, queue) !=
       thrd_success) {
      result = vk_errorf(device, VK_ERROR_OUT_OF_HOST_MEMORY,
                         "Failed to create the submission completion thread");
      goto fail_submission_context;
   }

   struct vk_sync_type const * const * internal_bo_timeline_type;
   for (internal_bo_timeline_type = physical_device->vk.supported_sync_types;
        *internal_bo_timeline_type != NULL; ++internal_bo_timeline_type) {
      if ((*internal_bo_timeline_type)->features &
          (VK_SYNC_FEATURE_TIMELINE | VK_SYNC_FEATURE_CPU_WAIT)) {
         break;
      }
   }
   assert(*internal_bo_timeline_type != NULL);
   result = vk_sync_create(&device->vk, *internal_bo_timeline_type, VK_SYNC_IS_TIMELINE, 0,
                           &queue->internal_bo_timeline);
   if (result != VK_SUCCESS) {
      goto fail_completion_thread;
   }
   queue->internal_bo_timeline_next_value = 1;
   queue->wsi_hw_wait_probe_state = 0;

   queue->shader_rings_bytes_shr8 = 0;
   queue->shader_rings = NULL;
   queue->shader_rings_last_usage = 0;

   queue->vk.driver_submit = terakan_queue_submit;

   *queue_out = queue;
   return VK_SUCCESS;

fail_completion_thread:
   mtx_lock(&device->completion_mutex);
   queue->shutdown_competion_thread = true;
   mtx_unlock(&device->completion_mutex);
   cnd_broadcast(&device->completion_condition);
   thrd_join(queue->completion_thread, NULL);
fail_submission_context:
   device->winsys_fn->queue->release_submission_context(queue->submission_context);
fail_queue:
   vk_queue_finish(&queue->vk);
fail_alloc:
   vk_free(&device->vk.alloc, queue);
   return result;
}
