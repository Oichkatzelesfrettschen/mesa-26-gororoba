/*
 * SPDX-License-Identifier: MIT
 *
 * HyperZ descriptor ownership across submission preparation and transport.
 */

#undef NDEBUG

#include "r3v_native.h"

#include "amd/r300/common/r300_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <radeon_drm.h>

struct hyperz_info_mock {
   uint32_t selectors[8];
   uint32_t call_count;
   uint32_t acquire_reply;
   uint32_t release_reply;
   int acquire_result;
   int release_result;
};

static struct hyperz_info_mock hyperz_info_mock;

static int
hyperz_command_write_read(int fd, unsigned long command, void *data,
                          unsigned size)
{
   (void)fd;
   assert(command == DRM_RADEON_INFO);
   assert(size == sizeof(struct drm_radeon_info));
   struct drm_radeon_info *arguments = data;
   assert(arguments->request == RADEON_INFO_WANT_HYPERZ);
   uint32_t *value = (uint32_t *)(uintptr_t)arguments->value;
   const uint32_t selector = *value;
   assert(selector <= 1u);
   assert(hyperz_info_mock.call_count <
          sizeof(hyperz_info_mock.selectors) /
             sizeof(hyperz_info_mock.selectors[0]));
   hyperz_info_mock.selectors[hyperz_info_mock.call_count++] = selector;

   const int result = selector == 1u ? hyperz_info_mock.acquire_result
                                     : hyperz_info_mock.release_result;
   if (result != 0)
      return result;
   *value = selector == 1u ? hyperz_info_mock.acquire_reply
                           : hyperz_info_mock.release_reply;
   return 0;
}

static const struct radeon_drm_vk_ioctl_ops hyperz_info_ops = {
   .command_write_read = hyperz_command_write_read,
};

static void
init_device(struct r3v_native_device *device)
{
   memset(device, 0, sizeof(*device));
   memset(&hyperz_info_mock, 0, sizeof(hyperz_info_mock));
   hyperz_info_mock.acquire_reply = 1u;
   assert(radeon_drm_vk_device_init(&device->drm, 42, &hyperz_info_ops) == 0);
}

static struct r3v_native_cmd_buffer
hyperz_command(uint32_t words[2])
{
   words[0] = CP_PACKET0(R300_ZB_BW_CNTL, 0);
   words[1] = R300_FAST_FILL_ENABLE;
   return (struct r3v_native_cmd_buffer){
      .ib = words,
      .ib_size_dwords = 2u,
   };
}

static enum r300_zb_hyperz_verdict
prepare(struct r3v_native_device *device,
        const struct r3v_native_cmd_buffer *command,
        struct r3v_hyperz_grant_transaction *transaction,
        int *request_result, uint32_t *returned_ownership)
{
   struct r300_zb_hyperz_site site;
   return r3v_native_hyperz_submission_prepare(
      device, command, transaction, &site, request_result,
      returned_ownership);
}

