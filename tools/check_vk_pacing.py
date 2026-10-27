#!/usr/bin/env python3
"""Compile and test the Vulkan low-latency deadline decision from vk.c.

The production helper is extracted verbatim so a late frame cannot silently
regress into sleeping for an extra display refresh. This test needs only a host
C compiler; it does not require Vulkan headers or the Switch toolchain.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "source/hooks/vk.c").read_text()
START = "static uint64_t low_latency_pace_delay_ns("
END = "\n/* Present returns quickly with MAILBOX"
start = SOURCE.index(START)
end = SOURCE.index(END, start)
helper = SOURCE[start:end]

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>

@@HELPER@@

static int fails;
static void check(uint64_t actual, uint64_t expected, const char *label) {
  if (actual != expected) {
    printf("FAIL %s: got %llu, expected %llu\n", label,
           (unsigned long long)actual, (unsigned long long)expected);
    ++fails;
  } else {
    printf("PASS %s\n", label);
  }
}

int main(void) {
  const uint64_t next = UINT64_C(1020000000); /* 50 Hz after prior present */
  const uint64_t period = UINT64_C(20000000);
  const uint64_t lead = UINT64_C(2000000);
  unsigned long long skipped = 0, late = 0;

  check(low_latency_pace_delay_ns(UINT64_C(1000000000), next, period, lead,
                                  &skipped, &late),
        UINT64_C(18000000), "early frame waits only until the lead-in");
  check(low_latency_pace_delay_ns(UINT64_C(1018000000), next, period, lead,
                                  &skipped, &late),
        0, "at the submit deadline it does not sleep");
  check(late, 1, "a missed lead-in is counted");
  check(low_latency_pace_delay_ns(UINT64_C(1019500000), next, period, lead,
                                  &skipped, &late),
        0, "inside the final 0.5 ms before vblank it submits immediately");
  check(low_latency_pace_delay_ns(UINT64_C(1021000000), next, period, lead,
                                  &skipped, &late),
        0, "after a missed vblank it never waits for the next refresh");
  check(skipped, 1, "one missed refresh is counted");
  check(low_latency_pace_delay_ns(UINT64_C(1060000000), next, period, lead,
                                  &skipped, &late),
        0, "several missed refreshes still submit immediately");
  check(skipped, 4, "all elapsed refresh deadlines are counted");
  check(low_latency_pace_delay_ns(100, 0, period, lead, &skipped, &late),
        0, "no vblank anchor means no delay");
  check(low_latency_pace_delay_ns(100, next, 0, lead, &skipped, &late),
        0, "invalid refresh period means no delay");
  return fails ? 1 : 0;
}
"""

with tempfile.TemporaryDirectory(prefix="vkpace-") as tmp:
    c_file = Path(tmp) / "pacing_test.c"
    binary = Path(tmp) / "pacing_test"
    c_file.write_text(HARNESS.replace("@@HELPER@@", helper))
    cc = os.environ.get("CC", "gcc")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
                    "-o", str(binary), str(c_file)], check=True)
    subprocess.run([str(binary)], check=True)
