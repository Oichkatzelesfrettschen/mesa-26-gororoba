/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V graphics pipeline: creation admits exactly the state
 * vector whose lowering is the qualified triangle cell.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/compiler/r300_vertex_job_nir.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"

#include "vk_log.h"
#include "vk_pipeline_layout.h"
#include "vk_render_pass.h"
#include "vk_shader_module.h"

#include <string.h>

/* The render-pass shape whose one subpass the cell realizes: a single
 * B8G8R8A8_UNORM single-sample color attachment, cleared on load and
 * stored, referenced as the one color output of the one subpass.
 */
bool
r3v_native_render_pass_matches_cell(const struct vk_render_pass *pass)
{
   if (pass == NULL || pass->is_multiview || pass->attachment_count != 1 ||
       pass->subpass_count != 1)
      return false;
   const struct vk_render_pass_attachment *att = &pass->attachments[0];
   if (att->format != R3V_NATIVE_TARGET_FORMAT || att->samples != 1 ||
       att->load_op != VK_ATTACHMENT_LOAD_OP_CLEAR ||
       att->store_op != VK_ATTACHMENT_STORE_OP_STORE)
      return false;
   const struct vk_subpass *subpass = &pass->subpasses[0];
   return subpass->input_count == 0 && subpass->color_count == 1 &&
          subpass->color_attachments[0].attachment == 0 &&
          subpass->depth_stencil_attachment == NULL;
}

/* SPIR-V ingestion for the semantic front end: plain spirv_to_nir plus
 * the shared normalization, so the driver and the front-end calibration
 * tests prepare shaders through one path.  Specialization and stage
 * flags stay outside the admitted subset.
 */
static nir_shader *
stage_to_nir(const VkPipelineShaderStageCreateInfo *stage,
             mesa_shader_stage mesa_stage)
{
   VK_FROM_HANDLE(vk_shader_module, module, stage->module);
   if (module == NULL || stage->flags != 0 ||
       stage->pSpecializationInfo != NULL || stage->pName == NULL)
      return NULL;
   nir_shader *nir = r300_vertex_job_spirv_to_nir(
      (const uint32_t *)module->data, module->size / 4, mesa_stage,
      stage->pName);
   if (nir == NULL)
      return NULL;
   const char *reason;
   if (!r300_vertex_job_nir_normalize(nir, &reason)) {
      ralloc_free(nir);
      return NULL;
   }
   return nir;
}

/* The qualified fragment binary is the solid-green constant write, so
 * the semantic admission is a fragment program storing exactly
 * vec4(0, 1, 0, 1) to color output 0.
 */
static const uint32_t r3v_native_green_bits[4] = {
   0, 0x3f800000u, 0, 0x3f800000u,
};

/* The one vertex-input freedom the CPU executor covers: a single
 * binding of per-vertex F32 records with one location-0 attribute at
 * any offset the draw-time bound can prove readable.
 */
static enum r300_vertex_format_id
attribute_format_id(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R32_SFLOAT:
      return R300_VERTEX_FORMAT_F32_1;
   case VK_FORMAT_R32G32_SFLOAT:
      return R300_VERTEX_FORMAT_F32_2;
   case VK_FORMAT_R32G32B32_SFLOAT:
      return R300_VERTEX_FORMAT_F32_3;
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return R300_VERTEX_FORMAT_F32_4;
   default:
      return R300_VERTEX_FORMAT_INVALID;
   }
}

/* Semantic stage admission: the vertex module lowers to the CPU job IR
 * and the fragment module reads back as the qualified constant color.
 * The job leaves with input_format_id unassigned; the caller binds it
 * from the vertex-input state and validates the finished job.
 */
