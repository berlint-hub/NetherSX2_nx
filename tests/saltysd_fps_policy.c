#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../source/saltysd_fps_policy.h"

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
    return 1; \
  } \
} while (0)

int main(void) {
  CHECK(!saltysd_fpslocker_display_sync_enabled(0));
  CHECK(saltysd_fpslocker_display_sync_enabled(
      SALTYSD_FPSLOCKER_DISPLAY_SYNC_HANDHELD));
  CHECK(saltysd_fpslocker_display_sync_enabled(
      SALTYSD_FPSLOCKER_DISPLAY_SYNC_DOCKED));
  CHECK(saltysd_fpslocker_display_sync_enabled(
      SALTYSD_FPSLOCKER_DISPLAY_SYNC_HANDHELD |
      SALTYSD_FPSLOCKER_DISPLAY_SYNC_DOCKED));
  CHECK(!saltysd_fpslocker_display_sync_enabled(0x04));

  uint64_t intervals[64];
  for (size_t i = 0; i < 64; ++i)
    intervals[i] = UINT64_C(20000000); /* 50 Hz at a 1 GHz test clock */
  intervals[0] = UINT64_C(100000000); /* one severe hitch should not downshift */
  SaltysdFpsEstimate estimate =
      saltysd_estimate_fps(intervals, 64, UINT64_C(1000000000), 60);
  CHECK(estimate.average_fps == 47);
  CHECK(estimate.p90_fps == 50);
  CHECK(saltysd_estimate_fps(intervals, 59, UINT64_C(1000000000), 60)
            .average_fps == 0);

  puts("FPSLocker display-sync policy: all checks passed");
  return 0;
}
