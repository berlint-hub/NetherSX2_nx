/* Minimal SaltySD display-rate client. The request/response layout follows the
 * public SaltyNX SaltySD IPC protocol (GetDisplayRefreshRate, command 10).
 * Keep this non-blocking: unlike SaltySD_Connect() in some client headers, do
 * not retry/sleep on startup when the named port is absent. */
#include <stdint.h>
#include <string.h>
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/arm/tls.h>
#include <switch/arm/counter.h>

#include "saltysd_display.h"

#define SALTYSD_PORT "SaltySD"
#define SFCI_MAGIC 0x49434653u
#define SFCO_MAGIC 0x4f434653u
#define IPC_REQUEST 4u
#define SALTYSD_GET_DISPLAY_REFRESH_RATE 10u

static volatile int s_enabled;
static volatile int s_port_available;
static volatile uint32_t s_service_rate;
static volatile uint32_t s_last_connect_result = UINT32_MAX;
static volatile int s_last_query_status = -1;
static volatile uint64_t s_last_present_tick;
#define PRESENT_INTERVAL_SAMPLES 16u
static volatile uint64_t s_present_intervals[PRESENT_INTERVAL_SAMPLES];
static volatile uint32_t s_present_interval_count;
static volatile uint64_t s_next_query_tick;

static int saltysd_query_refresh_rate(uint8_t *rate) {
  Handle session = INVALID_HANDLE;
  const Result connected = svcConnectToNamedPort(&session, SALTYSD_PORT);
  __atomic_store_n(&s_last_connect_result, (uint32_t)connected, __ATOMIC_RELEASE);
  if (R_FAILED(connected)) {
    /* SaltyNX currently creates SaltySD with a single session slot. During
     * SaltyNX Core startup that slot can be occupied, making connect return
     * KernelError_OutOfResource even though the service is present. Treat
     * this as a failed rate query, not as an absent port: presentation timing
     * is still usable and swap interval 1 should be enabled. */
    if (R_MODULE(connected) == Module_Kernel &&
        R_DESCRIPTION(connected) == KernelError_OutOfResource)
      return 1;
    return 0;
  }

  /* HIPC request with PID descriptor and a 32-byte SFCI payload. The raw
   * request starts at TLS+0x20 after the aligned PID descriptor. */
  uint32_t *tls = (uint32_t *)armGetTls();
  memset(tls, 0, 0x100);
  tls[0] = IPC_REQUEST;
  tls[1] = 0x8000000cu;        /* 12 raw words plus the special-header flag */
  tls[2] = 1;                  /* special header: send caller PID */
  uint64_t *request = (uint64_t *)((uint8_t *)tls + 0x20);
  request[0] = SFCI_MAGIC;
  request[1] = SALTYSD_GET_DISPLAY_REFRESH_RATE;

  const Result sent = svcSendSyncRequest(session);
  int valid = 0;
  if (R_SUCCEEDED(sent)) {
    const uint32_t ctrl0 = tls[0];
    const uint32_t ctrl1 = tls[1];
    uint32_t *cursor = tls + 2;
    if (ctrl1 & 0x80000000u) {
      const uint32_t special = *cursor++;
      if (special & 1u) cursor += 2; /* response PID */
      cursor += ((special >> 1) & 15u) + ((special >> 5) & 15u);
    }
    cursor += ((ctrl0 >> 16) & 15u) * 2u;
    cursor += (((ctrl0 >> 20) & 15u) + ((ctrl0 >> 24) & 15u) +
               ((ctrl0 >> 28) & 15u)) * 3u;
    const uintptr_t raw_address = ((uintptr_t)cursor + 15u) & ~(uintptr_t)15u;
    const uint32_t response_bytes = (ctrl1 & 0x1ffu) * 4u;
    if (response_bytes >= 24u && raw_address + response_bytes <=
        (uintptr_t)tls + 0x100u) {
      const uint64_t *response = (const uint64_t *)raw_address;
      const uint64_t protocol_result = response[1];
      const uint64_t refresh_rate = response[2];
      if ((uint32_t)response[0] == SFCO_MAGIC && protocol_result == 0 &&
          refresh_rate >= 20 && refresh_rate <= 240) {
        *rate = (uint8_t)refresh_rate;
        valid = 1;
      }
    }
  }
  svcCloseHandle(session);
  return valid ? 2 : 1; /* 0 = unavailable, 1 = present/query failed, 2 = rate */
}

