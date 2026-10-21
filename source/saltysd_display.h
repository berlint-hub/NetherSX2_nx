#ifndef NETHERSX2_SALTYSD_DISPLAY_H
#define NETHERSX2_SALTYSD_DISPLAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in display pacing bridge. SaltySD IPC is throttled and thread-safe; the
 * frontend polls while running and the Vulkan backend makes one opportunistic
 * poll at initial swapchain creation so SaltyNX has time to attach first.
 * Present backends otherwise read the cached, lock-free state. */
void saltysd_display_sync_configure(bool enabled);
void saltysd_display_sync_poll(void);
void saltysd_display_sync_note_present(void);
bool saltysd_display_sync_active(void);
/* SaltySD rate, or a confident 50/60 Hz estimate from recent presentation
 * timestamps; returns 0 while fallback timing is ambiguous. */
float saltysd_display_sync_rate(void);
/* Last one-shot/query status: -1 not queried/disabled, 0 service unavailable,
 * 1 service present but query failed/session busy, 2 refresh-rate query succeeded. */
int saltysd_display_sync_query_status(void);
unsigned saltysd_display_sync_service_rate(void);
unsigned saltysd_display_sync_connect_result(void);

#ifdef __cplusplus
}
#endif

#endif