static bool
stages_build_vertex_job(const VkGraphicsPipelineCreateInfo *info,
                        struct r300_vertex_job *job)
{
   if (info->stageCount != 2)
      return false;
   const VkPipelineShaderStageCreateInfo *vertex = NULL;
   const VkPipelineShaderStageCreateInfo *fragment = NULL;
   for (uint32_t s = 0; s < 2; s++) {
      const VkPipelineShaderStageCreateInfo *stage = &info->pStages[s];
      if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT && vertex == NULL)
         vertex = stage;
      else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
               fragment == NULL)
         fragment = stage;
      else
         return false;
   }
   if (vertex == NULL || fragment == NULL)
      return false;

   const char *reason;
   bool admitted = false;
   nir_shader *vs = stage_to_nir(vertex, MESA_SHADER_VERTEX);
   if (vs != NULL) {
      admitted = r300_vertex_job_from_nir(vs, job, &reason);
      ralloc_free(vs);
   }
   if (!admitted)
      return false;

   uint32_t color_bits[4];
   admitted = false;
   nir_shader *fs = stage_to_nir(fragment, MESA_SHADER_FRAGMENT);
   if (fs != NULL) {
      admitted = r300_fragment_nir_constant_color(fs, color_bits, &reason);
      ralloc_free(fs);
   }
   return admitted &&
          memcmp(color_bits, r3v_native_green_bits,
                 sizeof(color_bits)) == 0;
}

/* The viewport/scissor pair is the pipeline's target-extent claim: both
 * name one extent inside the published maximum at offset zero, the draw
 * later requires it equal to the pass target's extent, and the
 * matching cell resolves its scissor words from it.  Every other fixed
 * state stays the cell's exact vector.
 */
static bool
fixed_state_matches_cell(const VkGraphicsPipelineCreateInfo *info,
                         uint32_t *target_width, uint32_t *target_height)
{
   const VkPipelineInputAssemblyStateCreateInfo *ia =
      info->pInputAssemblyState;
   if (ia == NULL || ia->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
       ia->primitiveRestartEnable != VK_FALSE)
      return false;

   const VkPipelineViewportStateCreateInfo *vp = info->pViewportState;
   if (vp == NULL || vp->viewportCount != 1 || vp->scissorCount != 1 ||
       vp->pViewports == NULL || vp->pScissors == NULL)
      return false;
   const VkViewport *viewport = &vp->pViewports[0];
   const VkRect2D *scissor = &vp->pScissors[0];
   const uint32_t width = scissor->extent.width;
   const uint32_t height = scissor->extent.height;
   if (width < 1 || width > R3V_NATIVE_TARGET_WIDTH || height < 1 ||
       height > R3V_NATIVE_TARGET_HEIGHT)
      return false;
   if (viewport->x != 0.0f || viewport->y != 0.0f ||
       viewport->width != (float)width ||
       viewport->height != (float)height ||
       viewport->minDepth != 0.0f || viewport->maxDepth != 1.0f)
      return false;
   if (scissor->offset.x != 0 || scissor->offset.y != 0)
      return false;

   const VkPipelineRasterizationStateCreateInfo *rs =
      info->pRasterizationState;
   if (rs == NULL || rs->depthClampEnable != VK_FALSE ||
       rs->rasterizerDiscardEnable != VK_FALSE ||
       rs->polygonMode != VK_POLYGON_MODE_FILL ||
       rs->cullMode != VK_CULL_MODE_NONE ||
       rs->depthBiasEnable != VK_FALSE || rs->lineWidth != 1.0f)
      return false;

   const VkPipelineMultisampleStateCreateInfo *ms = info->pMultisampleState;
   if (ms == NULL || ms->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
       ms->sampleShadingEnable != VK_FALSE ||
       ms->alphaToCoverageEnable != VK_FALSE ||
       ms->alphaToOneEnable != VK_FALSE ||
       (ms->pSampleMask != NULL && (ms->pSampleMask[0] & 1u) != 1u))
      return false;

   const VkPipelineColorBlendStateCreateInfo *cb = info->pColorBlendState;
   if (cb == NULL || cb->logicOpEnable != VK_FALSE ||
       cb->attachmentCount != 1)
      return false;
   const VkPipelineColorBlendAttachmentState *blend = &cb->pAttachments[0];
   if (blend->blendEnable != VK_FALSE ||
       blend->colorWriteMask != (VK_COLOR_COMPONENT_R_BIT |
                                 VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT |
                                 VK_COLOR_COMPONENT_A_BIT))
      return false;

   if (info->pTessellationState != NULL ||
       info->pDepthStencilState != NULL ||
       (info->pDynamicState != NULL &&
        info->pDynamicState->dynamicStateCount != 0))
      return false;

   /* The out-parameters publish on the single success return, so a
    * refused pipeline never leaves a validated-looking extent behind.
    */
   *target_width = width;
   *target_height = height;
   return true;
}

