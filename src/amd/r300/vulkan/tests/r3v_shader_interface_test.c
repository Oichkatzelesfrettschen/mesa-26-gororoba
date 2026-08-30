/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the shader-interface record: each fixture's
 * qualifiers read back per location and built-in, the two-stage link
 * admits the matched pairs and refuses each mismatch by name, and every
 * record's canonical serialization pins to a BLAKE3 digest, so a reader
 * that later drops a qualifier moves a pinned digest.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_native_reference_spirv.h"
#include "r3v_shader_interface.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS(array) (sizeof(array) / sizeof((array)[0]))

static struct r3v_shader_interface
read(const uint32_t *words, size_t count,
     enum r3v_shader_interface_stage stage)
{
   struct r3v_shader_interface out;
   const char *reason = NULL;
   bool admitted =
      r3v_shader_interface_from_spirv(words, count, "main", stage, &out,
                                      &reason);
   if (!admitted)
      fprintf(stderr, "unexpected refusal: %s\n", reason);
   assert(admitted);
   return out;
}

static const char *
refusal(const uint32_t *words, size_t count,
        enum r3v_shader_interface_stage stage)
{
   struct r3v_shader_interface out;
   const char *reason = NULL;
   bool admitted =
      r3v_shader_interface_from_spirv(words, count, "main", stage, &out,
                                      &reason);
   if (admitted) {
      char text[1024];
      r3v_shader_interface_serialize(&out, text, sizeof(text));
      fprintf(stderr, "unexpected admission:\n%s", text);
   }
   assert(!admitted && reason != NULL);
   return reason;
}

static const char *
link_refusal(const struct r3v_shader_interface *vs,
             const struct r3v_shader_interface *fs)
{
   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   bool linked = r3v_shader_interface_link(vs, fs, &link, &reason);
   assert(!linked && reason != NULL);
   return reason;
}

static void expect_reason(const char *actual, const char *expected)
{
   if (strcmp(actual, expected) != 0)
      fprintf(stderr, "reason %s, expected %s\n", actual, expected);
   assert(strcmp(actual, expected) == 0);
}

/* Digest pins: the serialization of each fixture record.  A pin is
 * the record's identity; a change here is a contract change and needs
 * the review that accompanies one. */
static void
expect_digest(const char *label, const char *text, const char *expected)
{
   blake3_hash hash;
   char hex[BLAKE3_OUT_LEN * 2 + 1];
   _mesa_blake3_compute(text, strlen(text), hash);
   _mesa_blake3_format(hex, hash);
   if (strcmp(hex, expected) != 0)
      fprintf(stderr, "%s digest %s, pinned %s\n%s", label, hex, expected,
              text);
   assert(strcmp(hex, expected) == 0);
}

static void
expect_interface_digest(const char *label,
                        const struct r3v_shader_interface *in,
                        const char *expected)
{
   char text[1024];
   size_t n = r3v_shader_interface_serialize(in, text, sizeof(text));
   assert(n < sizeof(text));
   expect_digest(label, text, expected);
}

static void
expect_link_digest(const char *label,
                   const struct r3v_shader_interface_link *in,
                   const char *expected)
{
   char text[2048];
   size_t n = r3v_shader_interface_link_serialize(in, text, sizeof(text));
   assert(n < sizeof(text));
   expect_digest(label, text, expected);
}

static const struct r3v_shader_interface_location *
one(const struct r3v_shader_interface_location *table, uint32_t l)
{
   assert(table[l].present);
   return &table[l];
}

/* glslang's implicit gl_PerVertex declares ClipDistance[1] and
 * CullDistance[1] in every vertex module; the record reports the
 * declared lengths. */
#define GLSLANG_DEFAULT_CLIP 1
#define GLSLANG_DEFAULT_CULL 1

