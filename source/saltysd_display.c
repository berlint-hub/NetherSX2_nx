/* SaltySD bridge for FPSLocker display cadence and Vulkan buffer settings.
 * NX-FPS's import-replacement hook does not see this process's internal Vulkan
 * entry points. NetherSX2 wraps its own present path for frame measurement;
 * FPSLocker communication/settings remain on the shared-block bridge. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/shmem.h>
#include <switch/arm/tls.h>
#include <switch/arm/counter.h>

#include "saltysd_display.h"
#include "saltysd_fps_policy.h"

#define SALTYSD_PORT "NethFPS"
#define SALTYSD_LEGACY_PORT "SaltySD"
#define SFCI_MAGIC 0x49434653u
#define SFCO_MAGIC 0x4f434653u
#define IPC_REQUEST 4u
#define SALTYSD_GET_SHARED_MEMORY_HANDLE 7u
#define SALTYSD_GET_DISPLAY_REFRESH_RATE 10u
#define SALTYSD_SHARED_MEMORY_SIZE 0x1000u
#define NX_FPS_MAGIC 0x00465053u
/* Packed NxFpsSharedBlock prefix; several fields are intentionally unaligned. */
#define NX_FPS_FPS_OFFSET 4u
#define NX_FPS_AVG_OFFSET 5u
#define NX_FPS_PLUGIN_OFFSET 9u
#define NX_FPS_TARGET_OFFSET 10u
#define NX_FPS_API_OFFSET 14u
#define NX_FPS_BUFFERS_OFFSET 55u
#define NX_FPS_SET_BUFFERS_OFFSET 56u
#define NX_FPS_DISPLAY_SYNC_OFFSET 59u
#define NX_FPS_API_VULKAN 3u
#define RESOURCE_LIMIT_SESSION_COUNT_MAX 4u
#define RESOURCE_LIMIT_SESSION_COUNT_VALUE 32u
#define PRESENT_INTERVAL_SAMPLES 64u
#define PRESENT_MIN_VALID_SAMPLES 60u
#define PRESENT_PUBLISH_INTERVAL_MS 100u
#define PRESENT_STALE_MS 1000u

static volatile int s_enabled;
static volatile int s_buffers_enabled;
static volatile uint32_t s_service_rate;
static volatile uint32_t s_last_connect_result = UINT32_MAX;
static volatile uint32_t s_nethfps_connect_result = UINT32_MAX;
static volatile uint32_t s_legacy_connect_result = UINT32_MAX;
/* 0 = no successful port, 1 = NethFPS, 2 = legacy SaltySD. */
static volatile unsigned s_connect_port;
static volatile int s_last_query_status = -1;
static volatile uint64_t s_last_present_tick;
static volatile uint64_t s_present_intervals[PRESENT_INTERVAL_SAMPLES];
static volatile uint32_t s_present_interval_count;
static volatile uint64_t s_next_query_tick;
static volatile uint64_t s_last_publish_tick;
static volatile unsigned s_observed_fpslocker_target;
static volatile int s_target_ready;
static volatile int s_published_measurement;

static SharedMemory s_shared_memory;
static int s_shared_memory_mapped;
static volatile int s_shared_memory_mapping;
static volatile int s_shutdown_in_progress;
/* Publish/replace the block as one atomic pointer; individual field pointers
 * are derived from a local block snapshot and are never cleared separately. */
static volatile uint8_t * volatile s_fpslocker_block;
static volatile uint32_t s_session_limit_result = UINT32_MAX;
static int s_session_limit_attempted;

/* The emulator core consumes the default session allowance during bootstrap.
 * Raise the process limit before this bridge's first named-port connection. */
static void saltysd_raise_session_limit_once(void) {
  if (s_session_limit_attempted)
    return;
  s_session_limit_attempted = 1;

  u64 resource_limit = 0;
  Result result = svcGetInfo(&resource_limit, InfoType_ResourceLimit,
                             INVALID_HANDLE, 0);
  if (R_FAILED(result) || !resource_limit) {
    __atomic_store_n(&s_session_limit_result,
                     R_FAILED(result) ? (uint32_t)result : UINT32_MAX,
                     __ATOMIC_RELEASE);
    return;
  }

  result = svcSetResourceLimitLimitValue((Handle)resource_limit,
      (LimitableResource)RESOURCE_LIMIT_SESSION_COUNT_MAX,
      RESOURCE_LIMIT_SESSION_COUNT_VALUE);
  svcCloseHandle((Handle)resource_limit);
  __atomic_store_n(&s_session_limit_result, (uint32_t)result,
                   __ATOMIC_RELEASE);
}

