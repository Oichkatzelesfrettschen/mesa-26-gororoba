/*
 * SPDX-License-Identifier: MIT
 *
 * Attended status-load runner: submits the frozen FLOAT_2 tuple stream
 * while the paired-status census samples, coordinated over a
 * SOCK_SEQPACKET barrier channel by the status-load submitter machine.
 * A declared serial bound resubmits the single-draw cell up to that
 * bound, one outstanding submission at a time; a declared burst count
 * instead records the burst cell -- one IB composing that many members
 * over disjoint carrier rows -- and runs one submission window.  Each submission walks the full event
 * ladder -- poison, arm, ioctl, fence, verify, retain, repoison -- and
 * the first containment, submission, completion, barrier, or
 * verification failure aborts the run with its exact reason.  The
 * merged two-sender transcript, the per-iteration carriers, and the
 * outcome summary retain in the evidence directory; the census-side
 * overlap verdict belongs to the offline correlation reducer, never to
 * this program.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_status_load_machine.h"

#include "amd/r300/common/r300_r2vb_float2_tuple_pass.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The sampler climbs to READY inside this budget or the run aborts
 * before any submission; the post-run STOPPED wait shares it and its
 * expiry is recorded rather than fatal.
 */
#define SAMPLER_WAIT_BUDGET_MS 120000

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

/* Extracts one string field value from a protocol JSON line; the fixed
 * emitter spelling ("name": "value" or "name": number) is the format
 * contract, so a full JSON parser stays out of the attended binary.
 */
static bool
message_field(const char *line, const char *name, char *out, size_t capacity)
{
   char needle[64];
   int n = snprintf(needle, sizeof(needle), "\"%s\": ", name);
   if (n <= 0 || (size_t)n >= sizeof(needle))
      return false;
   const char *value = strstr(line, needle);
   if (value == NULL)
      return false;
   value += n;
   if (*value == '"')
      value++;
   size_t length = 0;
   while (value[length] != '\0' && value[length] != '"' &&
          value[length] != ',' && value[length] != '}' &&
          length + 1 < capacity)
      length++;
   if (length + 1 >= capacity)
      return false;
   memcpy(out, value, length);
   out[length] = '\0';
   return length > 0;
}

/* Everything the ops table acts on: the loaded device entry points, the
 * live mappings, the oracle inputs, the transcript, and the barrier
 * channel.  One struct so each op names exactly the state it touches.
 */
struct serial_run {
   VkDevice device;
   VkQueue queue;
   VkCommandBuffer cmd;
   PFN_vkQueueSubmit submit;
   uint32_t *carrier_map;
   uint32_t carrier_bytes;
   const uint8_t *vertex_map;
   const uint8_t *vertex_reference;
   uint32_t vertex_bytes;
   const uint32_t *expected;
   uint32_t expected_dwords;
   /* One member for the serial cell; the burst carrier splits into this
    * many member rows and the oracle judges each row independently.
    */
   uint32_t members;
   uint32_t member_stride_bytes;
   const char *evidence_dir;
   FILE *transcript;
   int sampler_fd;
   bool sampler_closed;
   /* Set when a SAMPLER_ENTER_READ message arrives: the census read is
    * in progress from that instant, so a submission issued after it
    * lands inside the capture.
    */
   bool enter_read_seen;
   char nonce[R3V_STATUS_LOAD_NONCE_LENGTH + 1];
   struct r3v_status_load_machine machine;
   uint32_t iterations_delivered;
   VkResult last_submit_result;
   enum r3v_native_queue_status last_queue_status;
};

static uint64_t
run_now_ns(void *ctx)
{
   (void)ctx;
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC_RAW, &now);
   return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static int
run_emit(void *ctx, const char *line)
{
   struct serial_run *run = ctx;
   if (fputs(line, run->transcript) == EOF)
      return -1;
   return fflush(run->transcript) == 0 ? 0 : -1;
}

/* Receives every pending sampler datagram, retains each line in the
 * transcript, and feeds the observation to the machine.  A foreign
 * nonce, a wrong role, or a channel death before SAMPLER_STOPPED is a
 * barrier fault.  timeout_ms 0 drains without waiting.
 */
