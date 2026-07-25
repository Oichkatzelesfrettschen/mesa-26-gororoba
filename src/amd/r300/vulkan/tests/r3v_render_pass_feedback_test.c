/*
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for r3v render-pass self-feedback validation.
 */

#include <stdio.h>
#include <stdint.h>

#include "../r3v_render_pass.c"

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

/* Build a one-input subpass where attachment 0 is both the input attachment and
 * the depth/stencil reference so each case isolates the layout/aspect gate. */
static struct r3v_subpass
depth_input_subpass(VkImageLayout depth_layout,
                    VkImageLayout stencil_layout,
                    VkImageAspectFlags input_aspects)
{
   struct r3v_subpass sp = {
      .input_attachment_refs = {0},
      .input_attachment_aspects = {input_aspects},
      .input_attachment_count = 1,
      .depth_stencil_attachment_ref = 0,
      .depth_stencil_attachment_layout = depth_layout,
      .depth_stencil_stencil_layout = stencil_layout,
   };
   return sp;
}

/* Check that vkCreateRenderPass preserves maintenance2 input-aspect overrides
 * instead of deriving every sampled aspect from the attachment format. */
static void
check_input_aspect_override(void)
{
   struct r3v_render_pass rp = {
      .attachment_count = 1,
      .attachments = {
         [0] = {
            .format = VK_FORMAT_D24_UNORM_S8_UINT,
         },
      },
   };
   const VkInputAttachmentAspectReference refs[] = {
      {
         .subpass = 2,
         .inputAttachmentIndex = 1,
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
      },
   };
   const VkRenderPassInputAttachmentAspectCreateInfo aspect_info = {
      .sType =
         VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,
      .aspectReferenceCount = ARRAY_SIZE(refs),
      .pAspectReferences = refs,
   };

   CHECK(r3v_render_pass_input_attachment_aspects(
            &rp, &aspect_info, 2, 1, 0) == VK_IMAGE_ASPECT_DEPTH_BIT,
         "maintenance2 input aspect override is preserved");
   CHECK(r3v_render_pass_input_attachment_aspects(
            &rp, &aspect_info, 2, 0, 0) ==
         (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
         "unmatched input aspect override falls back to attachment format");
}

static void
check_subpass_storage_size(void)
{
   size_t storage_size = 1;
   CHECK(r3v_render_pass_subpass_storage_size(0, &storage_size) &&
         storage_size == 0,
         "zero subpasses require no storage");

   CHECK(r3v_render_pass_subpass_storage_size(3, &storage_size) &&
         storage_size == 3 * sizeof(struct r3v_subpass),
         "subpass storage size uses checked size_t multiplication");

   const size_t max_count = SIZE_MAX / sizeof(struct r3v_subpass);
   if (max_count < UINT32_MAX) {
      CHECK(r3v_render_pass_subpass_storage_size((uint32_t)max_count,
                                                    &storage_size) &&
            storage_size == max_count * sizeof(struct r3v_subpass),
            "maximum non-overflowing subpass count is accepted");
      CHECK(!r3v_render_pass_subpass_storage_size((uint32_t)max_count + 1,
                                                     &storage_size),
            "overflowing subpass storage size is rejected");
   } else {
      CHECK(r3v_render_pass_subpass_storage_size(UINT32_MAX,
                                                    &storage_size) &&
            storage_size == (size_t)UINT32_MAX * sizeof(struct r3v_subpass),
            "uint32 subpass count fits in wide size_t storage");
   }
}

/* Pin the same-subpass feedback matrix: writable output aliases reject, while
 * read-only depth/stencil aliases pass for the sampled aspect. */
int
main(void)
{
   struct r3v_subpass sp = {
      .color_attachment_refs = {0},
      .color_attachment_count = 1,
      .input_attachment_refs = {0},
      .input_attachment_aspects = {VK_IMAGE_ASPECT_COLOR_BIT},
      .input_attachment_count = 1,
      .depth_stencil_attachment_ref = VK_ATTACHMENT_UNUSED,
   };
   CHECK(r3v_subpass_self_dependency(&sp) == 0,
         "color attachment self-input is rejected");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == 0,
         "writable depth attachment self-input is rejected");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == VK_ATTACHMENT_UNUSED,
         "fully read-only depth/stencil self-input is accepted");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == VK_ATTACHMENT_UNUSED,
         "depth-read-only/stencil-writable layout accepts depth input");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_ASPECT_STENCIL_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == 0,
         "depth-read-only/stencil-writable layout rejects stencil input");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_STENCIL_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == VK_ATTACHMENT_UNUSED,
         "depth-writable/stencil-read-only layout accepts stencil input");

   sp = depth_input_subpass(
      vk_image_layout_depth_only(
         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL),
      vk_image_layout_stencil_only(
         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL),
      VK_IMAGE_ASPECT_STENCIL_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == VK_ATTACHMENT_UNUSED,
         "split depth-writable/stencil-read-only layout accepts stencil input");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == 0,
         "depth-writable/stencil-read-only layout rejects depth input");

   sp = depth_input_subpass(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_COLOR_BIT);
   CHECK(r3v_subpass_self_dependency(&sp) == 0,
         "mixed depth and non-depth/stencil aspect mask is rejected");

   check_input_aspect_override();
   check_subpass_storage_size();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }

   printf("OK\n");
   return 0;
}