static int saltysd_send_query(Handle session, uint32_t command_id,
                              uint32_t request_bytes, uint64_t *response,
                              size_t response_words, Handle *out_handle) {
  uint32_t *tls = (uint32_t *)armGetTls();
  memset(tls, 0, 0x100);
  tls[0] = IPC_REQUEST;
  tls[1] = 0x80000000u | (request_bytes / 4u);
  tls[2] = 1; /* PID descriptor, as required by the SaltySD IPC client. */
  uint64_t *request = (uint64_t *)((uint8_t *)tls + 0x20);
  request[0] = SFCI_MAGIC;
  request[1] = command_id;

  const Result sent = svcSendSyncRequest(session);
  if (R_FAILED(sent))
    return 0;

  const uint32_t ctrl0 = tls[0];
  const uint32_t ctrl1 = tls[1];
  uint32_t *cursor = tls + 2;
  unsigned copied_handles = 0;
  unsigned moved_handles = 0;
  Handle first_handle = INVALID_HANDLE;
  if (ctrl1 & 0x80000000u) {
    const uint32_t special = *cursor++;
    if (special & 1u)
      cursor += 2; /* response PID */
    copied_handles = (special >> 1) & 15u;
    moved_handles = (special >> 5) & 15u;
    if (copied_handles + moved_handles)
      first_handle = (Handle)*cursor;
    cursor += copied_handles + moved_handles;
  }
  cursor += ((ctrl0 >> 16) & 15u) * 2u;
  cursor += (((ctrl0 >> 20) & 15u) + ((ctrl0 >> 24) & 15u) +
             ((ctrl0 >> 28) & 15u)) * 3u;
  if (out_handle) {
    if (!copied_handles || first_handle == INVALID_HANDLE)
      return 0;
    *out_handle = first_handle;
  }
  if (!response_words)
    return out_handle != NULL;

  const uintptr_t raw_address = ((uintptr_t)cursor + 15u) & ~(uintptr_t)15u;
  const uint32_t response_bytes = (ctrl1 & 0x1ffu) * 4u;
  if (response_bytes < response_words * sizeof(uint64_t) ||
      raw_address + response_bytes > (uintptr_t)tls + 0x100u)
    return 0;

  const uint64_t *raw = (const uint64_t *)raw_address;
  if ((uint32_t)raw[0] != SFCO_MAGIC || raw[1] != 0)
    return 0;
  if (response)
    memcpy(response, raw, response_words * sizeof(uint64_t));
  return 1;
}

