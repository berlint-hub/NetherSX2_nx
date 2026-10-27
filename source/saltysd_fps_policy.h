/* Pure FPSLocker display-sync policy shared by the Switch bridge and host tests. */
#ifndef NETHERSX2_SALTYSD_FPS_POLICY_H
#define NETHERSX2_SALTYSD_FPS_POLICY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned average_fps;
  unsigned p90_fps;
} SaltysdFpsEstimate;

/* Input intervals are valid presentation intervals in system ticks.
 * The upper-tail FPS estimate is the reciprocal of the 10th-percentile frame
 * interval, which is the 90th percentile of FPS. Slow isolated hitches therefore
 * do not pull the panel report down. */
static inline SaltysdFpsEstimate
saltysd_estimate_fps(const uint64_t *intervals, size_t count,
                     uint64_t tick_frequency, size_t minimum_samples) {
  SaltysdFpsEstimate result = {0, 0};
  if (!intervals || !tick_frequency || count < minimum_samples || !count)
    return result;

  uint64_t sorted[64];
  if (count > sizeof(sorted) / sizeof(sorted[0]))
    count = sizeof(sorted) / sizeof(sorted[0]);
  uint64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint64_t value = intervals[i];
    size_t j = i;
    while (j && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
    total += value;
  }
  if (!total)
    return result;

  const uint64_t average = (tick_frequency * count + total / 2u) / total;
  const size_t p10_index = (count - 1u) / 10u;
  const uint64_t p90 = (tick_frequency + sorted[p10_index] / 2u) /
                       sorted[p10_index];
  if (average >= 14 && average <= 120)
    result.average_fps = (unsigned)average;
  if (p90 >= 14 && p90 <= 120)
    result.p90_fps = (unsigned)p90;
  return result;
}


#endif /* NETHERSX2_SALTYSD_FPS_POLICY_H */
