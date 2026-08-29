/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V graphics pipeline: creation admits exactly the state
 * vector whose lowering is the qualified triangle cell.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "amd/r300/common/r300_us_source_read.h"
#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/vulkan/r3v_shader_interface.h"
#include "amd/r300/vulkan/r3v_vertex_spirv.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"

#include "vk_log.h"
#include "vk_pipeline_layout.h"
#include "vk_render_pass.h"
#include "vk_shader_module.h"

#include <string.h>

/* The render-pass shape whose one subpass the cell realizes: a single
 * single-sample color attachment in either render-family lane order,
 * cleared on load and stored, referenced as the one color output of
 * the one subpass.  The attachment format names a lane order; the
 * framebuffer's view then binds the image whose own format
 * r3v_CmdBeginRenderPass holds equal to it, so the emitted
 * US_OUT_FMT_0 payload matches the target the pass declares.
 */
bool
r3v_native_render_pass_matches_cell(const struct vk_render_pass *pass)
{
   if (pass == NULL || pass->is_multiview || pass->attachment_count != 1 ||
       pass->subpass_count != 1)
      return false;
   const struct vk_render_pass_attachment *att = &pass->attachments[0];
   enum r300_triangle_lane_order lanes;
   if (!r3v_native_render_lane_order(att->format, &lanes) ||
       att->samples != 1 ||
       att->load_op != VK_ATTACHMENT_LOAD_OP_CLEAR ||
       att->store_op != VK_ATTACHMENT_STORE_OP_STORE)
      return false;
   const struct vk_subpass *subpass = &pass->subpasses[0];
   return subpass->input_count == 0 && subpass->color_count == 1 &&
          subpass->color_attachments[0].attachment == 0 &&
          subpass->depth_stencil_attachment == NULL;
}

/* SPIR-V ingestion for the semantic front end: the direct word-stream
 * admitter in vulkan/r3v_vertex_spirv.c, so the driver reads the
 * module itself with no intermediate compiler representation.
 * Specialization and stage flags stay outside the admitted subset, and
 * the entry name is pinned to "main": the admitted grammar carries one
 * entry point, so the pin makes the pName-to-OpEntryPoint match hold
 * by construction.
 */
static const uint32_t *
stage_words(const VkPipelineShaderStageCreateInfo *stage,
            size_t *word_count)
{
   VK_FROM_HANDLE(vk_shader_module, module, stage->module);
   if (module == NULL || stage->flags != 0 ||
       stage->pSpecializationInfo != NULL || stage->pName == NULL ||
       strcmp(stage->pName, "main") != 0 || module->size % 4 != 0)
      return NULL;
   *word_count = module->size / 4;
   return (const uint32_t *)module->data;
}

/* The fragment binary carries its constant in four R300_PFS_PARAM_0
 * payloads, and the register encodes an FP24 value, so the admitted
 * constant is any RGBA whose four binary32 bit patterns already sit on
 * the generic FP24 storage lattice: r300_fp24_quantize_bits clears the low
 * seven mantissa bits, and a pattern it leaves unchanged reaches the
 * generic constant model exactly, so the color the target holds is the
 * color the module wrote.  An off-lattice pattern would round in the
 * register and render a value the program never named, so it refuses.
 */
static bool
constant_color_on_fp24_lattice(const uint32_t color_bits[4])
{
   for (unsigned c = 0; c < 4; c++) {
      if (r300_fp24_quantize_bits(color_bits[c]) != color_bits[c])
         return false;
   }
   return true;
}