static void
sampler_pump(struct serial_run *run, int timeout_ms)
{
   while (run->machine.phase == R3V_STATUS_LOAD_RUNNING &&
          !run->sampler_closed) {
      struct pollfd pfd = { .fd = run->sampler_fd, .events = POLLIN };
      int ready = poll(&pfd, 1, timeout_ms);
      if (ready < 0) {
         if (errno == EINTR)
            continue;
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler channel poll failed");
         return;
      }
      if (ready == 0)
         return;

      char datagram[R3V_STATUS_LOAD_MESSAGE_CAPACITY];
      ssize_t got = recv(run->sampler_fd, datagram, sizeof(datagram) - 2, 0);
      if (got < 0) {
         if (errno == EINTR)
            continue;
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler channel receive failed");
         return;
      }
      if (got == 0) {
         run->sampler_closed = true;
         if (run->machine.sampler != R3V_STATUS_LOAD_SAMPLER_STOPPED)
            r3v_status_load_machine_fault(
               &run->machine,
               "sampler channel closed before SAMPLER_STOPPED");
         return;
      }
      datagram[got] = '\0';
      if (got == 0 || datagram[got - 1] != '\n') {
         datagram[got] = '\n';
         datagram[got + 1] = '\0';
      }
      if (fputs(datagram, run->transcript) == EOF ||
          fflush(run->transcript) != 0) {
         r3v_status_load_machine_fault(&run->machine,
                                       "transcript retention failed");
         return;
      }

      char role[32];
      char state[32];
      char timestamp[32];
      char nonce[R3V_STATUS_LOAD_NONCE_LENGTH + 2];
      if (!message_field(datagram, "sender_role", role, sizeof(role)) ||
          !message_field(datagram, "state", state, sizeof(state)) ||
          !message_field(datagram, "timestamp_ns", timestamp,
                         sizeof(timestamp)) ||
          !message_field(datagram, "run_nonce", nonce, sizeof(nonce))) {
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler message is malformed");
         return;
      }
      if (strcmp(role, "sampler") != 0) {
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler channel carried a foreign "
                                       "role");
         return;
      }
      if (strcmp(nonce, run->nonce) != 0) {
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler message carries a foreign "
                                       "nonce");
         return;
      }
      errno = 0;
      char *end = NULL;
      unsigned long long ns = strtoull(timestamp, &end, 10);
      if (errno != 0 || end == timestamp || *end != '\0') {
         r3v_status_load_machine_fault(&run->machine,
                                       "sampler timestamp is not decimal");
         return;
      }
      if (strcmp(state, "SAMPLER_ENTER_READ") == 0)
         run->enter_read_seen = true;
      r3v_status_load_machine_sampler(&run->machine, state, (uint64_t)ns);
      /* Loop with a zero wait so a burst of datagrams drains whole. */
      timeout_ms = 0;
   }
}

/* Pumps until the machine's sampler view reaches the target state or the
 * budget expires; returns true on arrival.
 */
static bool
sampler_wait(struct serial_run *run, enum r3v_status_load_sampler target,
             int budget_ms)
{
   const uint64_t deadline = run_now_ns(NULL) + (uint64_t)budget_ms * 1000000ull;
   while (run->machine.phase == R3V_STATUS_LOAD_RUNNING &&
          run->machine.sampler != target) {
      uint64_t now = run_now_ns(NULL);
      if (now >= deadline || run->sampler_closed)
         return false;
      sampler_pump(run, (int)((deadline - now) / 1000000ull) + 1);
   }
   return run->machine.sampler == target;
}

static void
fill_poison(struct serial_run *run)
{
   for (uint32_t i = 0; i < run->carrier_bytes / 4; i++)
      run->carrier_map[i] = R300_R2VB_PRODUCER_POISON_DWORD;
}

static int
op_poison(void *ctx, uint32_t iteration)
{
   (void)iteration;
   fill_poison(ctx);
   return 0;
}

/* Arming here is transcript durability: the pre-submission ladder syncs
 * to disk before the hazard, so a wedge that survives no further code
 * still leaves the armed iteration on record.
 */
