#include <stdio.h>
#include <stdint.h>
#include "../source/saltysd_fps_policy.h"

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
    return 1; \
  } \
} while (0)

int main(void) {
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

  puts("FPSLocker telemetry policy: all checks passed");
  return 0;
}
