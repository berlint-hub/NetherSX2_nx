#ifndef NETHERSX2_SALTYSD_DISPLAY_H
#define NETHERSX2_SALTYSD_DISPLAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in display pacing bridge. All SaltySD IPC runs on the frontend thread;
 * present backends only read the cached, lock-free state. */
void saltysd_display_sync_configure(bool enabled);
void saltysd_display_sync_poll(void);
void saltysd_display_sync_note_present(void);
bool saltysd_display_sync_active(void);
float saltysd_display_sync_rate(void);

#ifdef __cplusplus
}
#endif

#endif
