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
  CHECK(saltysd_fpslocker_ceil_target(49) == 50);
  CHECK(saltysd_fpslocker_ceil_target(45) == 45);
  CHECK(saltysd_fpslocker_ceil_target(44) == 45);
  CHECK(saltysd_fpslocker_ceil_target(52) == 55);
  CHECK(saltysd_fpslocker_ceil_target(60) == 60);
  CHECK(saltysd_fpslocker_ceil_target(0) == 0);

  CHECK(saltysd_fpslocker_next_step(60, 49) == 55);
  CHECK(saltysd_fpslocker_next_step(55, 49) == 50);
  CHECK(saltysd_fpslocker_next_step(50, 49) == 50);
  CHECK(saltysd_fpslocker_next_step(50, 52) == 55);
  CHECK(saltysd_fpslocker_next_step(45, 60) == 50);
  CHECK(saltysd_fpslocker_next_step(0, 49) == 50);

  unsigned candidate = 0;
  uint64_t candidate_since = 0;
  CHECK(!saltysd_fpslocker_candidate_stable(50, &candidate,
                                            UINT64_C(100), &candidate_since,
                                            UINT64_C(2000)));
  CHECK(candidate == 50 && candidate_since == 100);
  CHECK(!saltysd_fpslocker_candidate_stable(50, &candidate,
                                            UINT64_C(2099), &candidate_since,
                                            UINT64_C(2000)));
  CHECK(saltysd_fpslocker_candidate_stable(50, &candidate,
                                           UINT64_C(2100), &candidate_since,
                                           UINT64_C(2000)));
  CHECK(!saltysd_fpslocker_candidate_stable(55, &candidate,
                                            UINT64_C(2100), &candidate_since,
                                            UINT64_C(2000)));
  CHECK(candidate == 55 && candidate_since == 2100);
  CHECK(!saltysd_fpslocker_candidate_stable(55, &candidate,
                                            UINT64_C(4099), &candidate_since,
                                            UINT64_C(2000)));
  CHECK(saltysd_fpslocker_candidate_stable(55, &candidate,
                                           UINT64_C(4100), &candidate_since,
                                           UINT64_C(2000)));

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

  puts("FPSLocker rate policy: all checks passed");
  return 0;
}
