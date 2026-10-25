/* SPDX-License-Identifier: MIT */
/* Pure fractional frame schedule shared with host-side validation. */
#ifndef NETHERSX2_LSFG_SCHEDULE_H
#define NETHERSX2_LSFG_SCHEDULE_H

/* Do not ask a fixed-refresh panel to consume frames faster than it refreshes.
 * A missing/invalid panel rate leaves the user's configured target unchanged. */
static inline double lsfg_effective_target_fps(double configured_fps,
                                               double panel_hz) {
  if (!(configured_fps > 0.0))
    return 0.0;
  return panel_hz > 0.0 && panel_hz < configured_fps
      ? panel_hz : configured_fps;
}

/* Tiny deficits are usually cadence noise and do not justify routing every
 * source frame through the capture bridge. Require at least 2% headroom before
 * enabling fractional capture/generation. */
static inline int lsfg_fractional_should_activate(double source_fps,
                                                  double target_fps) {
  return source_fps > 0.0 && target_fps > 0.0 &&
         source_fps < target_fps * 0.98;
}

/* Decide whether this source present earns interpolation for the requested
 * output rate. The bridge emits two output frames when interpolating and one
 * when capturing, so the extra-frame fraction is target/source - 1, clamped to
 * [0, 1]. A source at or above target therefore passes through unchanged.
 *
 * phase is an error accumulator in [0,1); native_gap means the bridge missed a
 * source present, so capture one frame to re-warm its previous-frame slot before
 * another interpolation. */
static inline int lsfg_fractional_schedule(double source_fps, double target_fps,
                                           double *phase, int *native_gap) {
  if (!phase || !native_gap)
    return 0;
  if (*native_gap) {
    *native_gap = 0;
    return 0;
  }
  if (!(source_fps > 0.0) || !(target_fps > 0.0))
    return 0;

  double ratio = target_fps / source_fps;
  if (ratio < 1.0)
    ratio = 1.0;
  if (ratio > 2.0)
    ratio = 2.0;

  *phase += ratio - 1.0;
  if (*phase >= 1.0) {
    *phase -= 1.0;
    return 1;
  }
  return 0;
}

/* While fractional mode is still measuring (or has not found a source rate
 * below target), native presentation avoids the bridge's copy/submit overhead.
 * Once ready, every source frame must pass through the bridge for interpolation. */
static inline int lsfg_fractional_use_native_passthrough(int fractional_mode,
                                                         int ready) {
  return fractional_mode && !ready;
}

#endif /* NETHERSX2_LSFG_SCHEDULE_H */