/* The vertex-input freedom the CPU executor covers: per-vertex F32
 * records, each attribute the job reads at any offset the draw-time
 * bound can prove readable.
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

/* Vertex-input admission over the lowered job: every binding is
 * per-vertex or per-instance (the core instance rate, divisor one) with
 * its stride inside maxVertexInputBindingStride and
 * declared once; every attribute names a declared binding, a location
 * inside the job's slots, an F32 format, an offset inside
 * maxVertexInputAttributeOffset, and is declared once; and each
 * attribute the job reads closes inside its binding's stride when the
 * stride is nonzero, so consecutive records of one binding share no
 * bytes and the per-record bound arithmetic describes every read.  An
 * attribute the job leaves unread is admitted and carries no stream.
 * The job's read slots each take their format here; a read slot with
 * no attribute stays R300_VERTEX_FORMAT_INVALID and the job validation
 * that follows refuses it.
 */
static bool
vertex_input_admit(const VkPipelineVertexInputStateCreateInfo *vi,
                   struct r300_vertex_job *job,
                   struct r3v_native_pipeline *out)
{
   if (vi == NULL ||
       vi->vertexBindingDescriptionCount > R3V_NATIVE_MAX_VERTEX_BINDINGS ||
       vi->vertexAttributeDescriptionCount > R300_VERTEX_JOB_MAX_INPUTS)
      return false;
   uint32_t declared_bindings = 0;
   for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++) {
      const VkVertexInputBindingDescription *binding =
         &vi->pVertexBindingDescriptions[b];
      if (binding->binding >= R3V_NATIVE_MAX_VERTEX_BINDINGS ||
          (declared_bindings & (1u << binding->binding)) ||
          (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX &&
           binding->inputRate != VK_VERTEX_INPUT_RATE_INSTANCE) ||
          binding->stride > R3V_NATIVE_MAX_VERTEX_BINDING_STRIDE)
         return false;
      declared_bindings |= 1u << binding->binding;
      if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
         out->instance_rate_bindings |= 1u << binding->binding;
      out->binding_strides[binding->binding] = binding->stride;
   }
   const uint32_t read_mask = r300_vertex_job_input_mask(job);
   uint32_t declared_attributes = 0;
   for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++) {
      const VkVertexInputAttributeDescription *attribute =
         &vi->pVertexAttributeDescriptions[a];
      const enum r300_vertex_format_id format_id =
         attribute_format_id(attribute->format);
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(format_id);
      if (attribute->location >= R300_VERTEX_JOB_MAX_INPUTS ||
          (declared_attributes & (1u << attribute->location)) ||
          attribute->binding >= R3V_NATIVE_MAX_VERTEX_BINDINGS ||
          !(declared_bindings & (1u << attribute->binding)) ||
          format == NULL ||
          attribute->offset > R3V_NATIVE_MAX_VERTEX_ATTRIBUTE_OFFSET)
         return false;
      declared_attributes |= 1u << attribute->location;
      if (!(read_mask & (1u << attribute->location)))
         continue;
      const uint32_t stride = out->binding_strides[attribute->binding];
      if (stride != 0 &&
          (uint64_t)attribute->offset + format->semantic_record_bytes >
             stride)
         return false;
      out->attributes[attribute->location] =
         (struct r3v_native_vertex_attribute){
            .binding = attribute->binding,
            .offset = attribute->offset,
            .format_id = (int)format_id,
         };
      job->input_format_ids[attribute->location] = (int)format_id;
   }
   out->attribute_mask = read_mask;
   return true;
}

/* Semantic stage admission: the vertex module lowers to the CPU job IR
 * and the fragment module reads back as the shape the job's outputs
 * select -- the qualified constant color for a position-only job, the
 * varying pass-through for a job that stores the location-0 varying --
 * so a fragment program reading an unwritten varying or ignoring a
 * written one refuses the pipeline.  The job leaves with
 * input_format_ids unassigned; the caller binds them from the
 * vertex-input state and validates the finished job.
 */
