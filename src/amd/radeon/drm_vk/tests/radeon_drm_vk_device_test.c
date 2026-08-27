/*
 * SPDX-License-Identifier: MIT
 *
 * Host test for DRM_RADEON_INFO result-width routing.
 */

/* The asserts carry the test's side effects and verdicts, so they stay
 * live in NDEBUG builds.
 */
#undef NDEBUG

#include "radeon_drm_vk_device.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <radeon_drm.h>

struct radeon_info_mock {
   uint32_t calls;
   uint32_t last_request;
   int result_once;
};

static struct radeon_info_mock radeon_info_mock;

static uint32_t
radeon_info_u32_value(uint32_t request)
{
   return UINT32_C(0xa5000000) | request;
}

static uint64_t
radeon_info_u64_value(uint32_t request)
{
   return UINT64_C(0x1122334400000000) | request;
}

static bool
radeon_info_u32_reads_input(uint32_t request)
{
   switch (request) {
   case RADEON_INFO_CRTC_FROM_ID:
   case RADEON_INFO_WANT_HYPERZ:
   case RADEON_INFO_WANT_CMASK:
   case RADEON_INFO_RING_WORKING:
   case RADEON_INFO_READ_REG:
      return true;
   default:
      return false;
   }
}

static uint32_t
radeon_info_u32_input(uint32_t request)
{
   return UINT32_C(0x5a000000) | request;
}

static int
radeon_info_command_write_read(int fd, unsigned long command, void *data,
                               unsigned size)
{
   (void)fd;
   assert(command == DRM_RADEON_INFO);
   assert(size == sizeof(struct drm_radeon_info));

   struct drm_radeon_info *arguments = data;
   void *value = (void *)(uintptr_t)arguments->value;
   radeon_info_mock.calls++;
   radeon_info_mock.last_request = arguments->request;

   if (radeon_info_mock.result_once != 0) {
      int result = radeon_info_mock.result_once;
      radeon_info_mock.result_once = 0;
      memset(value, 0xa5, sizeof(uint64_t));
      return result;
   }

   switch (arguments->request) {
   case RADEON_INFO_TIMESTAMP:
   case RADEON_INFO_NUM_BYTES_MOVED:
   case RADEON_INFO_VRAM_USAGE:
   case RADEON_INFO_GTT_USAGE: {
      uint64_t input_value;
      memcpy(&input_value, value, sizeof(input_value));
      assert(input_value == 0);
      uint64_t value64 = radeon_info_u64_value(arguments->request);
      memcpy(value, &value64, sizeof(value64));
      break;
   }
   case RADEON_INFO_SI_TILE_MODE_ARRAY:
      for (uint32_t index = 0; index < 32; index++) {
         assert(((uint32_t *)value)[index] == 0);
         ((uint32_t *)value)[index] = UINT32_C(0x51000000) | index;
      }
      break;
   case RADEON_INFO_CIK_MACROTILE_MODE_ARRAY:
      for (uint32_t index = 0; index < 16; index++) {
         assert(((uint32_t *)value)[index] == 0);
         ((uint32_t *)value)[index] = UINT32_C(0xc1000000) | index;
      }
      break;
   default:
      if (radeon_info_u32_reads_input(arguments->request))
         assert(*(uint32_t *)value ==
                radeon_info_u32_input(arguments->request));
      else
         assert(*(uint32_t *)value == 0);
      *(uint32_t *)value = radeon_info_u32_value(arguments->request);
      break;
   }
   return 0;
}

static const struct radeon_drm_vk_ioctl_ops radeon_info_ops = {
   .command_write_read = radeon_info_command_write_read,
};

static void
radeon_info_device_init(struct radeon_drm_vk_device *device)
{
   memset(&radeon_info_mock, 0, sizeof(radeon_info_mock));
   assert(radeon_drm_vk_device_init(device, 42, &radeon_info_ops) == 0);
}