static VkResult
create_pipeline(struct r3v_native_device *device,
                const VkGraphicsPipelineCreateInfo *info,
                const VkAllocationCallbacks *pAllocator,
                VkPipeline *pPipeline)
{
   *pPipeline = VK_NULL_HANDLE;

   VK_FROM_HANDLE(vk_render_pass, pass, info->renderPass);
   VK_FROM_HANDLE(vk_pipeline_layout, layout, info->layout);

   const VkPipelineVertexInputStateCreateInfo *vi = info->pVertexInputState;
   enum r300_vertex_format_id format_id = R300_VERTEX_FORMAT_INVALID;
   if (vi != NULL && vi->vertexBindingDescriptionCount == 1 &&
       vi->vertexAttributeDescriptionCount == 1 &&
       vi->pVertexBindingDescriptions[0].binding == 0 &&
       vi->pVertexBindingDescriptions[0].inputRate ==
          VK_VERTEX_INPUT_RATE_VERTEX &&
       vi->pVertexAttributeDescriptions[0].location == 0 &&
       vi->pVertexAttributeDescriptions[0].binding == 0)
      format_id =
         attribute_format_id(vi->pVertexAttributeDescriptions[0].format);

   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics(format_id);
   uint32_t target_width = 0, target_height = 0;
   struct r300_vertex_job job;
   if (format == NULL || info->flags != 0 ||
       (vi->pVertexBindingDescriptions[0].stride != 0 &&
        vi->pVertexBindingDescriptions[0].stride <
           format->semantic_record_bytes) ||
       !stages_build_vertex_job(info, &job) ||
       !fixed_state_matches_cell(info, &target_width, &target_height) ||
       layout == NULL || layout->set_count != 0 ||
       layout->push_range_count != 0 ||
       !r3v_native_render_pass_matches_cell(pass) || info->subpass != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   job.input_format_id = (int)format_id;
   if (r300_cpu_vertex_job_validate(&job) != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_pipeline *pipeline =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pipeline),
                       VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->format_id = (int)format_id;
   pipeline->binding_stride = vi->pVertexBindingDescriptions[0].stride;
   pipeline->attribute_offset = vi->pVertexAttributeDescriptions[0].offset;
   pipeline->target_width = target_width;
   pipeline->target_height = target_height;
   pipeline->vertex_job = job;
   /* GPU-route admission metadata: the qualified TCL-bypass cell
    * delivers the raw attribute stream, so only the identity job
    * (LOAD_INPUT feeding the position store) is GPU-admissible; every
    * other admitted job executes on the CPU route.
    */
   pipeline->gpu_vertex_job_identity =
      job.instruction_count == 2 && job.constant_count == 0 &&
      job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT &&
      job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION &&
      job.instructions[1].src0 == job.instructions[0].dst;

   *pPipeline = r3v_native_pipeline_to_handle(pipeline);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateGraphicsPipelines(VkDevice _device, VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < createInfoCount; i++) {
      VkResult one = create_pipeline(device, &pCreateInfos[i], pAllocator,
                                     &pPipelines[i]);
      if (one != VK_SUCCESS && result == VK_SUCCESS)
         result = one;
   }
   return result;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyPipeline(VkDevice _device, VkPipeline _pipeline,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_pipeline, pipeline, _pipeline);

   if (pipeline == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, pipeline);
}
