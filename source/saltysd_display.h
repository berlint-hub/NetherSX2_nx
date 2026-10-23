#ifndef NETHERSX2_SALTYSD_DISPLAY_H
#define NETHERSX2_SALTYSD_DISPLAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in FPSLocker bridge. Present backends feed timestamps; stable cadence is
 * published as a handheld-only FPSlocked panel request, not an emulator cap.
 * FPSLocker owns the display-sync toggle; SaltySD IPC is throttled to ~1 Hz. */
void saltysd_display_sync_configure(bool enabled);
/* FPSLocker owns SetBuffers; the Vulkan backend only reads this boot-time
 * request and reports the actual swapchain image count through Buffers. */
void saltysd_fpslocker_buffers_configure(bool enabled);
unsigned saltysd_fpslocker_buffers_request(void);
void saltysd_fpslocker_buffers_report_actual(unsigned image_count);
void saltysd_display_sync_poll(void);
/* Publish the latest integer cadence to SaltyNX at 10 Hz, without doing IPC. */
void saltysd_display_sync_publish(void);
void saltysd_display_sync_shutdown(void);
void saltysd_display_sync_note_present(void);
bool saltysd_display_sync_active(void);
/* Estimated NetherSX2 present rate from recent presentation timestamps; zero
 * until the cadence is stable enough to publish as an FPSLocker target. */
float saltysd_display_sync_rate(void);
/* Last one-shot/query status: -1 not queried/disabled, 0 service unavailable,
 * 1 service present but query failed/session busy, 2 refresh-rate query succeeded. */
int saltysd_display_sync_query_status(void);
unsigned saltysd_display_sync_service_rate(void);
unsigned saltysd_display_sync_connect_result(void);
unsigned saltysd_display_sync_nethfps_connect_result(void);
unsigned saltysd_display_sync_legacy_connect_result(void);
/* 0 = no successful connection, 1 = NethFPS, 2 = legacy SaltySD. */
unsigned saltysd_display_sync_connect_port(void);
unsigned saltysd_display_sync_published_target(void);
bool saltysd_display_sync_target_ready(void);
unsigned saltysd_display_sync_session_limit_result(void);

#ifdef __cplusplus
}
#endif

#endif
