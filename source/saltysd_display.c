/* SaltySD bridge for following NetherSX2's present cadence from FPSLocker.
 * The SaltyNX shared block is the target FPSLocker already consumes; this
 * client updates only its FPSlocked byte and restores the previous value on
 * shutdown. No SaltyNX/NX-FPS code is patched. */
#include <stdint.h>
#include <string.h>
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/shmem.h>
#include <switch/arm/tls.h>
#include <switch/arm/counter.h>

#include "saltysd_display.h"

#define SALTYSD_PORT "NethFPS"
#define SALTYSD_LEGACY_PORT "SaltySD"
#define SFCI_MAGIC 0x49434653u
#define SFCO_MAGIC 0x4f434653u
#define IPC_REQUEST 4u
#define SALTYSD_GET_SHARED_MEMORY_HANDLE 7u
#define SALTYSD_GET_DISPLAY_REFRESH_RATE 10u
#define SALTYSD_SHARED_MEMORY_SIZE 0x1000u
#define NX_FPS_MAGIC 0x00465053u
/* Packed NxFpsSharedBlock prefix: MAGIC(4), FPS(1), FPSavg(4), pluginActive(1), FPSlocked. */
#define NX_FPS_TARGET_OFFSET 10u
#define NX_FPS_AVG_OFFSET 5u
#define PRESENT_INTERVAL_SAMPLES 16u

static volatile int s_enabled;
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
static volatile unsigned s_published_target;
static volatile int s_target_ready;

static SharedMemory s_shared_memory;
static int s_shared_memory_mapped;
static volatile uint8_t *s_fpslocked;
static volatile float *s_fpsavg;
static uint8_t s_saved_fpslocked;
static uint8_t s_last_written_fpslocked;
static int s_saved_fpslocked_valid;
static unsigned s_candidate_target;
static unsigned s_candidate_observations;

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
  Result connected = svcConnectToNamedPort(session, SALTYSD_PORT);
  __atomic_store_n(&s_nethfps_connect_result, (uint32_t)connected,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&s_legacy_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
  if (R_SUCCEEDED(connected)) {
    __atomic_store_n(&s_connect_port, 1, __ATOMIC_RELEASE);
    return connected;
  }

  /* Older SaltyNX builds lack the dedicated concurrent client endpoint. Keep
   * the fallback, but expose both results so its error cannot hide a
   * NethFPS-specific connection failure in device diagnostics. */
  connected = svcConnectToNamedPort(session, SALTYSD_LEGACY_PORT);
  __atomic_store_n(&s_legacy_connect_result, (uint32_t)connected,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&s_connect_port, R_SUCCEEDED(connected) ? 2 : 0,
                   __ATOMIC_RELEASE);
  return connected;
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
  if (s_shared_memory_mapped)
    return 1;
  Handle shared_handle = INVALID_HANDLE;
  /* Match libnx's minimum CMIF request size: the 16-byte SFCI header plus
   * its 16-byte IPC raw-data allowance. A 16-byte raw payload is too short for
   * SaltyNX's ipcParse path and is misread as an unsupported command. */
  if (!saltysd_send_query(session, SALTYSD_GET_SHARED_MEMORY_HANDLE, 32,
                          NULL, 2, &shared_handle))
    return 0;

  shmemLoadRemote(&s_shared_memory, shared_handle,
                  SALTYSD_SHARED_MEMORY_SIZE, Perm_Rw);
  if (R_FAILED(shmemMap(&s_shared_memory))) {
    shmemClose(&s_shared_memory);
    return 0;
  }
  s_shared_memory_mapped = 1;
  return 1;
}

static int saltysd_find_fpslocked(void) {
  s_fpslocked = NULL;
  s_fpsavg = NULL;
  if (!s_shared_memory_mapped)
    return 0;
  const uint8_t *base = (const uint8_t *)shmemGetAddr(&s_shared_memory);
  if (!base)
    return 0;

  for (size_t offset = 0; offset + NX_FPS_TARGET_OFFSET + 1 <=
                           SALTYSD_SHARED_MEMORY_SIZE; offset += 4) {
    uint32_t magic;
    memcpy(&magic, base + offset, sizeof(magic));
    if (magic == NX_FPS_MAGIC) {
      s_fpslocked = (volatile uint8_t *)(base + offset + NX_FPS_TARGET_OFFSET);
      s_fpsavg = (volatile float *)(base + offset + NX_FPS_AVG_OFFSET);
      __atomic_store_n(&s_target_ready, 1, __ATOMIC_RELEASE);
      if (!s_saved_fpslocked_valid) {
        s_saved_fpslocked = __atomic_load_n(s_fpslocked, __ATOMIC_ACQUIRE);
        s_last_written_fpslocked = s_saved_fpslocked;
        s_saved_fpslocked_valid = 1;
        __atomic_store_n(&s_published_target, s_saved_fpslocked, __ATOMIC_RELEASE);
      }
      return 1;
    }
  }
  s_saved_fpslocked_valid = 0;
  __atomic_store_n(&s_target_ready, 0, __ATOMIC_RELEASE);
  s_candidate_target = 0;
  s_candidate_observations = 0;
  __atomic_store_n(&s_published_target, 0, __ATOMIC_RELEASE);
  return 0;
}