static void
test_preowned_descriptor_uses_no_ioctl(void)
{
   struct r3v_native_device device;
   init_device(&device);
   device.hyperz_ownership = R300_ZB_HYPERZ_OWNED;
   uint32_t words[2];
   const struct r3v_native_cmd_buffer command = hyperz_command(words);
   struct r3v_hyperz_grant_transaction transaction;
   int request_result = -1;
   uint32_t returned_ownership = 0u;

   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) == R300_ZB_HYPERZ_ADMIT);
   assert(request_result == 0 && returned_ownership == 1u);
   assert(!transaction.newly_acquired);
   assert(r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(hyperz_info_mock.call_count == 0u);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_new_grant_rolls_back_without_an_accepted_cs(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t words[2];
   const struct r3v_native_cmd_buffer command = hyperz_command(words);
   struct r3v_hyperz_grant_transaction transaction;
   int request_result = -1;
   uint32_t returned_ownership = 0u;

   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) == R300_ZB_HYPERZ_ADMIT);
   assert(transaction.newly_acquired);
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_OWNED);
   r3v_hyperz_grant_transaction_record_ioctl(&transaction, false);
   assert(r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_UNOWNED);
   assert(hyperz_info_mock.call_count == 2u);
   assert(hyperz_info_mock.selectors[0] == 1u);
   assert(hyperz_info_mock.selectors[1] == 0u);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_accepted_cs_retains_a_new_grant(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t words[2];
   const struct r3v_native_cmd_buffer command = hyperz_command(words);
   struct r3v_hyperz_grant_transaction transaction;
   int request_result = -1;
   uint32_t returned_ownership = 0u;

   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) == R300_ZB_HYPERZ_ADMIT);
   r3v_hyperz_grant_transaction_record_ioctl(&transaction, false);
   r3v_hyperz_grant_transaction_record_ioctl(&transaction, true);
   r3v_hyperz_grant_transaction_record_ioctl(&transaction, false);
   assert(r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_OWNED);
   assert(hyperz_info_mock.call_count == 1u);
   assert(hyperz_info_mock.selectors[0] == 1u);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_withheld_and_failed_grants_remain_unowned(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t words[2];
   const struct r3v_native_cmd_buffer command = hyperz_command(words);
   struct r3v_hyperz_grant_transaction transaction;
   int request_result = -1;
   uint32_t returned_ownership = UINT32_MAX;

   hyperz_info_mock.acquire_reply = 0u;
   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) ==
          R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
   assert(request_result == 0 && returned_ownership == 0u);
   assert(!transaction.newly_acquired);
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_UNOWNED);
   radeon_drm_vk_device_finish(&device.drm);

   init_device(&device);
   hyperz_info_mock.acquire_result = -EBUSY;
   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) ==
          R300_ZB_HYPERZ_REFUSE_OWNERSHIP);
   assert(request_result == -EBUSY && returned_ownership == 1u);
   assert(!transaction.newly_acquired);
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_UNOWNED);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_failed_release_preserves_owned_state(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t words[2];
   const struct r3v_native_cmd_buffer command = hyperz_command(words);
   struct r3v_hyperz_grant_transaction transaction;
   int request_result = -1;
   uint32_t returned_ownership = 0u;

   assert(prepare(&device, &command, &transaction, &request_result,
                  &returned_ownership) == R300_ZB_HYPERZ_ADMIT);
   hyperz_info_mock.release_result = -EIO;
   assert(!r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_OWNED);

   hyperz_info_mock.release_result = 0;
   hyperz_info_mock.release_reply = 1u;
   assert(!r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_OWNED);

   hyperz_info_mock.release_reply = 0u;
   assert(r3v_native_hyperz_submission_finish(&device, &transaction));
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_UNOWNED);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_malformed_stream_reaches_no_ioctl(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t malformed_word = 0x40000000u;
   const struct r3v_native_cmd_buffer command = {
      .ib = &malformed_word,
      .ib_size_dwords = 1u,
   };
   struct r3v_hyperz_grant_transaction transaction;
   struct r300_zb_hyperz_site site;
   int request_result = -1;
   uint32_t returned_ownership = UINT32_MAX;

   assert(r3v_native_hyperz_submission_prepare(
             &device, &command, &transaction, &site, &request_result,
             &returned_ownership) == R300_ZB_HYPERZ_REFUSE_STREAM);
   assert(hyperz_info_mock.call_count == 0u);
   assert(!transaction.newly_acquired);
   radeon_drm_vk_device_finish(&device.drm);
}

static void
test_explicit_ownership_request(void)
{
   struct r3v_native_device device;
   init_device(&device);
   uint32_t returned_ownership = 0u;

   assert(r3v_native_hyperz_request_ownership(
             &device, &returned_ownership) == 0);
   assert(returned_ownership == 1u);
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_OWNED);
   assert(hyperz_info_mock.call_count == 1u);
   assert(hyperz_info_mock.selectors[0] == 1u);

   returned_ownership = 0u;
   assert(r3v_native_hyperz_request_ownership(
             &device, &returned_ownership) == 0);
   assert(returned_ownership == 1u);
   assert(hyperz_info_mock.call_count == 1u);
   radeon_drm_vk_device_finish(&device.drm);

   init_device(&device);
   hyperz_info_mock.acquire_reply = 0u;
   returned_ownership = UINT32_MAX;
   assert(r3v_native_hyperz_request_ownership(
             &device, &returned_ownership) == 0);
   assert(returned_ownership == 0u);
   assert(device.hyperz_ownership == R300_ZB_HYPERZ_UNOWNED);
   assert(hyperz_info_mock.call_count == 1u);
   radeon_drm_vk_device_finish(&device.drm);

   assert(r3v_native_hyperz_request_ownership(NULL,
                                              &returned_ownership) ==
          -EINVAL);
   assert(r3v_native_hyperz_request_ownership(&device, NULL) == -EINVAL);
}

int
main(void)
{
   test_preowned_descriptor_uses_no_ioctl();
   test_new_grant_rolls_back_without_an_accepted_cs();
   test_accepted_cs_retains_a_new_grant();
   test_withheld_and_failed_grants_remain_unowned();
   test_failed_release_preserves_owned_state();
   test_malformed_stream_reaches_no_ioctl();
   test_explicit_ownership_request();
   return 0;
}