static Result saltysd_connect(Handle *session) {
  const unsigned preferred =
      __atomic_load_n(&s_connect_port, __ATOMIC_ACQUIRE);
  Result nethfps = 0;
  Result legacy = 0;
  int tried_nethfps = 0;
  int tried_legacy = 0;

  /* Remember the working endpoint. In particular, once stock SaltyNX is
   * selected, do not retry the fork-only NethFPS port every second. */
  if (preferred == 2) {
    legacy = svcConnectToNamedPort(session, SALTYSD_LEGACY_PORT);
    tried_legacy = 1;
    __atomic_store_n(&s_legacy_connect_result, (uint32_t)legacy,
                     __ATOMIC_RELEASE);
    if (R_SUCCEEDED(legacy))
      return legacy;
  }
  if (preferred == 1 || preferred == 0) {
    nethfps = svcConnectToNamedPort(session, SALTYSD_PORT);
    tried_nethfps = 1;
    __atomic_store_n(&s_nethfps_connect_result, (uint32_t)nethfps,
                     __ATOMIC_RELEASE);
    if (R_SUCCEEDED(nethfps)) {
      __atomic_store_n(&s_legacy_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
      __atomic_store_n(&s_connect_port, 1, __ATOMIC_RELEASE);
      return nethfps;
    }
  }
  if (!tried_legacy) {
    legacy = svcConnectToNamedPort(session, SALTYSD_LEGACY_PORT);
    tried_legacy = 1;
    __atomic_store_n(&s_legacy_connect_result, (uint32_t)legacy,
                     __ATOMIC_RELEASE);
    if (R_SUCCEEDED(legacy)) {
      __atomic_store_n(&s_connect_port, 2, __ATOMIC_RELEASE);
      return legacy;
    }
  }

  __atomic_store_n(&s_connect_port, 0, __ATOMIC_RELEASE);
  return tried_legacy ? legacy : nethfps;
}

static int saltysd_query_refresh_rate(Handle session, uint8_t *rate) {
  uint64_t response[3] = {0};
  if (!saltysd_send_query(session, SALTYSD_GET_DISPLAY_REFRESH_RATE, 32,
                          response, 3, NULL))
    return 0;
  if (response[2] < 20 || response[2] > 240)
    return 0;
  *rate = (uint8_t)response[2];
  return 1;
}

static int saltysd_map_shared_memory(Handle session) {
  if (__atomic_load_n(&s_shutdown_in_progress, __ATOMIC_ACQUIRE))
    return 0;
  if (__atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE))
    return 1;
  int expected = 0;
  if (!__atomic_compare_exchange_n(&s_shared_memory_mapping, &expected, 1,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return __atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE) != 0;
  if (__atomic_load_n(&s_shutdown_in_progress, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
    return 0;
  }

  Handle shared_handle = INVALID_HANDLE;
  /* Match libnx's minimum CMIF request size: the 16-byte SFCI header plus
   * its 16-byte IPC raw-data allowance. A 16-byte raw payload is too short for
   * SaltyNX's ipcParse path and is misread as an unsupported command. */
  if (!saltysd_send_query(session, SALTYSD_GET_SHARED_MEMORY_HANDLE, 32,
                          NULL, 2, &shared_handle)) {
    __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
    return 0;
  }

  shmemLoadRemote(&s_shared_memory, shared_handle,
                  SALTYSD_SHARED_MEMORY_SIZE, Perm_Rw);
  if (R_FAILED(shmemMap(&s_shared_memory))) {
    shmemClose(&s_shared_memory);
    __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
    return 0;
  }
  if (__atomic_load_n(&s_shutdown_in_progress, __ATOMIC_ACQUIRE)) {
    shmemClose(&s_shared_memory);
    memset(&s_shared_memory, 0, sizeof(s_shared_memory));
    __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
    return 0;
  }
  __atomic_store_n(&s_shared_memory_mapped, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
  return 1;
}

static volatile uint8_t *saltysd_find_shared_block(void) {
  if (!__atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE))
    return NULL;
  const uint8_t *base = (const uint8_t *)shmemGetAddr(&s_shared_memory);
  if (!base)
    return NULL;

  for (size_t offset = 0; offset + NX_FPS_DISPLAY_SYNC_OFFSET + 1 <=
                           SALTYSD_SHARED_MEMORY_SIZE; offset += 4) {
    uint32_t magic;
    memcpy(&magic, base + offset, sizeof(magic));
    if (magic == NX_FPS_MAGIC)
      return (volatile uint8_t *)(base + offset);
  }
  return NULL;
}

static volatile uint8_t *saltysd_publish_shared_block(void) {
  volatile uint8_t *block = saltysd_find_shared_block();
  if (!block) {
    __atomic_store_n(&s_target_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_fpslocker_block, NULL, __ATOMIC_RELEASE);
    return NULL;
  }

  /* Publish the mapping as one release-store. Callers keep this local snapshot
   * and derive fields directly from it instead of sharing torn field pointers. */
  __atomic_store_n(&s_fpslocker_block, block, __ATOMIC_RELEASE);
  __atomic_store_n(&s_observed_fpslocker_target,
                   __atomic_load_n(block + NX_FPS_TARGET_OFFSET, __ATOMIC_ACQUIRE),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&s_target_ready, 1, __ATOMIC_RELEASE);
  return block;
}

static unsigned estimate_present_fps(unsigned *p90_fps) {
  const uint64_t frequency = armGetSystemTickFreq();
  uint32_t count = __atomic_load_n(&s_present_interval_count, __ATOMIC_ACQUIRE);
  if (count > PRESENT_INTERVAL_SAMPLES)
    count = PRESENT_INTERVAL_SAMPLES;
  if (count < PRESENT_MIN_VALID_SAMPLES)
    return 0;

  uint64_t valid[PRESENT_INTERVAL_SAMPLES];
  size_t valid_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint64_t interval = __atomic_load_n(&s_present_intervals[i],
                                               __ATOMIC_ACQUIRE);
    if (interval < frequency / 240u || interval > frequency / 10u)
      continue;
    valid[valid_count++] = interval;
  }
  if (valid_count < PRESENT_MIN_VALID_SAMPLES)
    return 0;

  const SaltysdFpsEstimate estimate = saltysd_estimate_fps(
      valid, valid_count, frequency, PRESENT_MIN_VALID_SAMPLES);
  if (p90_fps)
    *p90_fps = estimate.p90_fps;
  return estimate.average_fps;
}

static void clear_live_measurement(volatile uint8_t *block) {
  if (!block)
    return;
  const uint8_t zero = 0;
  const float zero_average = 0.0f;
  memcpy((void *)(block + NX_FPS_PLUGIN_OFFSET), &zero, sizeof(zero));
  memcpy((void *)(block + NX_FPS_FPS_OFFSET), &zero, sizeof(zero));
  memcpy((void *)(block + NX_FPS_AVG_OFFSET), &zero_average,
         sizeof(zero_average));
  __atomic_store_n(&s_published_measurement, 0, __ATOMIC_RELEASE);
}

static void publish_live_measurement(volatile uint8_t *block, unsigned fps,
                                     unsigned average_fps) {
  if (!block || !fps || !average_fps)
    return;

  const uint8_t fps_u8 = (uint8_t)fps;
  const uint8_t api = NX_FPS_API_VULKAN;
  const uint8_t one = 1;
  const float average = (float)average_fps;
  memcpy((void *)(block + NX_FPS_FPS_OFFSET), &fps_u8, sizeof(fps_u8));
  memcpy((void *)(block + NX_FPS_AVG_OFFSET), &average, sizeof(average));
  memcpy((void *)(block + NX_FPS_API_OFFSET), &api, sizeof(api));
  /* Publish active last so the overlay never sees a live plugin with no FPS. */
  memcpy((void *)(block + NX_FPS_PLUGIN_OFFSET), &one, sizeof(one));
  __atomic_store_n(&s_published_measurement, 1, __ATOMIC_RELEASE);
}

static void publish_present_rate(uint64_t now, uint64_t frequency) {
  volatile uint8_t *block = saltysd_publish_shared_block();
  if (!block) {
    __atomic_store_n(&s_observed_fpslocker_target, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_published_measurement, 0, __ATOMIC_RELEASE);
    return;
  }

  /* FPSlocked is FPSLocker's game-cap target, not a panel-rate mailbox.
   * Observe it for diagnostics but never modify it: FPSLocker owns this byte
   * and its increase/decrease controls, including the matching SaltySD IPC. */
  const unsigned target = __atomic_load_n(block + NX_FPS_TARGET_OFFSET,
                                           __ATOMIC_ACQUIRE);
  __atomic_store_n(&s_observed_fpslocker_target, target, __ATOMIC_RELEASE);

  const uint64_t last_present =
      __atomic_load_n(&s_last_present_tick, __ATOMIC_ACQUIRE);
  if (!last_present || now < last_present ||
      now - last_present > frequency * PRESENT_STALE_MS / 1000u) {
    if (__atomic_load_n(&s_published_measurement, __ATOMIC_ACQUIRE))
      clear_live_measurement(block);
    __atomic_store_n(&s_present_interval_count, 0, __ATOMIC_RELEASE);
    return;
  }

  const unsigned fps = estimate_present_fps(NULL);
  if (!fps)
    return; /* Preserve the last live report during a short pause/warm-up. */
  publish_live_measurement(block, fps, fps);
}

void saltysd_fpslocker_buffers_configure(bool enabled) {
  __atomic_store_n(&s_buffers_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
  if (enabled) {
    __atomic_store_n(&s_shutdown_in_progress, 0, __ATOMIC_RELEASE);
    saltysd_raise_session_limit_once();
  }
}

unsigned saltysd_fpslocker_buffers_request(void) {
  if (!__atomic_load_n(&s_buffers_enabled, __ATOMIC_ACQUIRE))
    return 0;
  volatile uint8_t *block = saltysd_find_shared_block();
  if (!block)
    return 0;
  const uint8_t requested = __atomic_load_n(
      block + NX_FPS_SET_BUFFERS_OFFSET, __ATOMIC_ACQUIRE);
  return requested >= 2 && requested <= 4 ? requested : 0;
}

void saltysd_fpslocker_buffers_report_actual(unsigned image_count) {
  if (!image_count || image_count > UINT8_MAX ||
      !__atomic_load_n(&s_buffers_enabled, __ATOMIC_ACQUIRE))
    return;
  volatile uint8_t *block = saltysd_find_shared_block();
  if (block)
    __atomic_store_n(block + NX_FPS_BUFFERS_OFFSET, (uint8_t)image_count,
                     __ATOMIC_RELEASE);
}

void saltysd_display_sync_configure(bool enabled) {
  if (!enabled) {
    saltysd_display_sync_shutdown();
    __atomic_store_n(&s_enabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_query_status, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
    __atomic_store_n(&s_nethfps_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
    __atomic_store_n(&s_legacy_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
    __atomic_store_n(&s_connect_port, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_present_tick, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_present_interval_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_next_query_tick, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_publish_tick, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_observed_fpslocker_target, 0, __ATOMIC_RELEASE);
    return;
  }

  __atomic_store_n(&s_shutdown_in_progress, 0, __ATOMIC_RELEASE);
  saltysd_raise_session_limit_once();
  __atomic_store_n(&s_enabled, 1, __ATOMIC_RELEASE);
}

void saltysd_display_sync_poll(void) {
  const int sync_enabled = __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE);
  const int buffers_enabled =
      __atomic_load_n(&s_buffers_enabled, __ATOMIC_ACQUIRE);
  if (!sync_enabled && !buffers_enabled)
    return;
  /* Buffer-only mode needs one successful shared-block discovery, not a
   * one-hertz service connection for the rest of the emulation session. */
  if (!sync_enabled && buffers_enabled &&
      __atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE) &&
      saltysd_find_shared_block() != NULL)
    return;

  const uint64_t now = armGetSystemTick();
  const uint64_t frequency = armGetSystemTickFreq();
  uint64_t next = __atomic_load_n(&s_next_query_tick, __ATOMIC_ACQUIRE);
  for (;;) {
    if (next && now < next)
      return;
    if (__atomic_compare_exchange_n(&s_next_query_tick, &next, now + frequency,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      break;
  }

  Handle session = INVALID_HANDLE;
  const Result connected = saltysd_connect(&session);
  __atomic_store_n(&s_last_connect_result, (uint32_t)connected, __ATOMIC_RELEASE);
  if (R_FAILED(connected)) {
    if (sync_enabled) {
      const int busy = R_MODULE(connected) == Module_Kernel &&
                       R_DESCRIPTION(connected) == KernelError_OutOfResource;
      const int query_status = busy ? 1 : 0;
      __atomic_store_n(&s_last_query_status, query_status, __ATOMIC_RELEASE);
      __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    }
    return;
  }

  if (sync_enabled) {
    uint8_t rate = 0;
    const int query_ok = saltysd_query_refresh_rate(session, &rate);
    __atomic_store_n(&s_last_query_status, query_ok ? 2 : 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, query_ok ? rate : 0, __ATOMIC_RELEASE);
  }

  /* SaltySD returns the shared handle over the same short-lived session. */
  if (!__atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE))
    saltysd_map_shared_memory(session);
  svcCloseHandle(session);

  /* The buffer-only bridge must not claim FPSLocker target/measurement fields.
   * Display sync discovers those fields only when it is explicitly enabled. */
  if (__atomic_load_n(&s_shared_memory_mapped, __ATOMIC_ACQUIRE)) {
    if (sync_enabled)
      saltysd_publish_shared_block();
    else
      (void)saltysd_find_shared_block();
  }
}

void saltysd_display_sync_publish(void) {
  /* Keep FPS telemetry available to FPSLocker even when its panel-sync toggle is
   * off; this path never writes the user's FPSlocked target. */
  if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE))
    return;

  const uint64_t now = armGetSystemTick();
  const uint64_t frequency = armGetSystemTickFreq();
  uint64_t previous = __atomic_load_n(&s_last_publish_tick, __ATOMIC_ACQUIRE);
  const uint64_t interval = frequency * PRESENT_PUBLISH_INTERVAL_MS / 1000u;
  for (;;) {
    if (previous && now - previous < interval)
      return;
    if (__atomic_compare_exchange_n(&s_last_publish_tick, &previous, now,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      break;
  }

  /* Keep the shared FPS fields fresh at 10 Hz. IPC remains limited to one
   * query per second in saltysd_display_sync_poll(). */
  publish_present_rate(now, frequency);
}

void saltysd_display_sync_note_present(void) {
  if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE))
    return;
  const uint64_t now = armGetSystemTick();
  const uint64_t previous = __atomic_exchange_n(&s_last_present_tick, now,
                                                 __ATOMIC_ACQ_REL);
  if (!previous || now <= previous)
    return;
  const uint64_t elapsed = now - previous;
  const uint64_t frequency = armGetSystemTickFreq();
  if (elapsed < frequency / 240u || elapsed > frequency / 5u) {
    if (elapsed > frequency / 5u)
      __atomic_store_n(&s_present_interval_count, 0, __ATOMIC_RELEASE);
    return;
  }

  const uint32_t index = __atomic_fetch_add(&s_present_interval_count, 1,
                                             __ATOMIC_ACQ_REL);
  __atomic_store_n(&s_present_intervals[index % PRESENT_INTERVAL_SAMPLES], elapsed,
                   __ATOMIC_RELEASE);
}

void saltysd_display_sync_shutdown(void) {
  __atomic_store_n(&s_shutdown_in_progress, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&s_enabled, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s_buffers_enabled, 0, __ATOMIC_RELEASE);
  while (__atomic_load_n(&s_shared_memory_mapping, __ATOMIC_ACQUIRE))
    svcSleepThread(1000000);

  volatile uint8_t *block = __atomic_load_n(&s_fpslocker_block,
                                             __ATOMIC_ACQUIRE);
  if (block) {
    uint32_t magic = 0;
    memcpy(&magic, (const void *)block, sizeof(magic));
    if (magic == NX_FPS_MAGIC &&
        __atomic_load_n(&s_published_measurement, __ATOMIC_ACQUIRE))
      clear_live_measurement(block);
  }

  __atomic_store_n(&s_fpslocker_block, NULL, __ATOMIC_RELEASE);
  __atomic_store_n(&s_published_measurement, 0, __ATOMIC_RELEASE);
  if (__atomic_exchange_n(&s_shared_memory_mapped, 0, __ATOMIC_ACQ_REL))
    shmemClose(&s_shared_memory);
  memset(&s_shared_memory, 0, sizeof(s_shared_memory));
  __atomic_store_n(&s_shared_memory_mapping, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s_last_publish_tick, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s_observed_fpslocker_target, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s_target_ready, 0, __ATOMIC_RELEASE);
}

bool saltysd_display_sync_active(void) {
  /* A mapped FPSLocker block alone is not a display-sync request. Honor its
   * own packed handheld/docked toggles so the user can control max-FPS and sync
   * independently, and don't suppress the low-latency path while both are off. */
  if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE))
    return false;
  volatile uint8_t *block = __atomic_load_n(&s_fpslocker_block,
                                             __ATOMIC_ACQUIRE);
  if (!block)
    return false;
  const uint8_t flags = __atomic_load_n(block + NX_FPS_DISPLAY_SYNC_OFFSET,
                                         __ATOMIC_ACQUIRE);
  return saltysd_fpslocker_display_sync_enabled(flags) != 0;
}

float saltysd_display_sync_rate(void) {
  if (!saltysd_display_sync_active())
    return 0.0f;
  return (float)estimate_present_fps(NULL);
}

int saltysd_display_sync_query_status(void) {
  return __atomic_load_n(&s_last_query_status, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_service_rate(void) {
  return __atomic_load_n(&s_service_rate, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_connect_result(void) {
  return __atomic_load_n(&s_last_connect_result, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_nethfps_connect_result(void) {
  return __atomic_load_n(&s_nethfps_connect_result, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_legacy_connect_result(void) {
  return __atomic_load_n(&s_legacy_connect_result, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_connect_port(void) {
  return __atomic_load_n(&s_connect_port, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_fpslocker_target(void) {
  return __atomic_load_n(&s_observed_fpslocker_target, __ATOMIC_ACQUIRE);
}

bool saltysd_display_sync_target_ready(void) {
  return __atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE) != 0;
}

unsigned saltysd_display_sync_session_limit_result(void) {
  return __atomic_load_n(&s_session_limit_result, __ATOMIC_ACQUIRE);
}