static bool
stages_build_vertex_job(const VkGraphicsPipelineCreateInfo *info,
                        struct r300_vertex_job *job, bool *varying,
                        bool *sampled, uint32_t color_bits[4],
                        struct r3v_shader_interface_link *interface)
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
   size_t vs_words = 0;
   const uint32_t *vs_data = stage_words(vertex, &vs_words);
   size_t fs_words = 0;
   const uint32_t *fs_data = stage_words(fragment, &fs_words);
   if (vs_data == NULL || fs_data == NULL)
      return false;

   /* The stage boundary links first: every qualifier the modules
    * declare is recorded and matched across the two stages before the
    * job lowering reads the vertex body, so a qualifier the lowering
    * cannot yet execute refuses here by name rather than vanishing. */
   struct r3v_shader_interface vs_interface, fs_interface;
   if (!r3v_shader_interface_from_spirv(vs_data, vs_words, vertex->pName,
                                        R3V_SHADER_INTERFACE_STAGE_VERTEX,
                                        &vs_interface, &reason) ||
       !r3v_shader_interface_from_spirv(fs_data, fs_words, fragment->pName,
                                        R3V_SHADER_INTERFACE_STAGE_FRAGMENT,
                                        &fs_interface, &reason) ||
       !r3v_shader_interface_link(&vs_interface, &fs_interface, interface,
                                  &reason))
      return false;

   if (!r3v_vertex_job_from_spirv(vs_data, vs_words, vertex->pName, job,
                                   &reason))
      return false;
   *varying = r300_vertex_job_has_varying(job);
   *sampled = false;
   if (*varying) {
      if (r3v_fragment_varying_passthrough_from_spirv(
             fs_data, fs_words, fragment->pName, &reason))
         return true;
      /* The varying job's other fragment shape samples the set-0
       * binding-0 combined image sampler at the varying's xy. */
      *sampled = r3v_fragment_sampled_texture_from_spirv(
         fs_data, fs_words, fragment->pName, &reason);
      return *sampled;
   }
   return r3v_fragment_constant_color_from_spirv(fs_data, fs_words,
                                                  fragment->pName,
                                                  color_bits, &reason) &&
          constant_color_on_fp24_lattice(color_bits);
}

/* The viewport/scissor pair is the pipeline's target-extent claim: both
 * name one extent inside the published maximum at offset zero, the draw
 * later requires it equal to the pass target's extent, and the
 * matching cell resolves its scissor words from it.  Every other fixed
 * state stays the cell's exact vector.
 */
