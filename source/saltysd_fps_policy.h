/* Pure FPSLocker rate policy shared by the Switch bridge and host tests. */
#ifndef NETHERSX2_SALTYSD_FPS_POLICY_H
#define NETHERSX2_SALTYSD_FPS_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define SALTYSD_FPSLOCKER_MIN_TARGET 45u
#define SALTYSD_FPSLOCKER_MAX_TARGET 60u
#define SALTYSD_FPSLOCKER_STEP 5u

typedef struct {
  unsigned average_fps;
  unsigned p90_fps;
} SaltysdFpsEstimate;

/* Input intervals are valid presentation intervals in system ticks.
 * The upper-tail FPS estimate is the reciprocal of the 10th-percentile frame
 * interval, which is the 90th percentile of FPS. Slow isolated hitches therefore
 * do not pull the panel request down. */
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

/* The handheld refresh table only accepts 45/50/55/60 for this bridge. Round
 * upward so an observed 49 FPS never asks the panel to run at 45 Hz. */
static inline unsigned saltysd_fpslocker_ceil_target(unsigned fps) {
  if (!fps)
    return 0;
  if (fps <= 45)
    return 45;
  if (fps <= 50)
    return 50;
  if (fps <= 55)
    return 55;
  return 60;
}

/* Hysteresis is applied one legal panel step at a time. A down-step requires
 * the measured high-percentile rate to be at least 5 Hz below the current target;
 * the caller holds any candidate step for the stability interval. */
static inline unsigned saltysd_fpslocker_next_step(unsigned current,
                                                    unsigned measured_fps) {
  const unsigned desired = saltysd_fpslocker_ceil_target(measured_fps);
  if (!desired)
    return current;
  if (!current || current < 45 || current > 60 || current % 5u)
    return desired;
  if (desired > current)
    return current + SALTYSD_FPSLOCKER_STEP;
  if (desired < current && measured_fps <= current - SALTYSD_FPSLOCKER_STEP)
    return current - SALTYSD_FPSLOCKER_STEP;
  return current;
}

/* Track a target candidate and report only after it has remained unchanged for
 * the requested interval. State is caller-owned so the helper is host-testable. */
static inline int saltysd_fpslocker_candidate_stable(
    unsigned target, unsigned *candidate, uint64_t now_ticks,
    uint64_t *candidate_since_ticks, uint64_t stable_ticks) {
  if (!target || !candidate || !candidate_since_ticks)
    return 0;
  if (*candidate != target || !*candidate_since_ticks ||
      now_ticks < *candidate_since_ticks) {
    *candidate = target;
    *candidate_since_ticks = now_ticks;
    return 0;
  }
  return now_ticks - *candidate_since_ticks >= stable_ticks;
}

#endif /* NETHERSX2_SALTYSD_FPS_POLICY_H */