static int
op_arm(void *ctx, uint32_t iteration)
{
   (void)iteration;
   struct serial_run *run = ctx;
   if (fflush(run->transcript) != 0)
      return -1;
   return fsync(fileno(run->transcript)) == 0 ? 0 : -1;
}

static int
op_submit(void *ctx, uint32_t iteration)
{
   (void)iteration;
   struct serial_run *run = ctx;
   run->last_submit_result =
      run->submit(run->queue, 1,
                  &(VkSubmitInfo){
                     .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &run->cmd,
                  },
                  VK_NULL_HANDLE);
   run->last_queue_status = r3v_native_queue_submission_status(run->device);
   /* The ioctl was reached and accepted for every status past refusal;
    * completion is the fence operation's verdict.
    */
   return run->last_queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED
             ? -1
             : 0;
}

static int
op_fence_wait(void *ctx, uint32_t iteration)
{
   (void)iteration;
   struct serial_run *run = ctx;
   return run->last_queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED ? 0
                                                                      : -1;
}

/* The oracle judges this iteration's carrier: negative when the checker
 * refuses its inputs, positive when the write is missing, wrong, or
 * uncontained, zero for the exact XY01 expansion with the fetch source
 * intact.
 */
static int
op_verify(void *ctx, uint32_t iteration)
{
   (void)iteration;
   struct serial_run *run = ctx;
   bool all_pass = true;
   uint32_t mismatched = 0;
   for (uint32_t m = 0; m < run->members; m++) {
      struct r300_r2vb_producer_carrier_verdict verdict;
      const uint32_t *row =
         run->carrier_map + (size_t)m * run->member_stride_bytes / 4;
      if (r300_r2vb_producer_carrier_check(
             run->expected, run->expected_dwords,
             R300_R2VB_PRODUCER_POISON_DWORD, row,
             run->member_stride_bytes, &verdict) != 0)
         return -1;
      if (!verdict.expected_pass || !verdict.tail_poison_pass)
         all_pass = false;
      mismatched += verdict.mismatched_dwords;
   }
   const bool vertex_intact = memcmp(run->vertex_map, run->vertex_reference,
                                     run->vertex_bytes) == 0;
   printf("[oracle] iteration=%u members=%u members_pass=%d mismatched=%u "
          "vertex_intact=%d\n",
          iteration, run->members, all_pass, mismatched, vertex_intact);
   fflush(stdout);
   return all_pass && vertex_intact ? 0 : 1;
}

static int
op_retain(void *ctx, uint32_t iteration)
{
   struct serial_run *run = ctx;
   char name[64];
   int n = snprintf(name, sizeof(name), "carrier_iter_%02u.bin", iteration);
   if (n <= 0 || (size_t)n >= sizeof(name))
      return -1;
   if (r3v_native_evidence_write_file(run->evidence_dir, name,
                                      run->carrier_map,
                                      run->carrier_bytes) != 0)
      return -1;
   run->iterations_delivered = iteration + 1;
   return 0;
}