void saltysd_display_sync_configure(bool enabled) {
  __atomic_store_n(&s_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
  if (!enabled) {
    __atomic_store_n(&s_port_available, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_query_status, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_connect_result, UINT32_MAX, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_present_tick, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_present_interval_count, 0, __ATOMIC_RELEASE);
  }
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

  uint8_t rate = 0;
  const int query_status = saltysd_query_refresh_rate(&rate);
  __atomic_store_n(&s_last_query_status, query_status, __ATOMIC_RELEASE);
  if (query_status == 2) {
    __atomic_store_n(&s_service_rate, rate, __ATOMIC_RELEASE);
    __atomic_store_n(&s_port_available, 1, __ATOMIC_RELEASE);
  } else if (query_status == 1) {
    /* Service exists but the rate query failed; use presentation timing. */
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_port_available, 1, __ATOMIC_RELEASE);
  } else {
    /* No service: make the setting a true no-op and keep today's pacing. */
    __atomic_store_n(&s_port_available, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
  }
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

  /* Keep recent present intervals. A raw presents-per-second average is not a
   * display refresh estimate: a slow guest can present at 20 FPS on a 50/60 Hz
   * display. Instead, rate estimation below checks for a stable 50/60 Hz
   * cadence and its integer-multiple intervals (missed refreshes). */
  const uint32_t index = __atomic_fetch_add(&s_present_interval_count, 1,
                                             __ATOMIC_ACQ_REL);
  __atomic_store_n(&s_present_intervals[index % PRESENT_INTERVAL_SAMPLES], elapsed,
                   __ATOMIC_RELEASE);
}

bool saltysd_display_sync_active(void) {
  return __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&s_port_available, __ATOMIC_ACQUIRE);
}

static unsigned estimate_present_display_rate(void) {
  const uint64_t frequency = armGetSystemTickFreq();
  uint32_t count = __atomic_load_n(&s_present_interval_count, __ATOMIC_ACQUIRE);
  if (count > PRESENT_INTERVAL_SAMPLES)
    count = PRESENT_INTERVAL_SAMPLES;
  if (count < 4)
    return 0;

  unsigned matches_50 = 0, matches_60 = 0;
  uint64_t error_50 = 0, error_60 = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint64_t interval = __atomic_load_n(&s_present_intervals[i],
                                               __ATOMIC_ACQUIRE);
    if (!interval)
      continue;

    const uint64_t period_50 = frequency / 50u;
    const uint64_t period_60 = frequency / 60u;
    const uint64_t periods_50 = (interval + period_50 / 2u) / period_50;
    const uint64_t periods_60 = (interval + period_60 / 2u) / period_60;
    if (periods_50 >= 1 && periods_50 <= 8) {
      const uint64_t expected = periods_50 * period_50;
      const uint64_t delta = interval > expected ? interval - expected : expected - interval;
      if (delta <= period_50 / 6u) {
        matches_50++;
        error_50 += (delta * 1000u) / period_50;
      }
    }
    if (periods_60 >= 1 && periods_60 <= 8) {
      const uint64_t expected = periods_60 * period_60;
      const uint64_t delta = interval > expected ? interval - expected : expected - interval;
      if (delta <= period_60 / 6u) {
        matches_60++;
        error_60 += (delta * 1000u) / period_60;
      }
    }
  }

  if (matches_50 < 4 && matches_60 < 4)
    return 0;
  if (matches_50 >= matches_60 + 2)
    return 50;
  if (matches_60 >= matches_50 + 2)
    return 60;
  if (matches_50 >= 4 && matches_60 >= 4) {
    const uint64_t avg_50 = error_50 / matches_50;
    const uint64_t avg_60 = error_60 / matches_60;
    if (avg_50 + 40u < avg_60)
      return 50;
    if (avg_60 + 40u < avg_50)
      return 60;
  }
  return 0; /* ambiguous timing: keep the core's PAL/NTSC defaults */
}

float saltysd_display_sync_rate(void) {
  if (!saltysd_display_sync_active())
    return 0.0f;
  const uint32_t service_rate = __atomic_load_n(&s_service_rate, __ATOMIC_ACQUIRE);
  if (service_rate)
    return (float)service_rate;
  /* Only accept a stable display cadence inferred from recent timestamps;
   * never mistake a slow game's delivered FPS for the panel refresh rate. */
  return (float)estimate_present_display_rate();
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
