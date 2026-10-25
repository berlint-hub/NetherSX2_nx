#include "lsfg_schedule.h"

#include <stdio.h>

static int check(const char *name, int condition) {
  if (condition)
    return 0;
  fprintf(stderr, "FAIL: %s\n", name);
  return 1;
}

static unsigned generated_count(double source_fps, double target_fps,
                               unsigned presents) {
  double phase = 0.0;
  int native_gap = 0;
  unsigned generated = 0;
  for (unsigned i = 0; i < presents; ++i)
    generated += (unsigned)lsfg_fractional_schedule(
        source_fps, target_fps, &phase, &native_gap);
  return generated;
}

int main(void) {
  int failures = 0;
  failures += check("60 Hz request is capped to a 50 Hz panel",
                    lsfg_effective_target_fps(60.0, 50.0) == 50.0);
  failures += check("lower configured target is preserved",
                    lsfg_effective_target_fps(40.0, 50.0) == 40.0);
  failures += check("unknown panel rate does not change target",
                    lsfg_effective_target_fps(60.0, 0.0) == 60.0);
  failures += check("invalid configured target is disabled",
                    lsfg_effective_target_fps(0.0, 50.0) == 0.0);
  failures += check("44-to-50 source enters fractional mode",
                    lsfg_fractional_should_activate(44.0, 50.0));
  failures += check("near-target cadence noise stays native",
                    !lsfg_fractional_should_activate(39.5, 40.0) &&
                    !lsfg_fractional_should_activate(40.0, 40.0));
  failures += check("meaningful source deficit activates fractional mode",
                    lsfg_fractional_should_activate(39.0, 40.0));

  failures += check("50-to-50 passes through",
                    generated_count(50.0, 50.0, 300) == 0);
  failures += check("source above target passes through",
                    generated_count(60.0, 50.0, 300) == 0);
  failures += check("25-to-50 reaches 2x",
                    generated_count(25.0, 50.0, 300) == 300);
  failures += check("43-to-50 uses fractional cadence",
                    generated_count(43.0, 50.0, 300) == 48);
  failures += check("44.444-to-50 schedules one in eight",
                    generated_count(44.44444444444444, 50.0, 360) == 45);
  failures += check("49-to-50 preserves low generation cadence",
                    generated_count(49.0, 50.0, 490) == 10);
  failures += check("27.3-to-50 rounds accumulated phase",
                    generated_count(27.3, 50.0, 300) == 249);
  failures += check("ratio is capped at 2x",
                    generated_count(10.0, 50.0, 300) == 300);
  failures += check("invalid rates do not generate",
                    generated_count(0.0, 50.0, 300) == 0 &&
                    generated_count(30.0, 0.0, 300) == 0);

  double phase = 0.625;
  int native_gap = 1;
  failures += check("native gap forces one capture and preserves phase",
                    !lsfg_fractional_schedule(25.0, 50.0, &phase, &native_gap) &&
                    !native_gap && phase == 0.625);
  failures += check("scheduler resumes after re-warm capture",
                    lsfg_fractional_schedule(25.0, 50.0, &phase, &native_gap) &&
                    phase == 0.625);
  failures += check("null scheduler state is rejected",
                    !lsfg_fractional_schedule(25.0, 50.0, NULL, &native_gap) &&
                    !lsfg_fractional_schedule(25.0, 50.0, &phase, NULL));
  failures += check("fractional warm-up stays on the native path",
                    lsfg_fractional_use_native_passthrough(1, 0));
  failures += check("latched fractional source enters the bridge",
                    !lsfg_fractional_use_native_passthrough(1, 1));
  failures += check("non-fractional mode keeps its existing routing",
                    !lsfg_fractional_use_native_passthrough(0, 0));

  if (failures)
    return 1;
  puts("LSFG fractional schedule boundaries, ratios, and re-warm behavior passed");
  return 0;
}
