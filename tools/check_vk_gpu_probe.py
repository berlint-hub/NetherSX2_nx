#!/usr/bin/env python3
"""Compile and run the GPU-wait instrumentation from source/hooks/vk.c on the host.

The point is the same as tests/patches: a shim whose parameter list does not
match the real Vulkan entry point still compiles, because forwarding an
argument silently widens it, and the mistake only shows up as corruption on
hardware -- which is what the reversed svcGetThreadCoreMask argument order
looked like. So the block is extracted from vk.c VERBATIM (never retyped),
compiled against the real vulkan_core.h from the pinned NXVK package with
-Wall -Wextra -Wformat=2, and then exercised with fake driver functions:
counters must move for blocking waits, must not move for polls, must not
consume clock reads on the poll path, must count a batch of pipelines, and the
report must not divide by zero on an empty window.

No devkitPro and no GPU needed, only a host gcc. The Vulkan headers come from
the nxvk-vulkan-tegra-x1.tar.gz that CI already extracts; pass their include
directory with --vulkan-include or let the candidates below find them.

Exit status is 0 only when the harness compiled without warnings AND every
check passed.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# The hitch instrumentation shares this region and reaches into two other
# modules, so their types come from the headers verbatim (never retyped here)
# and their functions are faked in the preamble.
HDR_TYPES = (
    (os.path.join("source", "libc_shim.h"),
     "typedef struct {\n  unsigned long long calls;", "} FileIoStats;"),
    (os.path.join("source", "pthr.h"),
     "#define PTHR_CPU_ROLES 3", "#define PTHR_CPU_ROLES 3"),
    (os.path.join("source", "pthr.h"),
     "typedef struct {\n  uint64_t ticks[PTHR_CPU_ROLES];", "} CpuTimeSample;"),
)

BEGIN = "/* Which side of the CPU/GPU boundary"
END = "static void perf_report(uint64_t now) {"
REPORT_BEGIN = "  /* Third line of the same window"
REPORT_END = "  gpu_fence_calls = gpu_fence_ns = gpu_fence_polls = 0;"

PREAMBLE = r"""
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint64_t fake_clock = 0;
static uint64_t clock_step = 0;
static uint64_t lsfg_monotonic_ns(void) {
  const uint64_t t = fake_clock;
  fake_clock += clock_step;
  return t;
}
static void vk_diag_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vk_diag_note(const char *fmt, ...) { (void)fmt; }

/* The two other modules the per-frame hitch line reads. Types are pasted in
 * verbatim by the generator; only the functions are faked. */
__HDR_TYPES__
static FileIoStats fake_io;
static CpuTimeSample fake_cpu;
static void fileio_stats_snapshot(FileIoStats *out) { *out = fake_io; }
static void pthr_cpu_time_sample(CpuTimeSample *out) { *out = fake_cpu; }
static int pthr_cpu_time_ms(const CpuTimeSample *from, const CpuTimeSample *to,
                            char *buf, size_t len) {
  (void)from; (void)to;
  return snprintf(buf, len, "EE 0.0 GS 0.0 MTVU 0.0 ms") > 0;
}

"""

EPILOGUE = r"""
/* perf_report's new body, verbatim, with only its inputs faked. */
static void report_body(double seconds) {
__REPORT__
}

static int fake_fence_calls;
static VkResult VKAPI_CALL fake_wait(VkDevice d, uint32_t n, const VkFence *f,
                                     VkBool32 a, uint64_t t) {
  (void)d; (void)n; (void)f; (void)a; (void)t;
  fake_fence_calls++;
  return VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_idle(VkQueue q) { (void)q; return VK_SUCCESS; }
static int fake_pipe_last_count;
static VkResult VKAPI_CALL fake_pipes(VkDevice d, VkPipelineCache c, uint32_t n,
                                      const VkGraphicsPipelineCreateInfo *i,
                                      const VkAllocationCallbacks *a, VkPipeline *o) {
  (void)d; (void)c; (void)i; (void)a; (void)o;
  fake_pipe_last_count = (int)n;
  return VK_SUCCESS;
}
static int fake_barrier_calls;
static void VKAPI_CALL fake_barrier(VkCommandBuffer cb, VkPipelineStageFlags s,
                                    VkPipelineStageFlags dt, VkDependencyFlags fl,
                                    uint32_t mc, const VkMemoryBarrier *m,
                                    uint32_t bc, const VkBufferMemoryBarrier *b,
                                    uint32_t ic, const VkImageMemoryBarrier *im) {
  (void)cb; (void)s; (void)dt; (void)fl; (void)mc; (void)m;
  (void)bc; (void)b; (void)ic; (void)im;
  fake_barrier_calls++;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fail = 1; } } while (0)