static int
op_repoison(void *ctx, uint32_t iteration)
{
   (void)iteration;
   fill_poison(ctx);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <evidence-directory>\n", argv[0]);
      return 2;
   }
   struct serial_run run = { .sampler_fd = -1 };
   run.evidence_dir = argv[1];

   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, run.evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR and the evidence argument name "
              "different directories\n");
      return 1;
   }

   /* The declared serial bound is this run's iteration count; the driver
    * gate re-parses the same spelling, so the two bounds cannot drift.
    * A declared burst count instead selects the burst cell: one
    * submission window whose IB composes that many members.  The two
    * declarations name different cells, so a run carries exactly one.
    */
   const char *serial = getenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS");
   uint32_t iterations = 0;
   if (serial != NULL && serial[0] >= '1' && serial[0] <= '9') {
      size_t i = 0;
      for (; serial[i] >= '0' && serial[i] <= '9' && i < 3; i++)
         iterations = iterations * 10 + (uint32_t)(serial[i] - '0');
      if (serial[i] != '\0' || iterations > R3V_STATUS_LOAD_MAX_ITERATIONS)
         iterations = 0;
   }
   const char *burst = getenv("R3V_NATIVE_AUTHORIZED_BURST_DRAWS");
   uint32_t burst_draws = 0;
   if (burst != NULL && burst[0] >= '1' && burst[0] <= '9') {
      size_t i = 0;
      for (; burst[i] >= '0' && burst[i] <= '9' && i < 3; i++)
         burst_draws = burst_draws * 10 + (uint32_t)(burst[i] - '0');
      if (burst[i] != '\0' ||
          burst_draws > R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS)
         burst_draws = 0;
   }
   if (burst_draws != 0 && iterations != 0) {
      fprintf(stderr,
              "serial and burst declarations name different cells; "
              "declare exactly one\n");
      return 1;
   }
   if (burst_draws != 0) {
      iterations = 1;
   } else if (iterations == 0) {
      fprintf(stderr,
              "declare R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS or "
              "R3V_NATIVE_AUTHORIZED_BURST_DRAWS as an exact decimal "
              "1..64\n");
      return 1;
   }

   const char *nonce = getenv("R3V_STATUS_LOAD_RUN_NONCE");
   if (nonce == NULL || strlen(nonce) != R3V_STATUS_LOAD_NONCE_LENGTH) {
      fprintf(stderr,
              "R3V_STATUS_LOAD_RUN_NONCE is not 32 lowercase hex "
              "characters\n");
      return 1;
   }
   memcpy(run.nonce, nonce, R3V_STATUS_LOAD_NONCE_LENGTH + 1);

   uint32_t member_stride = 0;
   if (r300_r2vb_float2_tuple_burst_member_stride_bytes(&member_stride) !=
       0) {
      fprintf(stderr, "member geometry unresolved\n");
      return 1;
   }
   uint32_t carrier_bytes = 0;
   if (burst_draws != 0) {
      if (r3v_native_burst_carrier_bytes(burst_draws, &carrier_bytes) != 0) {
         fprintf(stderr, "burst carrier geometry unresolved\n");
         return 1;
      }
   } else if (r3v_native_producer_carrier_bytes(&carrier_bytes) != 0) {
      fprintf(stderr, "carrier geometry unresolved\n");
      return 1;
   }
   run.carrier_bytes = carrier_bytes;
   run.members = burst_draws != 0 ? burst_draws : 1;
   run.member_stride_bytes =
      burst_draws != 0 ? member_stride : carrier_bytes;
   run.vertex_bytes = R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                      (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                       R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES);
   static uint32_t expected[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4];
   run.expected = expected;
   run.expected_dwords = (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   if (r300_r2vb_float2_tuple_reference_expected(expected,
                                                 run.expected_dwords) != 0) {
      fprintf(stderr, "tuple identity delivery failed\n");
      return 1;
   }
   static uint8_t vertex_reference[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                                   (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                                    R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES)];
   if (r300_r2vb_float2_tuple_vertex_stream(
          r300_r2vb_float2_tuple_reference_records,
          R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_reference,
          run.vertex_bytes) != 0) {
      fprintf(stderr, "vertex stream serialization failed\n");
      return 1;
   }
   run.vertex_reference = vertex_reference;

   stage("transcript");
   char transcript_path[PATH_MAX];
   if (snprintf(transcript_path, sizeof(transcript_path),
                "%s/status_event_transcript.jsonl",
                run.evidence_dir) >= (int)sizeof(transcript_path)) {
      fprintf(stderr, "evidence path too long\n");
      return 1;
   }
   run.transcript = fopen(transcript_path, "wx");
   if (run.transcript == NULL) {
      fprintf(stderr, "transcript creation failed (%s); a retained "
                      "transcript never rewrites\n",
              strerror(errno));
      return 1;
   }

   stage("sampler channel");
   char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   if (snprintf(socket_path, sizeof(socket_path), "%s/sampler.sock",
                run.evidence_dir) >= (int)sizeof(socket_path)) {
      fprintf(stderr, "sampler socket path exceeds sun_path\n");
      return 1;
   }
   int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
   if (listener < 0) {
      fprintf(stderr, "sampler socket creation failed\n");
      return 1;
   }
   struct sockaddr_un address = { .sun_family = AF_UNIX };
   memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
   if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
       listen(listener, 1) != 0) {
      fprintf(stderr, "sampler socket bind failed (%s)\n", strerror(errno));
      return 1;
   }
   printf("[sampler] listening at %s\n", socket_path);
   fflush(stdout);
   struct pollfd accept_poll = { .fd = listener, .events = POLLIN };
   if (poll(&accept_poll, 1, SAMPLER_WAIT_BUDGET_MS) <= 0) {
      fprintf(stderr, "no sampler connected within the wait budget\n");
      return 1;
   }
   run.sampler_fd = accept(listener, NULL, NULL);
   close(listener);
   if (run.sampler_fd < 0) {
      fprintf(stderr, "sampler accept failed\n");
      return 1;
   }

   const struct r3v_status_load_ops ops = {
      .ctx = &run,
      .poison = op_poison,
      .arm = op_arm,
      .submit = op_submit,
      .fence_wait = op_fence_wait,
      .verify = op_verify,
      .retain = op_retain,
      .repoison = op_repoison,
      .now_ns = run_now_ns,
      .emit = run_emit,
   };
   if (r3v_status_load_machine_init(&run.machine, &ops, run.nonce,
                                    iterations) != 0) {
      fprintf(stderr, "machine initialization refused its inputs\n");
      return 1;
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   if (create_instance(&(VkInstanceCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                       },
                       NULL, &instance) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed\n");
      return 1;
   }
#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device\n");
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS482\n");
      return 1;
   }

   stage("device");
   const float priority = 1.0f;
   if (vkCreateDevice(
          pdev,
          &(VkDeviceCreateInfo){
             .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
             .queueCreateInfoCount = 1,
             .pQueueCreateInfos =
                &(VkDeviceQueueCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                   .queueFamilyIndex = 0,
                   .queueCount = 1,
                   .pQueuePriorities = &priority,
                },
          },
          NULL, &run.device) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice failed\n");
      return 1;
   }
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(run.device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkDestroyDevice);
   run.submit = vkQueueSubmit;

   stage("memory");
   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(run.device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = run.carrier_bytes,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &carrier_memory) != VK_SUCCESS ||
       vkAllocateMemory(run.device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = run.vertex_bytes,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &vertex_memory) != VK_SUCCESS) {
      fprintf(stderr, "allocation failed\n");
      return 1;
   }

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          run.device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &pool) != VK_SUCCESS ||
       vkAllocateCommandBuffers(
          run.device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &run.cmd) != VK_SUCCESS ||
       vkBeginCommandBuffer(
          run.cmd, &(VkCommandBufferBeginInfo){
                      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                   }) != VK_SUCCESS) {
      fprintf(stderr, "command buffer preparation failed\n");
      return 1;
   }
   VkResult record_result =
      burst_draws != 0
         ? r3v_native_record_r2vb_status_load_burst(
              run.cmd, carrier_memory, vertex_memory, burst_draws)
         : r3v_native_record_r2vb_status_load_serial(
              run.cmd, carrier_memory, vertex_memory);
   if (record_result != VK_SUCCESS ||
       vkEndCommandBuffer(run.cmd) != VK_SUCCESS) {
      fprintf(stderr, "status-load cell recording failed\n");
      return 1;
   }
   vkGetDeviceQueue(run.device, 0, 0, &run.queue);

   /* Both mappings stay live for the whole run: the queue publishes and
    * invalidates CPU caches around every ioctl for mapped references, so
    * per-iteration repoison and verification read and write through
    * these pointers alone.
    */
   void *carrier_map = NULL;
   void *vertex_map = NULL;
   if (vkMapMemory(run.device, carrier_memory, 0, VK_WHOLE_SIZE, 0,
                   &carrier_map) != VK_SUCCESS ||
       vkMapMemory(run.device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                   &vertex_map) != VK_SUCCESS) {
      fprintf(stderr, "persistent mapping failed\n");
      return 1;
   }
   run.carrier_map = carrier_map;
   run.vertex_map = vertex_map;

   stage("sampler ready");
   if (!sampler_wait(&run, R3V_STATUS_LOAD_SAMPLER_READY,
                     SAMPLER_WAIT_BUDGET_MS)) {
      if (run.machine.phase == R3V_STATUS_LOAD_RUNNING)
         r3v_status_load_machine_fault(&run.machine,
                                       "sampler never reached ready "
                                       "within the wait budget");
   }

   /* The hazard: up to the declared bound of live DRM_RADEON_CS
    * submissions, one outstanding at a time, each under the full event
    * ladder.  The machine stops the run at its first failure and no
    * resubmission follows an abort.
    */
   /* The burst's single window must overlap the census capture, so the
    * submission waits for the sampler's ENTER_READ: the kernel is
    * recording from that message on, and the ioctl issued after it puts
    * the GPU-busy interval inside the capture instead of racing it.
    * The serial cell keeps its many-window overlap and submits from
    * READY.
    */
   if (burst_draws != 0) {
      stage("census read in progress");
      const uint64_t read_deadline =
         run_now_ns(NULL) + (uint64_t)SAMPLER_WAIT_BUDGET_MS * 1000000ull;
      while (run.machine.phase == R3V_STATUS_LOAD_RUNNING &&
             !run.enter_read_seen && !run.sampler_closed &&
             run_now_ns(NULL) < read_deadline)
         sampler_pump(&run, 100);
      if (!run.enter_read_seen &&
          run.machine.phase == R3V_STATUS_LOAD_RUNNING)
         r3v_status_load_machine_fault(&run.machine,
                                       "census read never began within "
                                       "the wait budget");
   }

   stage("serial submissions");
   for (uint32_t i = 0; i < iterations; i++) {
      sampler_pump(&run, 0);
      if (run.machine.phase != R3V_STATUS_LOAD_RUNNING)
         break;
      if (r3v_status_load_machine_iterate(&run.machine) != 0)
         break;
   }

   stage("sampler stop");
   const bool sampler_stopped =
      sampler_wait(&run, R3V_STATUS_LOAD_SAMPLER_STOPPED,
                   SAMPLER_WAIT_BUDGET_MS / 4);
   if (run.machine.phase == R3V_STATUS_LOAD_RUNNING)
      r3v_status_load_machine_finish(&run.machine);

   stage("outcome");
   const enum r3v_status_load_phase phase =
      r3v_status_load_machine_phase(&run.machine);
   char outcome_json[1024];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"%s\",\n"
      "  \"run_nonce\": \"%s\",\n"
      "  \"declared_submissions\": %u,\n"
      "  \"burst_draws\": %u,\n"
      "  \"iterations_delivered\": %u,\n"
      "  \"machine_phase\": \"%s\",\n"
      "  \"abort_reason\": \"%s\",\n"
      "  \"sampler_stopped_observed\": %s,\n"
      "  \"last_submit_result\": %d,\n"
      "  \"last_queue_status\": \"%s\"\n"
      "}\n",
      burst_draws != 0 ? "r3v-native-status-load-burst-outcome/1"
                       : "r3v-native-status-load-serial-outcome/1",
      run.nonce, iterations, burst_draws, run.iterations_delivered,
      phase == R3V_STATUS_LOAD_COMPLETE
         ? "COMPLETE"
         : (phase == R3V_STATUS_LOAD_ABORTED ? "ABORTED" : "RUNNING"),
      r3v_status_load_machine_abort_reason(&run.machine),
      sampler_stopped ? "true" : "false", run.last_submit_result,
      r3v_native_queue_status_name(run.last_queue_status));
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(run.evidence_dir,
                                      "status_load_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return 1;
   }
   fclose(run.transcript);
   if (run.sampler_fd >= 0)
      close(run.sampler_fd);

   stage("teardown");
   vkDestroyCommandPool(run.device, pool, NULL);
   vkFreeMemory(run.device, carrier_memory, NULL);
   vkFreeMemory(run.device, vertex_memory, NULL);
   vkDestroyDevice(run.device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("verdict: %s (%u/%u delivered)\n",
          phase == R3V_STATUS_LOAD_COMPLETE
             ? (burst_draws != 0 ? "BURST_COMPLETE" : "SERIAL_COMPLETE")
             : (burst_draws != 0 ? "BURST_ABORTED" : "SERIAL_ABORTED"),
          run.iterations_delivered, iterations);
   fflush(stdout);
   return phase == R3V_STATUS_LOAD_COMPLETE &&
                run.iterations_delivered == iterations
             ? 0
             : 1;
}
