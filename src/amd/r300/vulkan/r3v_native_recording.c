/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V fail-closed recording surface: every core Vulkan 1.0 command
 * outside the qualified draw subset poisons its command buffer.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "vk_log.h"

#include <string.h>

/* The native command buffer executes only an installed fixed IB, and
 * the public surface in r3v_native_draw.c is the one route that
 * installs it through Vulkan recording.  Every other core 1.0 vkCmd*
 * entrypoint records R3V_NATIVE_REFUSAL_RESULT into the command
 * buffer: vkEndCommandBuffer returns the error, the buffer ends
 * INVALID, and the queue refuses it.  A native definition for the
 * whole core set also keeps the runtime's common bridges (render-pass
 * emulation, the 1.0-to-1.3 forwarders) from dispatching into table
 * slots the native link set leaves NULL.
 */
static void
r3v_native_cmd_poison(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   vk_command_buffer_set_error(cmd_buffer, R3V_NATIVE_REFUSAL_RESULT);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBeginQuery(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t query,
   VkQueryControlFlags flags)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindDescriptorSets(
   VkCommandBuffer commandBuffer,
   VkPipelineBindPoint pipelineBindPoint,
   VkPipelineLayout layout,
   uint32_t firstSet,
   uint32_t descriptorSetCount,
   const VkDescriptorSet *pDescriptorSets,
   uint32_t dynamicOffsetCount,
   const uint32_t *pDynamicOffsets)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBindIndexBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   VkIndexType indexType)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdBlitImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageBlit *pRegions,
   VkFilter filter)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearAttachments(
   VkCommandBuffer commandBuffer,
   uint32_t attachmentCount,
   const VkClearAttachment *pAttachments,
   uint32_t rectCount,
   const VkClearRect *pRects)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearColorImage(
   VkCommandBuffer commandBuffer,
   VkImage image,
   VkImageLayout imageLayout,
   const VkClearColorValue *pColor,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdClearDepthStencilImage(
   VkCommandBuffer commandBuffer,
   VkImage image,
   VkImageLayout imageLayout,
   const VkClearDepthStencilValue *pDepthStencil,
   uint32_t rangeCount,
   const VkImageSubresourceRange *pRanges)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer srcBuffer,
   VkBuffer dstBuffer,
   uint32_t regionCount,
   const VkBufferCopy *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyBufferToImage(
   VkCommandBuffer commandBuffer,
   VkBuffer srcBuffer,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkBufferImageCopy *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageCopy *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyImageToBuffer(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkBuffer dstBuffer,
   uint32_t regionCount,
   const VkBufferImageCopy *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdCopyQueryPoolResults(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t firstQuery,
   uint32_t queryCount,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize stride,
   VkQueryResultFlags flags)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDispatch(
   VkCommandBuffer commandBuffer,
   uint32_t groupCountX,
   uint32_t groupCountY,
   uint32_t groupCountZ)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDispatchIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndexed(
   VkCommandBuffer commandBuffer,
   uint32_t indexCount,
   uint32_t instanceCount,
   uint32_t firstIndex,
   int32_t vertexOffset,
   uint32_t firstInstance)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndexedIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   uint32_t drawCount,
   uint32_t stride)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdDrawIndirect(
   VkCommandBuffer commandBuffer,
   VkBuffer buffer,
   VkDeviceSize offset,
   uint32_t drawCount,
   uint32_t stride)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdEndQuery(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t query)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdExecuteCommands(
   VkCommandBuffer commandBuffer,
   uint32_t commandBufferCount,
   const VkCommandBuffer *pCommandBuffers)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdFillBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize size,
   uint32_t data)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdNextSubpass(
   VkCommandBuffer commandBuffer,
   VkSubpassContents contents)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdPipelineBarrier(
   VkCommandBuffer commandBuffer,
   VkPipelineStageFlags srcStageMask,
   VkPipelineStageFlags dstStageMask,
   VkDependencyFlags dependencyFlags,
   uint32_t memoryBarrierCount,
   const VkMemoryBarrier *pMemoryBarriers,
   uint32_t bufferMemoryBarrierCount,
   const VkBufferMemoryBarrier *pBufferMemoryBarriers,
   uint32_t imageMemoryBarrierCount,
   const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdPushConstants(
   VkCommandBuffer commandBuffer,
   VkPipelineLayout layout,
   VkShaderStageFlags stageFlags,
   uint32_t offset,
   uint32_t size,
   const void *pValues)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdResetEvent(
   VkCommandBuffer commandBuffer,
   VkEvent event,
   VkPipelineStageFlags stageMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdResetQueryPool(
   VkCommandBuffer commandBuffer,
   VkQueryPool queryPool,
   uint32_t firstQuery,
   uint32_t queryCount)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdResolveImage(
   VkCommandBuffer commandBuffer,
   VkImage srcImage,
   VkImageLayout srcImageLayout,
   VkImage dstImage,
   VkImageLayout dstImageLayout,
   uint32_t regionCount,
   const VkImageResolve *pRegions)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetBlendConstants(
   VkCommandBuffer commandBuffer,
   const float blendConstants[4])
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetDepthBias(
   VkCommandBuffer commandBuffer,
   float depthBiasConstantFactor,
   float depthBiasClamp,
   float depthBiasSlopeFactor)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetDepthBounds(
   VkCommandBuffer commandBuffer,
   float minDepthBounds,
   float maxDepthBounds)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetEvent(
   VkCommandBuffer commandBuffer,
   VkEvent event,
   VkPipelineStageFlags stageMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetLineWidth(
   VkCommandBuffer commandBuffer,
   float lineWidth)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetScissor(
   VkCommandBuffer commandBuffer,
   uint32_t firstScissor,
   uint32_t scissorCount,
   const VkRect2D *pScissors)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilCompareMask(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t compareMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilReference(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t reference)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetStencilWriteMask(
   VkCommandBuffer commandBuffer,
   VkStencilFaceFlags faceMask,
   uint32_t writeMask)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdSetViewport(
   VkCommandBuffer commandBuffer,
   uint32_t firstViewport,
   uint32_t viewportCount,
   const VkViewport *pViewports)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdUpdateBuffer(
   VkCommandBuffer commandBuffer,
   VkBuffer dstBuffer,
   VkDeviceSize dstOffset,
   VkDeviceSize dataSize,
   const void *pData)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdWaitEvents(
   VkCommandBuffer commandBuffer,
   uint32_t eventCount,
   const VkEvent *pEvents,
   VkPipelineStageFlags srcStageMask,
   VkPipelineStageFlags dstStageMask,
   uint32_t memoryBarrierCount,
   const VkMemoryBarrier *pMemoryBarriers,
   uint32_t bufferMemoryBarrierCount,
   const VkBufferMemoryBarrier *pBufferMemoryBarriers,
   uint32_t imageMemoryBarrierCount,
   const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   r3v_native_cmd_poison(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL
r3v_CmdWriteTimestamp(
   VkCommandBuffer commandBuffer,
   VkPipelineStageFlagBits pipelineStage,
   VkQueryPool queryPool,
   uint32_t query)
{
   r3v_native_cmd_poison(commandBuffer);
}

/* The one image shape is a linear color target with its whole
 * allocation committed at bind, so its sparse requirement set is
 * empty.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSparseMemoryRequirements(
   VkDevice _device, VkImage image,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
   *pSparseMemoryRequirementCount = 0;
}