static const uint32_t radeon_info_u32_requests[] = {
   RADEON_INFO_DEVICE_ID,
   RADEON_INFO_NUM_GB_PIPES,
   RADEON_INFO_NUM_Z_PIPES,
   RADEON_INFO_ACCEL_WORKING,
   RADEON_INFO_CRTC_FROM_ID,
   RADEON_INFO_ACCEL_WORKING2,
   RADEON_INFO_TILING_CONFIG,
   RADEON_INFO_WANT_HYPERZ,
   RADEON_INFO_WANT_CMASK,
   RADEON_INFO_CLOCK_CRYSTAL_FREQ,
   RADEON_INFO_NUM_BACKENDS,
   RADEON_INFO_NUM_TILE_PIPES,
   RADEON_INFO_FUSION_GART_WORKING,
   RADEON_INFO_BACKEND_MAP,
   RADEON_INFO_VA_START,
   RADEON_INFO_IB_VM_MAX_SIZE,
   RADEON_INFO_MAX_PIPES,
   RADEON_INFO_MAX_SE,
   RADEON_INFO_MAX_SH_PER_SE,
   RADEON_INFO_FASTFB_WORKING,
   RADEON_INFO_RING_WORKING,
   RADEON_INFO_SI_CP_DMA_COMPUTE,
   RADEON_INFO_SI_BACKEND_ENABLED_MASK,
   RADEON_INFO_MAX_SCLK,
   RADEON_INFO_VCE_FW_VERSION,
   RADEON_INFO_VCE_FB_VERSION,
   RADEON_INFO_ACTIVE_CU_COUNT,
   RADEON_INFO_CURRENT_GPU_TEMP,
   RADEON_INFO_CURRENT_GPU_SCLK,
   RADEON_INFO_CURRENT_GPU_MCLK,
   RADEON_INFO_READ_REG,
   RADEON_INFO_VA_UNMAP_WORKING,
   RADEON_INFO_GPU_RESET_COUNTER,
};

static const uint32_t radeon_info_u64_requests[] = {
   RADEON_INFO_TIMESTAMP,
   RADEON_INFO_NUM_BYTES_MOVED,
   RADEON_INFO_VRAM_USAGE,
   RADEON_INFO_GTT_USAGE,
};

static const uint32_t radeon_info_array_requests[] = {
   RADEON_INFO_SI_TILE_MODE_ARRAY,
   RADEON_INFO_CIK_MACROTILE_MODE_ARRAY,
};

static void
test_u32_request_domain(void)
{

   struct radeon_drm_vk_device device;
   radeon_info_device_init(&device);
   for (uint32_t index = 0; index < ARRAY_SIZE(radeon_info_u32_requests);
        index++) {
      const uint32_t request = radeon_info_u32_requests[index];
      struct {
         uint32_t before;
         uint32_t value;
         uint32_t after;
      } guarded = {
         .before = UINT32_C(0x01020304),
         .value = radeon_info_u32_input(request),
         .after = UINT32_C(0x8899aabb),
      };
      assert(radeon_drm_vk_device_info_u32(&device, request,
                                           &guarded.value) == 0);
      assert(guarded.value == radeon_info_u32_value(request));
      assert(guarded.before == UINT32_C(0x01020304));
      assert(guarded.after == UINT32_C(0x8899aabb));
      assert(radeon_info_mock.last_request == request);
   }
   assert(radeon_info_mock.calls == ARRAY_SIZE(radeon_info_u32_requests));
   radeon_drm_vk_device_finish(&device);
}

static void
test_u64_request_domain(void)
{
   struct radeon_drm_vk_device device;
   radeon_info_device_init(&device);
   for (uint32_t index = 0; index < ARRAY_SIZE(radeon_info_u64_requests);
        index++) {
      const uint32_t request = radeon_info_u64_requests[index];
      struct {
         uint64_t before;
         uint64_t value;
         uint64_t after;
      } guarded = {
         .before = UINT64_C(0x0102030405060708),
         .value = UINT64_MAX,
         .after = UINT64_C(0x8899aabbccddeeff),
      };
      assert(radeon_drm_vk_device_info_u64(&device, request,
                                           &guarded.value) == 0);
      assert(guarded.value == radeon_info_u64_value(request));
      assert(guarded.before == UINT64_C(0x0102030405060708));
      assert(guarded.after == UINT64_C(0x8899aabbccddeeff));
   }
   assert(radeon_info_mock.calls == ARRAY_SIZE(radeon_info_u64_requests));
   radeon_drm_vk_device_finish(&device);
}