static bool
fixed_state_matches_cell(const VkGraphicsPipelineCreateInfo *info,
                         uint32_t *target_width, uint32_t *target_height,
                         bool *dynamic_out)
{
   const VkPipelineInputAssemblyStateCreateInfo *ia =
      info->pInputAssemblyState;
   if (ia == NULL || ia->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
       ia->primitiveRestartEnable != VK_FALSE)
      return false;

   /* Dynamic state admits the viewport/scissor pair together and
    * nothing else: the values then come from the vkCmdSet commands and
    * the draw holds them to the same cell shape the static form pins
    * here.
    */
   bool dynamic_viewport_scissor = false;
   const VkPipelineDynamicStateCreateInfo *dyn = info->pDynamicState;
   if (dyn != NULL && dyn->dynamicStateCount != 0) {
      if (dyn->dynamicStateCount != 2)
         return false;
      bool has_viewport = false, has_scissor = false;
      for (uint32_t i = 0; i < dyn->dynamicStateCount; i++) {
         if (dyn->pDynamicStates[i] == VK_DYNAMIC_STATE_VIEWPORT)
            has_viewport = true;
         else if (dyn->pDynamicStates[i] == VK_DYNAMIC_STATE_SCISSOR)
            has_scissor = true;
         else
            return false;
      }
      if (!has_viewport || !has_scissor)
         return false;
      dynamic_viewport_scissor = true;
   }

   const VkPipelineViewportStateCreateInfo *vp = info->pViewportState;
   if (vp == NULL || vp->viewportCount != 1 || vp->scissorCount != 1)
      return false;
   uint32_t width = 0, height = 0;
   if (!dynamic_viewport_scissor) {
      if (vp->pViewports == NULL || vp->pScissors == NULL)
         return false;
      const VkViewport *viewport = &vp->pViewports[0];
      const VkRect2D *scissor = &vp->pScissors[0];
      width = scissor->extent.width;
      height = scissor->extent.height;
      if (width < 1 || width > R3V_NATIVE_RENDER_MAX_EXTENT || height < 1 ||
          height > R3V_NATIVE_RENDER_MAX_EXTENT)
         return false;
      if (viewport->x != 0.0f || viewport->y != 0.0f ||
          viewport->width != (float)width ||
          viewport->height != (float)height ||
          viewport->minDepth != 0.0f || viewport->maxDepth != 1.0f)
         return false;
      if (scissor->offset.x != 0 || scissor->offset.y != 0)
         return false;
   }

   const VkPipelineRasterizationStateCreateInfo *rs =
      info->pRasterizationState;
   if (rs == NULL || rs->depthClampEnable != VK_FALSE ||
       rs->rasterizerDiscardEnable != VK_FALSE ||
       rs->polygonMode != VK_POLYGON_MODE_FILL ||
       (rs->cullMode & ~(VkCullModeFlags)VK_CULL_MODE_FRONT_AND_BACK) != 0 ||
       (rs->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE &&
        rs->frontFace != VK_FRONT_FACE_CLOCKWISE) ||
       rs->depthBiasEnable != VK_FALSE || rs->lineWidth != 1.0f)
      return false;

   /* Single-sample rasterization with the coverage semantics of the
    * one sample: a mask clearing bit 0 leaves the sample uncovered, so
    * the pipeline records it and the draw produces no fragment.
    */
   const VkPipelineMultisampleStateCreateInfo *ms = info->pMultisampleState;
   if (ms == NULL || ms->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
       ms->sampleShadingEnable != VK_FALSE ||
       ms->alphaToCoverageEnable != VK_FALSE ||
       ms->alphaToOneEnable != VK_FALSE)
      return false;

   /* The cell writes every fragment straight to the target, so the
    * admitted blend vocabulary is the arithmetic that equals that
    * write: blending only as the identity configuration (source
    * factor ONE, destination ZERO, op ADD on both channels), the logic
    * op only as COPY, and the write mask as all channels or none -- a
    * zero mask writes no channel, so the host collapses the draw's
    * triangles and the target keeps its clear.
    */
   const VkPipelineColorBlendStateCreateInfo *cb = info->pColorBlendState;
   if (cb == NULL || cb->attachmentCount != 1 ||
       (cb->logicOpEnable != VK_FALSE && cb->logicOp != VK_LOGIC_OP_COPY))
      return false;
   const VkPipelineColorBlendAttachmentState *blend = &cb->pAttachments[0];
   if (blend->blendEnable != VK_FALSE &&
       (blend->srcColorBlendFactor != VK_BLEND_FACTOR_ONE ||
        blend->dstColorBlendFactor != VK_BLEND_FACTOR_ZERO ||
        blend->colorBlendOp != VK_BLEND_OP_ADD ||
        blend->srcAlphaBlendFactor != VK_BLEND_FACTOR_ONE ||
        blend->dstAlphaBlendFactor != VK_BLEND_FACTOR_ZERO ||
        blend->alphaBlendOp != VK_BLEND_OP_ADD))
      return false;
   const VkColorComponentFlags full_mask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
   if (blend->colorWriteMask != full_mask && blend->colorWriteMask != 0)
      return false;

   /* The pass shape carries no depth/stencil attachment, so the only
    * depth/stencil state a valid program presents is the fully
    * disabled one; any enabled test, bounds, or write refuses until a
    * depth surface exists.
    */
   const VkPipelineDepthStencilStateCreateInfo *ds =
      info->pDepthStencilState;
   if (ds != NULL &&
       (ds->flags != 0 || ds->depthTestEnable != VK_FALSE ||
        ds->depthWriteEnable != VK_FALSE ||
        ds->depthBoundsTestEnable != VK_FALSE ||
        ds->stencilTestEnable != VK_FALSE))
      return false;

   /* pTessellationState is read only when the stages include both
    * tessellation shaders (VkGraphicsPipelineCreateInfo: the pointer is
    * ignored otherwise), and the stage admission above binds a vertex
    * and a fragment stage alone, so a supplied pointer carries no state
    * here and stays unread.
    */

   /* The out-parameters publish on the single success return, so a
    * refused pipeline never leaves a validated-looking extent behind;
    * a dynamic pipeline publishes zero and the draw resolves it.
    */
   *target_width = width;
   *target_height = height;
   *dynamic_out = dynamic_viewport_scissor;
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

   uint32_t target_width = 0, target_height = 0;
   bool dynamic_viewport_scissor = false;
   struct r300_vertex_job job;
   bool varying = false;
   bool sampled = false;
   uint32_t color_bits[4] = { 0 };
   struct r3v_native_pipeline admitted = { 0 };
   if (info->flags != 0 ||
       !stages_build_vertex_job(info, &job, &varying, &sampled,
                                color_bits, &admitted.shader_interface) ||
       !vertex_input_admit(info->pVertexInputState, &job, &admitted) ||
       r300_cpu_vertex_job_validate(&job) != 0 ||
       !fixed_state_matches_cell(info, &target_width, &target_height,
                                 &dynamic_viewport_scissor) ||
       layout == NULL || layout->push_range_count != 0 ||
       !r3v_native_render_pass_matches_cell(pass) || info->subpass != 0)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   /* The sampled module names set 0 binding 0, so its layout carries
    * exactly that combined-image-sampler binding under the fragment
    * stage; every other pipeline carries no set.
    */
   if (!sampled) {
      if (layout->set_count != 0)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   } else {
      if (layout->set_count != 1)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      const struct r3v_native_descriptor_set_layout *set0 =
         container_of(layout->set_layouts[0],
                      struct r3v_native_descriptor_set_layout, vk);
      const struct r3v_native_descriptor_layout_binding *b0 =
         &set0->bindings[0];
      if (!b0->present ||
          b0->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
          b0->count != 1 ||
          (b0->stages & ~(VkShaderStageFlags)
                           VK_SHADER_STAGE_FRAGMENT_BIT) != 0)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      for (uint32_t bi = 1; bi < R3V_NATIVE_DESCRIPTOR_BINDING_MAX; bi++) {
         if (set0->bindings[bi].present)
            return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      }
   }

   struct r3v_native_pipeline *pipeline =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*pipeline),
                       VK_OBJECT_TYPE_PIPELINE);
   if (pipeline == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   pipeline->attribute_mask = admitted.attribute_mask;
   memcpy(pipeline->attributes, admitted.attributes,
          sizeof(pipeline->attributes));
   memcpy(pipeline->binding_strides, admitted.binding_strides,
          sizeof(pipeline->binding_strides));
   pipeline->instance_rate_bindings = admitted.instance_rate_bindings;
   pipeline->cull_mode = info->pRasterizationState->cullMode;
   pipeline->front_face = info->pRasterizationState->frontFace;
   pipeline->sample_mask_zero =
      (info->pMultisampleState->pSampleMask != NULL &&
       (info->pMultisampleState->pSampleMask[0] & 1u) == 0) ||
      info->pColorBlendState->pAttachments[0].colorWriteMask == 0;
   pipeline->dynamic_viewport_scissor = dynamic_viewport_scissor;
   pipeline->target_width = target_width;
   pipeline->target_height = target_height;
   pipeline->vertex_job = job;
   pipeline->shader_interface = admitted.shader_interface;
   r3v_post_vs_lowering_from_interface(&pipeline->shader_interface,
                                       &pipeline->post_vs);
   /* The record-time route: a Flat interface executes on the CPU
    * route (the R2VB host model admits flat_mask == 0 alone), the
    * admission above pinned the triangle-list topology, the
    * pass-through fragment binary reads US input 0 -- the RS
    * destination color 0 lands in -- when the module pair carries the
    * varying without texture sampling, and RS480's GA_COLOR_CONTROL
    * carries PROVOKING_VERTEX_FIRST.  The clipping class is judged per
    * triangle at execution. */
   /* CPU delivery holds while every R2VB delivery gate stays closed;
    * a Flat interface executes on the CPU route regardless (the R2VB
    * host model's admission in r3v_native_cell.c requires
    * flat_mask == 0), and the NoPerspective route's partial-clip
    * refusal lives on the CPU route alone, so an open gate leaves a
    * NoPerspective interface UNSUPPORTED and its draws refuse. */
   const bool cpu_delivery = device->r2vb_delivery_gate == NULL &&
                             device->r2vb_gpu_delivery_gate == NULL &&
                             device->r2vb_fetched_gate == NULL;
   const struct r3v_interpolation_query interpolation = {
      .cpu_delivery = cpu_delivery || admitted.shader_interface.flat_mask != 0,
      .triangle_list = true,
      .clip_class = R3V_INTERPOLATION_CLIP_ACCEPT,
      .link = &pipeline->shader_interface,
      .rs_destination_available = varying && !sampled,
      .fragment_consumes_destination = varying && !sampled,
      .provoking_first_representable = true,
   };
   /* The Flat replication pin demotes the direct GA route alone: a
    * NoPerspective interface keeps its route, and an UNSUPPORTED mix
    * stays refused, under the pin. */
   pipeline->interpolation_route =
      r3v_interpolation_route_select(&interpolation, NULL);
   if (device->flat_replication_pin != NULL &&
       pipeline->interpolation_route ==
          R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0)
      pipeline->interpolation_route = R3V_INTERPOLATION_ROUTE_REPLICATE;
   /* The admission above refused any topology other than the triangle
    * list, so that predicate is the admission's. */
   const struct r3v_rs_probe_query probe = {
      .tex_adj_gate = device->rs_tex_adj_probe_gate != NULL,
      .w_select_gate = device->rs_w_select_probe_gate != NULL,
      .cpu_delivery = cpu_delivery,
      .triangle_list = true,
      .link = &pipeline->shader_interface,
      .rs_destination_available = varying && !sampled,
      .fragment_consumes_destination = varying && !sampled,
   };
   pipeline->rs_probe_candidate = r3v_rs_probe_candidate_select(&probe, NULL);
   /* A gated probe candidate owns the rasterizer control word: its
    * NoPerspective pass records the candidate word alone, so the
    * production W_SELECT route yields to it. */
   if (pipeline->rs_probe_candidate != R3V_RS_PROBE_NONE &&
       pipeline->interpolation_route ==
          R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT)
      pipeline->interpolation_route = R3V_INTERPOLATION_ROUTE_REPLICATE;
   pipeline->varying = varying;
   pipeline->sampled = sampled;
   memcpy(pipeline->color_bits, color_bits, sizeof(pipeline->color_bits));
   /* GPU-route admission metadata: the qualified TCL-bypass cell
    * delivers the raw attribute stream, so only the identity job
    * (LOAD_INPUT of slot 0 feeding the position store) over a
    * per-vertex slot-0 binding is GPU-admissible -- an instance-rate
    * binding repeats one record, which the linear source fetch cannot
    * express; every other admitted job executes on the CPU route.
    */
   pipeline->gpu_vertex_job_identity =
      job.instruction_count == 2 && job.constant_count == 0 &&
      job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT &&
      job.instructions[0].src0 == 0 &&
      job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION &&
      job.instructions[1].src0 == job.instructions[0].dst &&
      !(admitted.instance_rate_bindings &
        (1u << admitted.attributes[0].binding));

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
