/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V device: transport ownership, queue, and fail-closed command
 * surface.
 */

#include "r3v_native.h"

#include "amd/r300/common/r300_compute_verb.h"
#include "amd/r300/common/r300_rb2d_fill.h"

#include "r3v_entrypoints.h"
#include "r3v_measurement_declaration.h"
#include "r3v_physical_device.h"
#include "r3v_private.h"
#include "r3v_submit_preflight.h"

#include "vk_alloc.h"
#include "vk_common_entrypoints.h"
#include "vk_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The exact-value native submission gate: "1" opens; unset, empty, and every
 * other value stay closed.  Read once at device creation so the decision
 * cannot drift mid-process.
 */
static bool
r3v_native_submit_hazard_accepted(void)
{
   const char *value = getenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   return value != NULL && strcmp(value, "1") == 0;
}

/* An empty evidence path has no retention destination.  Treat it like an
 * unset value so the queue cannot format artifact names against the root
 * directory while the submit gate is open.
 */
/* The plan-capture path: any absolute path selects capture; empty and
 * unset leave it off.
 */
static const char *
r3v_native_plan_capture_file(void)
{
   const char *value = getenv("R3V_NATIVE_PLAN_CAPTURE_FILE");
   return value != NULL && value[0] != '\0' ? value : NULL;
}

/* The plan file and its nonce: an absolute path and the 32-hex nonce the
 * plan carries; unset or empty leaves replay off.
 */
static const char *
r3v_native_plan_file(void)
{
   const char *value = getenv("R3V_NATIVE_PLAN_FILE");
   return value != NULL && value[0] != '\0' ? value : NULL;
}

static const char *
r3v_native_plan_nonce(void)
{
   const char *value = getenv("R3V_NATIVE_PLAN_NONCE");
   return value != NULL && value[0] != '\0' ? value : NULL;
}

static const char *
r3v_native_manifest_dir(void)
{
   const char *value = getenv("R3V_NATIVE_MANIFEST_DIR");
   return value != NULL && value[0] != '\0' ? value : NULL;
}

/* An R2VB gate opens on the exact value "1" alone; the cached literal
 * keeps the decision independent of later environment mutation.
 */
static const char *
exact_gate(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0 ? "1" : NULL;
}

/* One declared carrier pitch, held to the word's own grids: a decimal
 * count of bytes on the 64-byte grid inside DST_PITCH_OFFSET's pitch
 * field.  A present declaration the driver cannot read is recorded
 * malformed rather than dropped, so it closes the route it governs
 * instead of silently asserting nothing. */
static void
read_declared_pitch(const char *declared, uint32_t *pitch_out,
                    bool *malformed_out)
{
   *pitch_out = 0u;
   *malformed_out = false;
   if (declared == NULL)
      return;
   char *end = NULL;
   const unsigned long value =
      declared[0] != '\0' ? strtoul(declared, &end, 10) : 0u;
   if (declared[0] != '\0' && end != NULL && *end == '\0' && value != 0u &&
       value % R300_RB2D_PITCH_GRANULARITY == 0u &&
       value / R300_RB2D_PITCH_GRANULARITY <= R300_RB2D_MAX_PITCH_UNITS)
      *pitch_out = (uint32_t)value;
   else
      *malformed_out = true;
}