static void
test_array_request_domain(void)
{
   struct radeon_drm_vk_device device;
   radeon_info_device_init(&device);

   struct {
      uint64_t before;
      uint32_t values[32];
      uint64_t after;
   } si = {
      .before = UINT64_C(0x0102030405060708),
      .after = UINT64_C(0x8899aabbccddeeff),
   };
   memset(si.values, 0xff, sizeof(si.values));
   assert(radeon_drm_vk_device_info_u32_array(
             &device, RADEON_INFO_SI_TILE_MODE_ARRAY, si.values,
             ARRAY_SIZE(si.values)) == 0);
   for (uint32_t index = 0; index < ARRAY_SIZE(si.values); index++)
      assert(si.values[index] == (UINT32_C(0x51000000) | index));
   assert(si.before == UINT64_C(0x0102030405060708));
   assert(si.after == UINT64_C(0x8899aabbccddeeff));

   struct {
      uint64_t before;
      uint32_t values[16];
      uint64_t after;
   } cik = {
      .before = UINT64_C(0x1020304050607080),
      .after = UINT64_C(0x99aabbccddeeff00),
   };
   memset(cik.values, 0xff, sizeof(cik.values));
   assert(radeon_drm_vk_device_info_u32_array(
             &device, RADEON_INFO_CIK_MACROTILE_MODE_ARRAY, cik.values,
             ARRAY_SIZE(cik.values)) == 0);
   for (uint32_t index = 0; index < ARRAY_SIZE(cik.values); index++)
      assert(cik.values[index] == (UINT32_C(0xc1000000) | index));
   assert(cik.before == UINT64_C(0x1020304050607080));
   assert(cik.after == UINT64_C(0x99aabbccddeeff00));
   assert(radeon_info_mock.calls == 2);
   radeon_drm_vk_device_finish(&device);
}

static void
assert_array_rejected(struct radeon_drm_vk_device *device, uint32_t request,
                      uint32_t *values, const uint32_t *expected,
                      size_t preserved_count, size_t rejected_count)
{
   const uint32_t calls_before = radeon_info_mock.calls;
   assert(radeon_drm_vk_device_info_u32_array(
             device, request, values, rejected_count) == -EINVAL);
   assert(radeon_info_mock.calls == calls_before);
   assert(memcmp(values, expected,
                 preserved_count * sizeof(*values)) == 0);
}

