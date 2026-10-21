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
static volatile int s_last_query_status = -1;
static volatile uint64_t s_last_present_tick;
static volatile uint64_t s_present_period_ticks;
static uint64_t s_next_query_tick;

static uint64_t atomic_load_u64(const volatile uint64_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static int saltysd_query_refresh_rate(uint8_t *rate) {
  Handle session = INVALID_HANDLE;
  const Result connected = svcConnectToNamedPort(&session, SALTYSD_PORT);
  if (R_FAILED(connected))
    return 0;

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
  return valid ? 2 : 1; /* 0 = no named port, 1 = query/IPC failure */
}

void saltysd_display_sync_configure(bool enabled) {
  __atomic_store_n(&s_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
  if (!enabled) {
    __atomic_store_n(&s_port_available, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_service_rate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_query_status, -1, __ATOMIC_RELEASE);
  }
}

void saltysd_display_sync_poll(void) {
  if (!__atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE))
    return;

  const uint64_t now = armGetSystemTick();
  if (s_next_query_tick && now < s_next_query_tick)
    return;
  s_next_query_tick = now + armGetSystemTickFreq();

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
  if (elapsed < frequency / 240u || elapsed > frequency * 2u) {
    __atomic_store_n(&s_present_period_ticks, 0, __ATOMIC_RELEASE);
    return;
  }
  uint64_t average = atomic_load_u64(&s_present_period_ticks);
  if (!average)
    average = elapsed;
  else
    average = (average * 7u + elapsed) / 8u;
  __atomic_store_n(&s_present_period_ticks, average, __ATOMIC_RELEASE);
}

bool saltysd_display_sync_active(void) {
  return __atomic_load_n(&s_enabled, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&s_port_available, __ATOMIC_ACQUIRE);
}

float saltysd_display_sync_rate(void) {
  if (!saltysd_display_sync_active())
    return 0.0f;
  const uint32_t service_rate = __atomic_load_n(&s_service_rate, __ATOMIC_ACQUIRE);
  if (service_rate)
    return (float)service_rate;
  const uint64_t period = atomic_load_u64(&s_present_period_ticks);
  if (!period)
    return 0.0f;
  return (float)armGetSystemTickFreq() / (float)period;
}

int saltysd_display_sync_query_status(void) {
  return __atomic_load_n(&s_last_query_status, __ATOMIC_ACQUIRE);
}

unsigned saltysd_display_sync_service_rate(void) {
  return __atomic_load_n(&s_service_rate, __ATOMIC_ACQUIRE);
}