void
r3v_native_device_refresh_delivery_gates(struct r3v_native_device *device)
{
   device->flat_replication_pin =
      exact_gate("R3V_NATIVE_FLAT_REPLICATION_PINNED");
   device->rs_tex_adj_probe_gate = exact_gate("R3V_NATIVE_RS_TEX_ADJ_PROBE");
   device->rs_w_select_probe_gate =
      exact_gate("R3V_NATIVE_RS_W_SELECT_PROBE");
   device->noperspective_carrier_force =
      exact_gate("R3V_NATIVE_NOPERSPECTIVE_CARRIER_FORCE");
   device->r2vb_delivery_gate =
      exact_gate("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   device->r2vb_gpu_delivery_gate =
      exact_gate("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
   device->r2vb_fetched_gate =
      exact_gate("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL");
   uint32_t route_count = 0;
   const struct r300_operation_route_row *routes =
      r300_operation_route_rows(&route_count);
   for (uint32_t r = 0; r < route_count; r++) {
      const struct r300_operation_route_row *row = &routes[r];
      if (row->gate != NULL)
         device->compute_route_gates[row->route_id] = exact_gate(row->gate);
   }
   /* The operator's declared carrier for the windowed contract, read on
    * the same pass as the gates so an environment mutation moves no
    * decision under a recorded command buffer.  A value outside the pitch
    * field or off the 64-byte grid is recorded as malformed rather than
    * dropped: a declaration the driver cannot read closes the windowed
    * route instead of silently asserting nothing. */
   read_declared_pitch(getenv("R3V_NATIVE_RB2D_V2_EXPECTED_PITCH_BYTES"),
                       &device->rb2d_v2_expected_pitch_bytes,
                       &device->rb2d_v2_expected_pitch_malformed);
   /* The pinned carrier and the qualification carrier are read the same
    * way and fail closed the same way: the route consumes the pin in place
    * of the chooser and lowers the carrier's evidence floor only where the
    * qualification declaration names that exact pin. */
   read_declared_pitch(getenv("R3V_NATIVE_RB2D_V2_PINNED_PITCH_BYTES"),
                       &device->rb2d_v2_pinned_pitch_bytes,
                       &device->rb2d_v2_pinned_pitch_malformed);
   read_declared_pitch(
      getenv("R3V_NATIVE_RB2D_CARRIER_QUALIFICATION_PITCH_BYTES"),
      &device->rb2d_carrier_qualification_pitch_bytes,
      &device->rb2d_carrier_qualification_pitch_malformed);

   /* The policy is read once, so a route decision cannot change under a
    * command buffer already recorded against it. */
   device->execution_policy =
      r3v_execution_policy_from_value(getenv("R3V_NATIVE_EXECUTION_POLICY"));
   /* The declared submission identity rides the same read: an operator
    * authorizes one submission, and a value that appears after a command
    * buffer is recorded authorizes nothing this device performs. */
   const char *identity =
      getenv("R3V_NATIVE_AUTHORIZED_FILL_IDENTITY_BLAKE3");
   device->authorized_fill_identity = NULL;
   if (identity != NULL &&
       strlen(identity) ==
          sizeof(device->authorized_fill_identity_storage) - 1) {
      /* Copied rather than pointed at: a getenv result is valid until the
       * next environment mutation, and an authorization the environment can
       * replace under a recorded command buffer authorizes nothing.  A
       * value of any other width is not a digest and stays undeclared. */
      memcpy(device->authorized_fill_identity_storage, identity,
             sizeof(device->authorized_fill_identity_storage));
      device->authorized_fill_identity =
         device->authorized_fill_identity_storage;
   }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_GetDeviceProcAddr(VkDevice _device, const char *pName)
{
   if (_device == VK_NULL_HANDLE || pName == NULL)
      return NULL;
   VK_FROM_HANDLE(vk_device, device, _device);
   return vk_device_get_proc_addr(device, pName);
}

/* Opens this device's measurement session over the declaration the
 * environment names.
 *
 * It is static, and r3v_CreateDevice below is its one call site, so one
 * device opens one session once by linkage rather than by convention.  A
 * device that names no declaration keeps its session inactive and
 * succeeds; an explicit declaration that cannot be read, parsed, or held
 * to this deployment, board, and route refuses the device, because
 * silently downgrading a declared campaign to ordinary execution would
 * publish samples the operator never authorized.
 */
static VkResult
open_measurement_session(struct r3v_native_device *device)
{
   /* The facts the declaration is held against, collected here so the
    * load reads the filesystem once and the environment never.  A shim
    * harness that replaced the fact provider had no device to replace it
    * on yet, so the production provider answers and a declaration
    * written for the live deployment is the only one that opens. */
   char kernel_release[R3V_MEASUREMENT_SESSION_EPOCH_MAX];
   char module_srcversion[R3V_MEASUREMENT_SESSION_EPOCH_MAX];
   kernel_release[0] = '\0';
   module_srcversion[0] = '\0';
   const struct r3v_native_arming_provider *provider =
      r3v_native_arming_host_provider();
   provider->read_kernel_release(provider->ctx, kernel_release,
                                 sizeof(kernel_release));
   provider->read_module_srcversion(provider->ctx, module_srcversion,
                                    sizeof(module_srcversion));

   uint32_t route_count = 0;
   const struct r300_operation_route_row *routes =
      r300_operation_route_rows(&route_count);
   /* The gate cache becomes open-or-closed here, through the one function
    * that owns the rule that a gate opens on the literal "1" and on
    * nothing else, so the load consumes a decision rather than reading
    * the strings a second time. */
   bool route_gate_open[R300_OPERATION_ROUTE_COUNT];
   if (!r3v_route_gate_state_from_cache(device->compute_route_gates,
                                        route_gate_open,
                                        R300_OPERATION_ROUTE_COUNT))
      return vk_error(device->pdevice, VK_ERROR_INITIALIZATION_FAILED);
   const struct r3v_measurement_deployment deployment = {
      .pci_vendor_id = device->pdevice->pci_vendor_id,
      .pci_device_id = device->pdevice->pci_device_id,
      .kernel_release = kernel_release,
      .module_srcversion = module_srcversion,
      .platform_id = r3v_native_arming_platform(device),
      .routes = routes,
      .route_count = route_count,
      .route_gate_open = route_gate_open,
      .route_gate_count = R300_OPERATION_ROUTE_COUNT,
   };

   /* The device's storage is zeroed at allocation, and the session is
    * brought to its initial state explicitly here so the load's reopen
    * refusal reads a state this function established rather than one the
    * allocator happened to leave. */
   r3v_measurement_session_init(&device->measurement_session);
   const char *reason = NULL;
   const char *path = getenv("R3V_NATIVE_MEASUREMENT_DECLARATION");
   switch (r3v_measurement_declaration_open(&device->measurement_session,
                                            path, &deployment, &reason)) {
   case R3V_MEASUREMENT_DECLARATION_ABSENT:
   case R3V_MEASUREMENT_DECLARATION_OPENED:
      return VK_SUCCESS;
   case R3V_MEASUREMENT_DECLARATION_NO_MEMORY:
      return vk_error(device->pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);
   case R3V_MEASUREMENT_DECLARATION_REFUSED:
      break;
   }
   /* An operator who named a declaration asked for a measured campaign.
    * Running the ordinary path under a declaration the driver could not
    * read would publish samples against an authorization that never
    * opened, so the device refuses instead. */
   return vk_errorf(device->pdevice, VK_ERROR_INITIALIZATION_FAILED,
                    "r3v-native: R3V_NATIVE_MEASUREMENT_DECLARATION "
                    "refused: %s", reason != NULL ? reason : "");
}

VkResult
r3v_CreateDevice(VkPhysicalDevice physicalDevice,
                 const VkDeviceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(r3v_physical_device, pdevice, physicalDevice);
   VkResult result;

   /* The device caches one gate value per route identity below, so the route
    * table decides the shape of that array and the meaning of every entry.
    * A malformed table refuses the device here, with nothing allocated: a
    * device created over a table whose identities fall outside the array or
    * whose gate names no variable would index past its cache or resolve an
    * opt-in that cannot be read, and the first submission to consult a route
    * would carry the defect instead of reporting it.
    */
   uint32_t route_count = 0;
   const struct r300_operation_route_row *route_rows =
      r300_operation_route_rows(&route_count);
   const char *route_refusal = NULL;
   if (!r3v_route_table_admits_device(route_rows, route_count,
                                      &route_refusal)) {
      return vk_errorf(pdevice, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: route table refused: %s", route_refusal);
   }

   struct r3v_native_device *device =
      vk_zalloc2(&pdevice->vk.instance->alloc, pAllocator, sizeof(*device),
                 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* Driver entrypoints first; the runtime's common table fills the generic
    * state-tracking surface.  Entrypoints in neither table stay NULL, and
    * GetDeviceProcAddr reports them absent rather than pretending support.
    */
   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &r3v_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &pdevice->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }

   device->pdevice = pdevice;
   device->vk.command_buffer_ops = &r3v_native_cmd_buffer_ops;

   if (radeon_drm_vk_device_init(&device->drm, pdevice->render_node_fd,
                                 NULL) != 0) {
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return vk_error(pdevice, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   result = vk_queue_init(&device->queue.vk, &device->vk,
                          &(VkDeviceQueueCreateInfo){
                             .sType =
                                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                             .queueFamilyIndex = 0,
                             .queueCount = 1,
                          },
                          0);
   if (result != VK_SUCCESS) {
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }
   device->queue.vk.driver_submit = r3v_native_queue_submit;

   device->submit_hazard_accepted = r3v_native_submit_hazard_accepted();
   device->manifest_dir = r3v_native_manifest_dir();
   r3v_native_device_refresh_delivery_gates(device);

   /* A value naming no policy refuses the device.  Reading it as AUTO would
    * let host fallback run the work an operator required on the GPU, and the
    * run would carry that into its evidence unremarked. */
   if (device->execution_policy == R3V_EXECUTION_POLICY_INVALID) {
      const char *declared = getenv("R3V_NATIVE_EXECUTION_POLICY");
      vk_queue_finish(&device->queue.vk);
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return vk_errorf(pdevice, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: R3V_NATIVE_EXECUTION_POLICY names no "
                       "policy: \"%s\"; the values are auto, gpu_only, and "
                       "cpu_reference",
                       declared != NULL ? declared : "");
   }

   /* Both CONSTFILL RB2D gates open name two executors for one transfer
    * destination: the single-window contract and the windowed one write
    * the same bytes under different contracts, and the route policy
    * refuses that pair at every request.
    * Refusing at creation reports it once, where the operator can act on
    * it, rather than once per vkCmdFillBuffer. */
   if (device->compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL] !=
          NULL &&
       device->compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2] !=
          NULL) {
      vk_queue_finish(&device->queue.vk);
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return vk_errorf(pdevice, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v-native: "
                       "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL and "
                       "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL "
                       "both stand open; one route fills a linear transfer "
                       "destination, so close one gate");
   }

   /* A carrier-qualification run lowers the carrier's evidence floor to
    * PLANNED, the one admission the r3v native route set makes for a pitch
    * nothing has exercised.  It belongs to the windowed route alone,
    * so the declaration stands only while that route's gate is open and
    * refuses the device otherwise.  Both spellings are named because the
    * operator has to see which declaration and which gate disagree.
    */
   if ((device->rb2d_carrier_qualification_pitch_bytes != 0u ||
        device->rb2d_carrier_qualification_pitch_malformed) &&
       device->compute_route_gates[R300_OPERATION_ROUTE_RB2D_CONST_FILL_V2] ==
          NULL) {
      vk_queue_finish(&device->queue.vk);
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return vk_errorf(
         pdevice, VK_ERROR_INITIALIZATION_FAILED,
         "r3v-native: R3V_NATIVE_RB2D_CARRIER_QUALIFICATION_PITCH_BYTES "
         "declares a qualification carrier while "
         "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL is not \"1\"; "
         "the qualification carrier rides the windowed route alone");
   }

   /* The measurement declaration reads after the gates and the policy,
    * because it is held against the routes this device selected and a
    * device whose gates already disagree reports that disagreement
    * rather than a route refusal derived from it.  The session is a
    * value inside the device and owns no allocation once the load
    * returns, so a later construction failure needs no unwind of its
    * own. */
   result = open_measurement_session(device);
   if (result != VK_SUCCESS) {
      vk_queue_finish(&device->queue.vk);
      radeon_drm_vk_device_finish(&device->drm);
      vk_device_finish(&device->vk);
      vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
      return result;
   }

   /* Plan capture opens the CS ioctl with the hazard gate closed, so it
    * exists only under the preloaded drm-shim and only with the gate
    * closed; a set capture path outside that shape refuses the device.
    */
   const char *capture_path = r3v_native_plan_capture_file();
   if (capture_path != NULL) {
      const char *refusal = NULL;
      if (device->submit_hazard_accepted)
         refusal = "the hazard gate is open";
      else if (device->manifest_dir != NULL)
         refusal = "a capture session names no attended-evidence "
                   "directory";
      else if (!r3v_native_plan_capture_host_model_present())
         refusal = "no drm-shim host model answers the ioctl";
      int init = refusal == NULL
                    ? r3v_native_plan_capture_init(&device->plan_capture,
                                                   capture_path)
                    : -EINVAL;
      if (refusal != NULL || init != 0) {
         vk_queue_finish(&device->queue.vk);
         radeon_drm_vk_device_finish(&device->drm);
         vk_device_finish(&device->vk);
         vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
         return vk_errorf(pdevice, VK_ERROR_INITIALIZATION_FAILED,
                          "r3v-native: plan capture refused: %s",
                          refusal != NULL ? refusal : strerror(-init));
      }
      device->plan_capture_active = true;
   }

   /* Plan replay opens the ioctl for the plan's submissions alone, so
    * it exists only with every other authorization closed; the plan
    * parses at creation and binds to the running identity at the first
    * submission, where the arming provider is in place.
    */
   const char *plan_path = r3v_native_plan_file();
   if (plan_path != NULL) {
      const char *refusal = NULL;
      if (device->submit_hazard_accepted)
         refusal = "the hazard gate is open";
      else if (device->manifest_dir != NULL)
         refusal = "a plan session names its own evidence directory";
      else if (device->plan_capture_active)
         refusal = "a plan session and a capture session are one device "
                   "apiece";
      int init = refusal == NULL
                    ? r3v_native_plan_replay_init(&device->plan_replay,
                                                  plan_path,
                                                  r3v_native_plan_nonce())
                    : -EINVAL;
      if (refusal != NULL || init != 0) {
         const char *parse =
            init == -EPROTO
               ? r3v_native_plan_parse_result_name(
                    device->plan_replay.parse_result)
               : NULL;
         if (device->plan_capture_active)
            r3v_native_plan_capture_finish(&device->plan_capture);
         vk_queue_finish(&device->queue.vk);
         radeon_drm_vk_device_finish(&device->drm);
         vk_device_finish(&device->vk);
         vk_free2(&pdevice->vk.instance->alloc, pAllocator, device);
         return vk_errorf(pdevice, VK_ERROR_INITIALIZATION_FAILED,
                          "r3v-native: plan replay refused: %s",
                          refusal != NULL ? refusal
                          : parse != NULL ? parse
                                          : strerror(-init));
      }
      device->plan_replay_active = true;
   }

   *pDevice = r3v_native_device_to_handle(device);
   return VK_SUCCESS;
}

void
r3v_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   if (!device)
      return;

   vk_queue_finish(&device->queue.vk);
   r3v_native_prepared_release(device);
   if (device->plan_replay_active) {
      const char *state = r3v_native_plan_replay_close(&device->plan_replay);
      if (strcmp(state, "complete") != 0) {
         vk_logw(VK_LOG_OBJS(&device->vk.base),
                 "r3v-native: plan session closed %s", state);
      }
      r3v_native_plan_replay_finish(&device->plan_replay);
   }
   if (device->plan_capture_active) {
      int written = r3v_native_plan_capture_write(
         &device->plan_capture, device->pdevice->pci_vendor_id,
         device->pdevice->pci_device_id, NULL);
      if (written == -ENOENT) {
         int marked = r3v_native_plan_capture_mark_empty(&device->plan_capture);
         vk_logw(VK_LOG_OBJS(&device->vk.base),
                 "r3v-native: no executable submission ran; no plan "
                 "transcript written%s%s", marked == 0 ? "" : "; marker: ",
                 marked == 0 ? "" : strerror(-marked));
      } else if (written != 0) {
         vk_logw(VK_LOG_OBJS(&device->vk.base),
                 "r3v-native: plan transcript write at destroy failed: %s",
                 strerror(-written));
      }
      r3v_native_plan_capture_finish(&device->plan_capture);
   }
   r3v_native_hyperz_release(device);
   radeon_drm_vk_device_finish(&device->drm);
   vk_device_finish(&device->vk);
   vk_free2(&device->vk.alloc, pAllocator, device);
}