static void
test_invalid_info_calls_fail_before_ioctl(void)
{
   struct radeon_drm_vk_device device;
   radeon_info_device_init(&device);
   uint32_t value32 = UINT32_C(0x12345678);
   uint64_t value64 = UINT64_C(0x0123456789abcdef);
   uint32_t values[33];
   for (uint32_t index = 0; index < ARRAY_SIZE(values); index++)
      values[index] = UINT32_C(0x80000000) | index;
   uint32_t expected[ARRAY_SIZE(values)];
   memcpy(expected, values, sizeof(expected));
   const uint32_t unknown_request = RADEON_INFO_GPU_RESET_COUNTER + 1;

   for (uint32_t index = 0; index < ARRAY_SIZE(radeon_info_u32_requests);
        index++) {
      assert(radeon_drm_vk_device_info_u64(
                &device, radeon_info_u32_requests[index], &value64) ==
             -EINVAL);
      assert(radeon_info_mock.calls == 0);
      assert_array_rejected(&device, radeon_info_u32_requests[index], values,
                            expected, ARRAY_SIZE(values), 1);
   }
   for (uint32_t index = 0; index < ARRAY_SIZE(radeon_info_u64_requests);
        index++) {
      assert(radeon_drm_vk_device_info_u32(
                &device, radeon_info_u64_requests[index], &value32) ==
             -EINVAL);
      assert(radeon_info_mock.calls == 0);
      assert_array_rejected(&device, radeon_info_u64_requests[index], values,
                            expected, ARRAY_SIZE(values), 2);
   }
   for (uint32_t index = 0; index < ARRAY_SIZE(radeon_info_array_requests);
        index++) {
      assert(radeon_drm_vk_device_info_u32(
                &device, radeon_info_array_requests[index], &value32) ==
             -EINVAL);
      assert(radeon_drm_vk_device_info_u64(
                &device, radeon_info_array_requests[index], &value64) ==
             -EINVAL);
      assert(radeon_info_mock.calls == 0);
   }

   assert_array_rejected(&device, RADEON_INFO_SI_TILE_MODE_ARRAY, values,
                         expected, ARRAY_SIZE(values), 0);
   assert_array_rejected(&device, RADEON_INFO_SI_TILE_MODE_ARRAY, values,
                         expected, ARRAY_SIZE(values), 31);
   assert_array_rejected(&device, RADEON_INFO_SI_TILE_MODE_ARRAY, values,
                         expected, ARRAY_SIZE(values), 33);
   assert_array_rejected(&device, RADEON_INFO_SI_TILE_MODE_ARRAY, values,
                         expected, ARRAY_SIZE(values), SIZE_MAX);
   assert_array_rejected(&device, RADEON_INFO_CIK_MACROTILE_MODE_ARRAY,
                         values, expected, ARRAY_SIZE(values), 0);
   assert_array_rejected(&device, RADEON_INFO_CIK_MACROTILE_MODE_ARRAY,
                         values, expected, ARRAY_SIZE(values), 15);
   assert_array_rejected(&device, RADEON_INFO_CIK_MACROTILE_MODE_ARRAY,
                         values, expected, ARRAY_SIZE(values), 17);
   assert_array_rejected(&device, RADEON_INFO_CIK_MACROTILE_MODE_ARRAY,
                         values, expected, ARRAY_SIZE(values), SIZE_MAX);

   assert(radeon_drm_vk_device_info_u32(
             &device, unknown_request, &value32) == -EINVAL);
   assert(radeon_drm_vk_device_info_u64(
             &device, unknown_request, &value64) == -EINVAL);
   assert_array_rejected(&device, unknown_request, values, expected,
                         ARRAY_SIZE(values), ARRAY_SIZE(values));

   assert(radeon_drm_vk_device_info_u32(&device, RADEON_INFO_DEVICE_ID,
                                        NULL) == -EINVAL);
   assert(radeon_drm_vk_device_info_u64(&device, RADEON_INFO_VRAM_USAGE,
                                        NULL) == -EINVAL);
   assert(radeon_drm_vk_device_info_u32_array(
             &device, RADEON_INFO_SI_TILE_MODE_ARRAY, NULL, 32) == -EINVAL);

   assert(radeon_info_mock.calls == 0);
   assert(value32 == UINT32_C(0x12345678));
   assert(value64 == UINT64_C(0x0123456789abcdef));
   assert(memcmp(values, expected, sizeof(values)) == 0);
   radeon_drm_vk_device_finish(&device);
}

static void
test_ioctl_failure_preserves_caller_value(void)
{
   struct radeon_drm_vk_device device;
   radeon_info_device_init(&device);

   uint32_t value32 = UINT32_C(0x12345678);
   radeon_info_mock.result_once = -EIO;
   assert(radeon_drm_vk_device_info_u32(
             &device, RADEON_INFO_DEVICE_ID, &value32) == -EIO);
   assert(value32 == UINT32_C(0x12345678));

   uint64_t value64 = UINT64_C(0x0123456789abcdef);
   radeon_info_mock.result_once = -EIO;
   assert(radeon_drm_vk_device_info_u64(
             &device, RADEON_INFO_VRAM_USAGE, &value64) == -EIO);
   assert(value64 == UINT64_C(0x0123456789abcdef));

   uint32_t values[32];
   for (uint32_t index = 0; index < ARRAY_SIZE(values); index++)
      values[index] = UINT32_C(0x80000000) | index;
   uint32_t expected[32];
   memcpy(expected, values, sizeof(expected));
   radeon_info_mock.result_once = -EIO;
   assert(radeon_drm_vk_device_info_u32_array(
             &device, RADEON_INFO_SI_TILE_MODE_ARRAY, values,
             ARRAY_SIZE(values)) == -EIO);
   assert(memcmp(values, expected, sizeof(values)) == 0);
   assert(radeon_info_mock.calls == 3);
   radeon_drm_vk_device_finish(&device);
}

int
main(void)
{
   test_u32_request_domain();
   test_u64_request_domain();
   test_array_request_domain();
   test_invalid_info_calls_fail_before_ioctl();
   test_ioctl_failure_preserves_caller_value();
   return 0;
}