int main(void) {
  int fail = 0;

  /* The probe table must install exactly these seven shims, and must not claim
   * a function the driver did not hand over. */
  static const char *const want[] = {
    "vkWaitForFences", "vkQueueWaitIdle", "vkCreateGraphicsPipelines",
    "vkCmdPipelineBarrier", "vkCmdCopyImageToBuffer", "vkCmdCopyImage",
    "vkCmdBlitImage",
  };
  for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); ++i)
    CHECK(gpu_probe_lookup(want[i], (PFN_vkVoidFunction)1) != NULL, "%s not probed", want[i]);
  CHECK(gpu_probe_lookup("vkCmdDraw", (PFN_vkVoidFunction)1) == NULL, "vkCmdDraw probed");
  CHECK(gpu_probe_lookup(NULL, (PFN_vkVoidFunction)1) == NULL, "NULL name probed");
  CHECK(gpu_probe_lookup("vkWaitForFences", NULL) == NULL, "NULL fn installed");

  /* Unreachable-by-construction guards: a shim must never call through NULL. */
  real_wait_for_fences = NULL;
  CHECK(vkWaitForFences_shim(NULL, 0, NULL, VK_TRUE, 1) == VK_ERROR_INITIALIZATION_FAILED,
        "fence guard");
  real_queue_wait_idle = NULL;
  CHECK(vkQueueWaitIdle_shim(NULL) == VK_ERROR_INITIALIZATION_FAILED, "idle guard");
  real_create_graphics_pipelines = NULL;
  CHECK(vkCreateGraphicsPipelines_shim(NULL, NULL, 0, NULL, NULL, NULL) ==
            VK_ERROR_INITIALIZATION_FAILED, "pipeline guard");
  real_cmd_pipeline_barrier = NULL;
  vkCmdPipelineBarrier_shim(NULL, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
  real_cmd_copy_image_to_buffer = NULL;
  vkCmdCopyImageToBuffer_shim(NULL, NULL, 0, NULL, 0, NULL);
  real_cmd_copy_image = NULL;
  vkCmdCopyImage_shim(NULL, NULL, 0, NULL, 0, 0, NULL);
  real_cmd_blit_image = NULL;
  vkCmdBlitImage_shim(NULL, NULL, 0, NULL, 0, 0, NULL, 0);

  gpu_fence_calls = gpu_fence_ns = gpu_fence_polls = 0;
  gpu_idle_calls = gpu_idle_ns = 0;
  gpu_pipe_calls = gpu_pipe_ns = gpu_barrier_calls = 0;
  clock_step = 3000000; /* 3 ms between the two clock reads of one call */
  fake_clock = UINT64_C(1000000000);

  real_wait_for_fences = fake_wait;
  vkWaitForFences_shim(NULL, 1, NULL, VK_TRUE, UINT64_MAX);
  CHECK(gpu_fence_calls == 1, "blocking wait counted %llu", gpu_fence_calls);
  CHECK(gpu_fence_ns == 3000000, "blocking wait recorded %llu ns", gpu_fence_ns);
  CHECK(gpu_fence_polls == 0, "blocking wait counted as poll");
  vkWaitForFences_shim(NULL, 1, NULL, VK_TRUE, 0);
  CHECK(gpu_fence_polls == 1, "poll counted %llu", gpu_fence_polls);
  CHECK(gpu_fence_calls == 1, "poll counted as blocking wait");
  CHECK(gpu_fence_ns == 3000000, "poll consumed clock reads: %llu ns", gpu_fence_ns);
  CHECK(fake_fence_calls == 2, "poll not forwarded to the driver (%d calls)", fake_fence_calls);

  real_queue_wait_idle = fake_idle;
  vkQueueWaitIdle_shim(NULL);
  CHECK(gpu_idle_calls == 1 && gpu_idle_ns == 3000000, "queue idle %llu x %llu ns",
        gpu_idle_calls, gpu_idle_ns);

  real_create_graphics_pipelines = fake_pipes;
  vkCreateGraphicsPipelines_shim(NULL, NULL, 1, NULL, NULL, NULL);
  CHECK(gpu_pipe_calls == 1, "single pipeline counted %llu", gpu_pipe_calls);
  vkCreateGraphicsPipelines_shim(NULL, NULL, 8, NULL, NULL, NULL);
  CHECK(fake_pipe_last_count == 8, "batch not forwarded (count %d)", fake_pipe_last_count);
  CHECK(gpu_pipe_calls == 9, "batch of 8 should total 9, got %llu", gpu_pipe_calls);
  CHECK(gpu_pipe_ns == 6000000, "pipeline time %llu ns", gpu_pipe_ns);

  real_cmd_pipeline_barrier = fake_barrier;
  vkCmdPipelineBarrier_shim(NULL, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
  vkCmdPipelineBarrier_shim(NULL, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
  CHECK(gpu_barrier_calls == 2 && fake_barrier_calls == 2, "barriers %llu counted / %d forwarded",
        gpu_barrier_calls, fake_barrier_calls);

  /* The per-frame hitch bookkeeping now shares this region. Its only real
   * caller (perf_observe_present) is outside the extraction, so call it once
   * here: the region must still compile as a whole with -Werror. The logic of
   * those deltas is what tools/check_vk_hitch_frame.py tests. */
  perf_note_frame(0);

  /* An empty window must not divide by zero in the percentages. */
  report_body(0.0);
  report_body(5.0);

  printf(fail ? "vk gpu probe harness: FAIL\n" : "vk gpu probe harness: OK\n");
  return fail;
}
"""


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--vk", default=os.path.join(root, "source", "hooks", "vk.c"))
    ap.add_argument("--vulkan-include", default=None,
                    help="directory holding vulkan/vulkan_core.h")
    ap.add_argument("--keep", action="store_true", help="keep the generated harness")
    args = ap.parse_args()

    src = open(args.vk, encoding="utf-8").read()
    for needle in (BEGIN, END, REPORT_BEGIN, REPORT_END):
        if src.count(needle) != 1:
            print("anchor %r occurs %d times in %s" % (needle, src.count(needle), args.vk))
            return 2
    block = src[src.index(BEGIN):src.index(END)]
    report = src[src.index(REPORT_BEGIN):src.index(REPORT_END)]

    types = []
    for rel, start, end in HDR_TYPES:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8").read()
        if text.count(start) != 1 or text.count(end) != 1:
            print("anchor %r not unique in %s" % (start[:40], rel))
            return 2
        types.append(text[text.index(start):text.index(end) + len(end)])

    inc = args.vulkan_include
    if not inc:
        for cand in (os.environ.get("NXVK_INCLUDE"), "/tmp/nxvk/include",
                     os.path.join(root, "third_party", "nxvk", "include")):
            if cand and os.path.exists(os.path.join(cand, "vulkan", "vulkan_core.h")):
                inc = cand
                break
    if not inc:
        print("vulkan/vulkan_core.h not found; extract nxvk-vulkan-tegra-x1.tar.gz "
              "and pass --vulkan-include <dir>")
        return 2

    tmp = tempfile.mkdtemp(prefix="vkgpu")
    c = os.path.join(tmp, "gpu_test.c")
    open(c, "w", encoding="utf-8").write(
        PREAMBLE.replace("__HDR_TYPES__", "\n".join(types))
        + block + EPILOGUE.replace("__REPORT__", report))
    exe = os.path.join(tmp, "gpu_test")
    cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Wformat=2", "-Werror",
           "-I" + inc, "-o", exe, c]
    print(" ".join(cmd))
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout)
        print(build.stderr)
        return 1
    if build.stderr.strip():
        print("compiler warnings are treated as failures:\n" + build.stderr)
        return 1
    run = subprocess.run([exe], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.stderr.strip():
        print(run.stderr.strip())
    if not args.keep:
        subprocess.run(["rm", "-rf", tmp])
    else:
        print("kept in " + tmp)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())