static unsigned estimate_present_fps(void) {
  const uint64_t frequency = armGetSystemTickFreq();
  uint32_t count = __atomic_load_n(&s_present_interval_count, __ATOMIC_ACQUIRE);
  if (count > PRESENT_INTERVAL_SAMPLES)
    count = PRESENT_INTERVAL_SAMPLES;
  if (count < 6)
    return 0;

  uint64_t total = 0;
  unsigned samples = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint64_t interval = __atomic_load_n(&s_present_intervals[i],
                                               __ATOMIC_ACQUIRE);
    if (interval < frequency / 240u || interval > frequency / 10u)
      continue;
    total += interval;
    samples++;
  }
  if (samples < 6 || !total)
    return 0;

  const uint64_t fps = (frequency * samples + total / 2u) / total;
  return (fps >= 14 && fps <= 120) ? (unsigned)fps : 0;
}

static unsigned quantize_fpslocker_target(unsigned fps) {
  static const uint8_t targets[] = {15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
  unsigned best = 0;
  unsigned best_delta = UINT32_MAX;
  for (size_t i = 0; i < sizeof(targets); i++) {
    const unsigned delta = fps > targets[i] ? fps - targets[i] : targets[i] - fps;
    if (delta < best_delta) {
      best = targets[i];
      best_delta = delta;
    }
  }
  return best;
}

static void publish_present_rate(void) {
  if (!saltysd_find_fpslocked())
    return;
  const unsigned fps = estimate_present_fps();
  /* NX-FPS installs its present hook by rewriting the game's import table;
     this process resolves vkQueuePresentKHR at runtime through the Vulkan
     wrapper instead, so the hooked copy is never called and NX-FPS never
     writes FPSavg here. Publish our own measurement into that field so
     SaltyNX fine mode has a rate to follow. The field is unaligned, hence
     memcpy. */
  if (fps && s_fpsavg) {
    const float measured = (float)fps;
    memcpy((void *)s_fpsavg, &measured, sizeof(measured));
  }
  const unsigned target = fps ? quantize_fpslocker_target(fps) : 0;
  if (!target)
    return;

  if (target == s_candidate_target)
    s_candidate_observations++;
  else {
    s_candidate_target = target;
    s_candidate_observations = 1;
  }
  /* Two one-second observations suppress mode changes caused by short hitches. */
  if (s_candidate_observations < 2 || target == s_last_written_fpslocked)
    return;

  __atomic_store_n(s_fpslocked, (uint8_t)target, __ATOMIC_RELEASE);
  s_last_written_fpslocked = (uint8_t)target;
  __atomic_store_n(&s_published_target, target, __ATOMIC_RELEASE);
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
    __atomic_store_n(&s_published_target, 0, __ATOMIC_RELEASE);
    s_candidate_target = 0;
    s_candidate_observations = 0;
    return;
  }

  __atomic_store_n(&s_enabled, 1, __ATOMIC_RELEASE);
}

void saltysd_display_sync_poll(void) {
  if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE))
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
    const int busy = R_MODULE(connected) == Module_Kernel &&
                     R_DESCRIPTION(connected) == KernelError_OutOfResource;
    const int query_status = busy ? 1 : 0;
    __atomic_store_n(&s_last_query_status, query_status, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    if (s_shared_memory_mapped)
      publish_present_rate(); /* Reuse a previously mapped NX-FPS block. */
    return;
  }

  uint8_t rate = 0;
  const int query_ok = saltysd_query_refresh_rate(session, &rate);
  __atomic_store_n(&s_last_query_status, query_ok ? 2 : 1, __ATOMIC_RELEASE);
  __atomic_store_n(&s_service_rate, query_ok ? rate : 0, __ATOMIC_RELEASE);

  /* SaltySD returns the shared handle over the same short-lived session. */
  if (!s_shared_memory_mapped)
    saltysd_map_shared_memory(session);
  svcCloseHandle(session);
  publish_present_rate();
}

void saltysd_display_sync_note_present(void) {
  if (!saltysd_display_sync_active())
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
  __atomic_store_n(&s_enabled, 0, __ATOMIC_RELEASE);
  if (s_fpslocked && s_saved_fpslocked_valid) {
    uint32_t magic = 0;
    memcpy(&magic, (const void *)(s_fpslocked - NX_FPS_TARGET_OFFSET),
           sizeof(magic));
    const uint8_t current = __atomic_load_n(s_fpslocked, __ATOMIC_ACQUIRE);
    if (magic == NX_FPS_MAGIC && current == s_last_written_fpslocked)
      __atomic_store_n(s_fpslocked, s_saved_fpslocked, __ATOMIC_RELEASE);
  }
  if (s_fpsavg) {
    /* 0.0 means "no measurement"; leave nothing stale behind. */
    const float zero = 0.0f;
    memcpy((void *)s_fpsavg, &zero, sizeof(zero));
    s_fpsavg = NULL;
  }
  s_fpslocked = NULL;
  s_saved_fpslocked_valid = 0;
  if (s_shared_memory_mapped)
    shmemClose(&s_shared_memory);
  s_shared_memory_mapped = 0;
  memset(&s_shared_memory, 0, sizeof(s_shared_memory));
  __atomic_store_n(&s_published_target, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&s_target_ready, 0, __ATOMIC_RELEASE);
}

bool saltysd_display_sync_active(void) {
  /* A busy/missing IPC port is not a successful sync. Only activate cadence
   * sampling once the FPSLocker target byte is actually mapped. */
  return __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE);
}

float saltysd_display_sync_rate(void) {
  if (!saltysd_display_sync_active())
    return 0.0f;
  return (float)estimate_present_fps();
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

unsigned saltysd_display_sync_published_target(void) {
  return __atomic_load_n(&s_published_target, __ATOMIC_ACQUIRE);
}

bool saltysd_display_sync_target_ready(void) {
  return __atomic_load_n(&s_target_ready, __ATOMIC_ACQUIRE) != 0;
}