static void test_reference_pair(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   assert(vs.position_declared);
   assert(vs.clip_distance_count == GLSLANG_DEFAULT_CLIP &&
          vs.cull_distance_count == GLSLANG_DEFAULT_CULL);
   const struct r3v_shader_interface_location *in0 = one(vs.inputs, 0);
   assert(in0->scalar == R3V_SHADER_INTERFACE_SCALAR_FLOAT32 &&
          in0->width == 4 && in0->component_mask == 0xf &&
          !in0->interpolation_declared);
   for (uint32_t l = 0; l < R3V_SHADER_INTERFACE_MAX_LOCATIONS; l++)
      assert(!vs.outputs[l].present);

   struct r3v_shader_interface fs = read(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(!fs.position_declared);
   const struct r3v_shader_interface_location *out0 = one(fs.outputs, 0);
   assert(out0->width == 4 && out0->component_mask == 0xf);
   for (uint32_t l = 0; l < R3V_SHADER_INTERFACE_MAX_LOCATIONS; l++)
      assert(!fs.inputs[l].present);

   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   assert(r3v_shader_interface_link(&vs, &fs, &link, &reason));
   assert(link.varying_mask == 0 && link.flat_mask == 0);
   expect_interface_digest("reference vertex", &vs,
      
      "ee7815a5a1861c39562801e812f2fb78515fd234c402abed1eb7587e6f3e4515");
   expect_link_digest("reference link", &link, 
      "daa725357c10c51f15797699cc8939b32e6e6a708c7668aefae2fa5b30fab1ba");
}

static void test_smooth_default(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_varying_spirv,
      WORDS(r3v_reference_vertex_varying_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface fs = read(
      r3v_reference_fragment_varying_spirv,
      WORDS(r3v_reference_fragment_varying_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   const struct r3v_shader_interface_location *o = one(vs.outputs, 0);
   const struct r3v_shader_interface_location *i = one(fs.inputs, 0);
   assert(!o->interpolation_declared && !i->interpolation_declared);
   assert(i->interpolation == R3V_SHADER_INTERFACE_SMOOTH);
   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   assert(r3v_shader_interface_link(&vs, &fs, &link, &reason));
   assert(link.varying_mask == 1 && link.flat_mask == 0 &&
          link.noperspective_mask == 0);
   assert(link.varyings[0].interpolation == R3V_SHADER_INTERFACE_SMOOTH);
   expect_link_digest("smooth link", &link, 
      "1572f4c33d0299106a6188e176d5f99861a969f539aec7c25843093f92fa38bc");
}

static void test_flat(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_flat_spirv, WORDS(r3v_reference_vertex_flat_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface fs = read(
      r3v_reference_fragment_flat_spirv,
      WORDS(r3v_reference_fragment_flat_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(vs.outputs, 0)->interpolation == R3V_SHADER_INTERFACE_FLAT &&
          one(vs.outputs, 0)->interpolation_declared);
   assert(one(fs.inputs, 0)->interpolation == R3V_SHADER_INTERFACE_FLAT &&
          one(fs.inputs, 0)->interpolation_declared);
   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   assert(r3v_shader_interface_link(&vs, &fs, &link, &reason));
   assert(link.varying_mask == 1 && link.flat_mask == 1);
   expect_link_digest("flat link", &link, 
      "096d66a97a78de50e41d8c32b8acea296e83aab07b05b111f7ace4eec9b5a7db");

   /* The fragment declaration alone governs: the smooth vertex module
    * of the varying pair links Flat against the Flat fragment. */
   struct r3v_shader_interface smooth_vs = read(
      r3v_reference_vertex_varying_spirv,
      WORDS(r3v_reference_vertex_varying_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   assert(r3v_shader_interface_link(&smooth_vs, &fs, &link, &reason));
   assert(link.flat_mask == 1);

   /* Known-bad: the Flat decoration stripped from the fragment module
    * reads back Smooth, and the linked digest moves. */
   uint32_t stripped[WORDS(r3v_reference_fragment_flat_spirv)];
   size_t n = WORDS(r3v_reference_fragment_flat_spirv);
   memcpy(stripped, r3v_reference_fragment_flat_spirv, sizeof(stripped));
   size_t at = 5, removed = 0;
   while (at < n) {
      uint32_t len = stripped[at] >> 16;
      if ((stripped[at] & 0xffffu) == 71 && len == 3 && stripped[at + 2] == 14) {
         memmove(&stripped[at], &stripped[at + len], (n - at - len) * 4);
         n -= len;
         removed++;
         continue;
      }
      at += len;
   }
   assert(removed == 1);
   struct r3v_shader_interface dropped = read(
      stripped, n, R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(dropped.inputs, 0)->interpolation ==
             R3V_SHADER_INTERFACE_SMOOTH &&
          !one(dropped.inputs, 0)->interpolation_declared);
   /* Against the Flat vertex module the vertex declaration governs. */
   assert(r3v_shader_interface_link(&vs, &dropped, &link, &reason));
   assert(link.flat_mask == 1);
   /* Against the smooth vertex module the pair is Smooth and the
    * digest is the smooth pair's, so the drop is visible. */
   assert(r3v_shader_interface_link(&smooth_vs, &dropped, &link, &reason));
   assert(link.flat_mask == 0);
   expect_link_digest("flat dropped", &link, 
      "1572f4c33d0299106a6188e176d5f99861a969f539aec7c25843093f92fa38bc");
}

static void test_mixed_and_noperspective(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_mixed_spirv,
      WORDS(r3v_reference_vertex_mixed_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface fs = read(
      r3v_reference_fragment_mixed_spirv,
      WORDS(r3v_reference_fragment_mixed_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(fs.inputs, 0)->interpolation == R3V_SHADER_INTERFACE_FLAT);
   assert(one(fs.inputs, 1)->interpolation == R3V_SHADER_INTERFACE_SMOOTH &&
          !one(fs.inputs, 1)->interpolation_declared);
   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   assert(r3v_shader_interface_link(&vs, &fs, &link, &reason));
   assert(link.varying_mask == 3 && link.flat_mask == 1 &&
          link.noperspective_mask == 0);
   expect_link_digest("mixed link", &link, 
      "e2d1a246537cf981c5b6fa245434267b14fb8223b2c06f3af6a1d70cdada409a");

   struct r3v_shader_interface np = read(
      r3v_reference_fragment_noperspective_spirv,
      WORDS(r3v_reference_fragment_noperspective_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(np.inputs, 0)->interpolation ==
          R3V_SHADER_INTERFACE_NOPERSPECTIVE);
   struct r3v_shader_interface smooth_vs = read(
      r3v_reference_vertex_varying_spirv,
      WORDS(r3v_reference_vertex_varying_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   assert(r3v_shader_interface_link(&smooth_vs, &np, &link, &reason));
   assert(link.noperspective_mask == 1 && link.flat_mask == 0);
   expect_link_digest("noperspective link", &link,
                      
      "98e4ad923b37a26d1ad67e5da801cbc8f56467d8c590a43d16337c7ff6c20086");

   /* The mixed carrier pair: Smooth at location 0 beside NoPerspective
    * at location 1, both float vec4, links with the NoPerspective mask
    * naming location 1 alone. */
   struct r3v_shader_interface mc_vs = read(
      r3v_reference_vertex_mixed_carrier_spirv,
      WORDS(r3v_reference_vertex_mixed_carrier_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface mc_fs = read(
      r3v_reference_fragment_mixed_carrier_spirv,
      WORDS(r3v_reference_fragment_mixed_carrier_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(r3v_shader_interface_link(&mc_vs, &mc_fs, &link, &reason));
   assert(link.varying_mask == 3 && link.flat_mask == 0 &&
          link.noperspective_mask == 2);
   assert(link.varyings[0].interpolation == R3V_SHADER_INTERFACE_SMOOTH &&
          link.varyings[1].interpolation ==
             R3V_SHADER_INTERFACE_NOPERSPECTIVE &&
          link.varyings[0].width == 4 && link.varyings[1].width == 4 &&
          link.varyings[0].component_mask == 0xf &&
          link.varyings[1].component_mask == 0xf);

   /* Conflicts: the Flat vertex output against the NoPerspective
    * fragment input, and both qualifiers on one fragment input. */
   struct r3v_shader_interface flat_vs = read(
      r3v_reference_vertex_flat_spirv, WORDS(r3v_reference_vertex_flat_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   expect_reason(link_refusal(&flat_vs, &np),
                 "interpolation qualifiers conflict across the stages");
   uint32_t both[WORDS(r3v_reference_fragment_noperspective_spirv) + 3];
   size_t n = WORDS(r3v_reference_fragment_noperspective_spirv);
   memcpy(both, r3v_reference_fragment_noperspective_spirv, n * 4);
   size_t at = 5;
   bool patched = false;
   while (at < n) {
      uint32_t len = both[at] >> 16;
      if ((both[at] & 0xffffu) == 71 && len == 3 && both[at + 2] == 13) {
         memmove(&both[at + 3], &both[at], (n - at) * 4);
         both[at] = (3u << 16) | 71u;
         both[at + 1] = both[at + 4];
         both[at + 2] = 14;
         n += 3;
         patched = true;
         break;
      }
      at += len;
   }
   assert(patched);
   expect_reason(refusal(both, n, R3V_SHADER_INTERFACE_STAGE_FRAGMENT),
                 "Flat and NoPerspective on one interface location");
}

static void test_blocks_and_components(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_block_spirv,
      WORDS(r3v_reference_vertex_block_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface fs = read(
      r3v_reference_fragment_block_spirv,
      WORDS(r3v_reference_fragment_block_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   /* Member 0 carries Flat through OpMemberDecorate and member 1 takes
    * the next location undecorated. */
   assert(one(vs.outputs, 0)->interpolation == R3V_SHADER_INTERFACE_FLAT);
   assert(one(vs.outputs, 1)->interpolation == R3V_SHADER_INTERFACE_SMOOTH);
   assert(one(fs.inputs, 0)->interpolation == R3V_SHADER_INTERFACE_FLAT);
   assert(one(fs.inputs, 1)->interpolation == R3V_SHADER_INTERFACE_SMOOTH);
   struct r3v_shader_interface_link link;
   const char *reason = NULL;
   assert(r3v_shader_interface_link(&vs, &fs, &link, &reason));
   assert(link.varying_mask == 3 && link.flat_mask == 1);
   /* The block pair declares the same boundary as the mixed pair. */
   expect_link_digest("block link", &link, 
      "e2d1a246537cf981c5b6fa245434267b14fb8223b2c06f3af6a1d70cdada409a");

   struct r3v_shader_interface cvs = read(
      r3v_reference_vertex_components_spirv,
      WORDS(r3v_reference_vertex_components_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface cfs = read(
      r3v_reference_fragment_components_spirv,
      WORDS(r3v_reference_fragment_components_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(cvs.outputs, 0)->component_mask == 0xf &&
          one(cvs.outputs, 0)->width == 4);
   assert(one(cfs.inputs, 0)->component_mask == 0xf);
   assert(r3v_shader_interface_link(&cvs, &cfs, &link, &reason));
   /* Two vec2 objects over one location declare the same boundary as
    * one smooth vec4, so the pin is the smooth pair's. */
   expect_link_digest("components link", &link, 
      "1572f4c33d0299106a6188e176d5f99861a969f539aec7c25843093f92fa38bc");
}

static void test_clip_cull(void)
{
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_clip_cull_spirv,
      WORDS(r3v_reference_vertex_clip_cull_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   assert(vs.position_declared && vs.clip_distance_count == 3 &&
          vs.cull_distance_count == 2);
   expect_interface_digest("clip cull vertex", &vs, 
      "cab13441b0141ecade6b4ee2212af9bd5171fc57a41fcfd0de930f283a21100c");

   /* Known-bad: the ClipDistance array length constant patched from 3
    * to 7 makes 7 + 2 exceed the combined budget of 8.  The length is
    * the unsigned OpConstant the OpTypeArray of float names. */
   uint32_t excess[WORDS(r3v_reference_vertex_clip_cull_spirv)];
   size_t n = WORDS(r3v_reference_vertex_clip_cull_spirv);
   memcpy(excess, r3v_reference_vertex_clip_cull_spirv, sizeof(excess));
   size_t at = 5;
   uint32_t patched = 0;
   while (at < n) {
      uint32_t len = excess[at] >> 16;
      if ((excess[at] & 0xffffu) == 43 && len == 4 && excess[at + 3] == 3) {
         excess[at + 3] = 7;
         patched++;
      }
      at += len;
   }
   /* glslang also emits the signed constant 3 (a gl_PerVertex member
    * index), which the reader never consults. */
   assert(patched == 2);
   expect_reason(refusal(excess, n, R3V_SHADER_INTERFACE_STAGE_VERTEX),
                 "ClipDistance plus CullDistance outside the combined "
                 "budget of 8");
}

static void test_link_refusals(void)
{
   struct r3v_shader_interface bare_vs = read(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface bare_fs = read(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   struct r3v_shader_interface flat_vs = read(
      r3v_reference_vertex_flat_spirv, WORDS(r3v_reference_vertex_flat_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface flat_fs = read(
      r3v_reference_fragment_flat_spirv,
      WORDS(r3v_reference_fragment_flat_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   expect_reason(link_refusal(&bare_vs, &flat_fs),
                 "fragment input without a vertex output at its location");
   expect_reason(link_refusal(&flat_vs, &bare_fs),
                 "vertex output without a fragment consumer");

   struct r3v_shader_interface scalar_vs = read(
      r3v_reference_vertex_scalar_spirv,
      WORDS(r3v_reference_vertex_scalar_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface scalar_fs = read(
      r3v_reference_fragment_scalar_spirv,
      WORDS(r3v_reference_fragment_scalar_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(scalar_vs.outputs, 0)->width == 1 &&
          one(scalar_vs.outputs, 0)->component_mask == 0x1);
   /* A float scalar links: the vertex carrier executes float lanes of
    * any width, and the route selector judges the mask. */
   {
      struct r3v_shader_interface_link scalar_link;
      const char *scalar_reason = NULL;
      assert(r3v_shader_interface_link(&scalar_vs, &scalar_fs, &scalar_link,
                                       &scalar_reason));
      assert(scalar_link.varyings[0].present &&
             scalar_link.varyings[0].width == 1 &&
             scalar_link.varyings[0].component_mask == 0x1);
   }
   expect_reason(link_refusal(&scalar_vs, &flat_fs),
                 "vertex output and fragment input shapes differ");

   struct r3v_shader_interface int_vs = read(
      r3v_reference_vertex_int_spirv, WORDS(r3v_reference_vertex_int_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   struct r3v_shader_interface int_fs = read(
      r3v_reference_fragment_int_spirv,
      WORDS(r3v_reference_fragment_int_spirv),
      R3V_SHADER_INTERFACE_STAGE_FRAGMENT);
   assert(one(int_vs.outputs, 0)->scalar == R3V_SHADER_INTERFACE_SCALAR_INT32);
   /* Both stages declare Flat, so the integer pair refuses on
    * execution alone. */
   expect_reason(link_refusal(&int_vs, &int_fs),
                 "varying outside the float lanes the vertex carrier "
                 "executes");
   /* With the fragment's Flat stripped and the vertex's absent the
    * integer pair refuses on the missing Flat first. */
   struct r3v_shader_interface int_smooth_vs = int_vs;
   struct r3v_shader_interface int_smooth_fs = int_fs;
   int_smooth_vs.outputs[0].interpolation_declared = false;
   int_smooth_vs.outputs[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   int_smooth_fs.inputs[0].interpolation_declared = false;
   int_smooth_fs.inputs[0].interpolation = R3V_SHADER_INTERFACE_SMOOTH;
   expect_reason(link_refusal(&int_smooth_vs, &int_smooth_fs),
                 "integer varying without Flat");
   expect_reason(link_refusal(&flat_fs, &flat_vs),
                 "link outside a vertex and a fragment record");
}

static void test_parse_refusals(void)
{
   /* A Flat vertex input: the qualifier sits on a boundary that
    * carries none. */
   uint32_t flat_input[WORDS(r3v_reference_vertex_spirv) + 3];
   size_t n = WORDS(r3v_reference_vertex_spirv);
   memcpy(flat_input, r3v_reference_vertex_spirv, n * 4);
   size_t at = 5;
   bool patched = false;
   while (at < n) {
      uint32_t len = flat_input[at] >> 16;
      /* The Location 0 decoration of the position input: OpDecorate
       * with Location and value 0 on a vertex-stage Input. */
      if ((flat_input[at] & 0xffffu) == 71 && len == 4 &&
          flat_input[at + 2] == 30 && flat_input[at + 3] == 0) {
         memmove(&flat_input[at + 3], &flat_input[at], (n - at) * 4);
         flat_input[at] = (3u << 16) | 71u;
         flat_input[at + 1] = flat_input[at + 4];
         flat_input[at + 2] = 14;
         n += 3;
         patched = true;
         break;
      }
      at += len;
   }
   assert(patched);
   expect_reason(refusal(flat_input, n, R3V_SHADER_INTERFACE_STAGE_VERTEX),
                 "interpolation qualifier on a boundary that carries none");

   expect_reason(refusal(r3v_reference_vertex_spirv,
                         WORDS(r3v_reference_vertex_spirv),
                         R3V_SHADER_INTERFACE_STAGE_FRAGMENT),
                 "entry point outside the requested model");
   uint32_t header[4] = { 0 };
   expect_reason(refusal(header, 4, R3V_SHADER_INTERFACE_STAGE_VERTEX),
                 "module outside the SPIR-V header");
   /* The first instruction's length word stretched past the module. */
   uint32_t overrun[WORDS(r3v_reference_vertex_spirv)];
   memcpy(overrun, r3v_reference_vertex_spirv, sizeof(overrun));
   overrun[5] = (0xffffu << 16) | (overrun[5] & 0xffffu);
   expect_reason(refusal(overrun, WORDS(overrun),
                         R3V_SHADER_INTERFACE_STAGE_VERTEX),
                 "instruction length outside the module");

   /* Serialization reports the full length under a short buffer and
    * keeps the buffer terminated. */
   struct r3v_shader_interface vs = read(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv),
      R3V_SHADER_INTERFACE_STAGE_VERTEX);
   char full[512];
   size_t need = r3v_shader_interface_serialize(&vs, full, sizeof(full));
   char shortbuf[8];
   assert(r3v_shader_interface_serialize(&vs, shortbuf, sizeof(shortbuf)) ==
          need);
   assert(shortbuf[7] == '\0' && strncmp(shortbuf, full, 7) == 0);
   assert(r3v_shader_interface_serialize(&vs, NULL, 0) == need);
}

int main(void)
{
   test_reference_pair();
   test_smooth_default();
   test_flat();
   test_mixed_and_noperspective();
   test_blocks_and_components();
   test_clip_cull();
   test_link_refusals();
   test_parse_refusals();
   printf("r3v_shader_interface: reference, smooth default, flat, mixed, "
          "noperspective, block, component, clip/cull, link refusals, "
          "parse refusals, and serialization calibrated\n");
   return 0;
}
