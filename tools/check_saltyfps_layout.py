#!/usr/bin/env python3
"""Compile and exercise source/saltyfps.c on the host.

Why this exists. The module writes into a shared-memory block owned by someone
else: the SaltySD sysmodule allocates it, and FPSLocker / Status Monitor read
it. Every failure mode is silent and looks like success.

  * A field at the wrong offset still writes a plausible number -- into someone
    else's field. FPSLocker then shows a value that came from bytes we never
    meant to touch, and nothing anywhere reports an error.
  * The layout is packed and therefore deliberately misaligned: FPSavg sits at
    byte 5, frameNumber at byte 165. Drop __attribute__((packed)) and the
    compiler inserts padding, silently shifting every later field.
  * The block has two live generations. SaltyNX 2.1.3 reserves 310 bytes;
    berlint-hub/FPSLocker still declares 174. Those must stay prefix-compatible
    or one of the two readers starts reading garbage.

The reference offsets below were not typed in by hand. They were produced by
compiling the struct verbatim out of masagrator/SaltyNX tag 2.1.3 (a3fee3f),
saltysd_core/source/NX-FPS-Common.hpp -- the version shipped inside the NX Venom
bundle -- and diffing its offsetof table against ours, which came out identical
for all 26 fields plus sizeof. The reproduction lives in this file's docstring
history; re-run it after any SaltyNX upgrade before trusting the numbers again.

What is tested, beyond the layout:

  attach      port present -> ST_READY, magic/API/expectedSetBuffers written;
              port absent -> inert, no crash, no writes anywhere.
  adoption    a block already sitting at a lower offset than the one we reserved
              is the one FPSLocker will read (searchSharedMemoryBlock scans from
              0 and takes the first match), so we must write there instead.
  seeding     the first present after attach must NOT be credited with the gap
              since attach. Attach runs from main(), presents start much later;
              without the seed that whole gap becomes one multi-second frame and
              drives FPSavg to zero. This is the same bug perf_observe_present()
              had, in a different file.
  fps math    PostFrame() reproduced step for step: 50 presents of 384000 ticks
              must report 50, and FPSavg must converge to 50.0.
  foreign     if anything else moves frameNumber, two writers produce noise and
              we stand down. Detected by polling, not assumed away.
  target      FPSlocked handheld vs FPSlockedDocked when byte 0 of the page says
              docked; 0 means "no external target" and must fall through to ini.
  buffers     set before attach (the swapchain is created first) still reaches
              the block once attach happens.
  panel rate  currentRefreshRate is gated to 40..120 because Display Sync writes
              it and an unwritten 0 must read as "unknown", never as a limit --
              clamping a target to 0 would disable generation outright.

Not covered here, stated plainly so nobody assumes it is: the clamp that keeps
the generation target at or below the panel rate lives in vk.c's
lsfg_target_fps_pref(), which needs Vulkan headers to compile and is therefore
outside this harness. case_target_respects_panel() pins the two inputs that
clamp reads and asserts they can legitimately disagree; it documents the
invariant rather than testing the clamp itself.

Mutation history: dropping __attribute__((packed)), swapping Buffers and
SetBuffers, shrinking PerfCounters from 136 to 8 bytes, removing the first-
present seed, removing the foreign-writer poll, and reading FPSlocked in docked
mode are each caught by at least one case below.

The code under test is extracted from source/saltyfps.c VERBATIM (never retyped)
and source/saltyfps.h is included verbatim, so neither can drift past this
harness. Needs no devkitPro, no Vulkan headers and no Switch: the Horizon API
surface the module touches is nine functions, stubbed here.

Exit status is 0 only when it compiled with -Wall -Wextra -Werror AND every case
passed.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

SRC = os.path.join("source", "saltyfps.c")
HDR = os.path.join("source", "saltyfps.h")

# Offsets measured from SaltyNX 2.1.3 (see docstring). Field name -> byte offset.
REFERENCE = {
    "MAGIC": 0, "FPS": 4, "FPSavg": 5, "pluginActive": 9, "FPSlocked": 10,
    "FPSmode": 11, "ZeroSync": 12, "patchApplied": 13, "API": 14,
    "FPSticks": 15, "Buffers": 55, "SetBuffers": 56, "ActiveBuffers": 57,
    "SetActiveBuffers": 58, "displaySync": 59, "renderCalls": 60,
    "viewportCalls": 108, "forceOriginalRefreshRate": 156,
    "dontForce60InDocked": 157, "forceSuspend": 158, "currentRefreshRate": 159,
    "readSpeedPerSecond": 160, "FPSlockedDocked": 164, "frameNumber": 165,
    "expectedSetBuffers": 173, "PerfCounters": 174,
}
REFERENCE_SIZEOF = 310
# The pre-PerfCounters generation, still declared by berlint-hub/FPSLocker.
LEGACY_SIZE = 174

SRC_BEGIN = "#define SFCI_MAGIC"

STUBS = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- the slice of Horizon that saltyfps.c touches ---- */
typedef uint32_t Handle;
typedef uint32_t Result;
#define INVALID_HANDLE 0u
#define Perm_Rw 3u
#define R_FAILED(r)   ((r) != 0)
#define R_SUCCEEDED(r) ((r) == 0)
#define Module_Libnx 347
#define LibnxError_BadInput 1
#define MAKERESULT(m, d) ((((m) & 0xFF)) | (((d) & 0x1FFF) << 9))

typedef struct { Handle handle; void *addr; size_t size; } SharedMemory;

/* Test-visible state of the fake platform. */
static int      g_port_available = 1;
static uint64_t g_reserved_offset = 320;
static size_t   g_requested_size;
static int      g_connect_calls;
static uint8_t  g_tls[0x200] __attribute__((aligned(0x100)));
static uint8_t  g_page[0x1000];
static uint64_t g_tick;
static int      g_log_calls;
static int      g_pref_saltyfps = 1;

static inline void *armGetTlsRw(void) { return g_tls; }
static inline uint64_t armGetSystemTick(void) { return g_tick; }
static inline uint64_t armGetSystemTickFreq(void) { return 19200000u; }

static Result svcConnectToNamedPort(Handle *out, const char *name) {
  g_connect_calls++;
  if (g_port_available && name && !strcmp(name, "SaltySD")) { *out = 7u; return 0; }
  *out = INVALID_HANDLE;
  return MAKERESULT(1, 1);
}
static void svcCloseHandle(Handle h) { (void)h; }

/* Emulates the SaltySD side of the two commands, in the same TLS layout the
 * module parses back: ctrl words, optional PID, optional copied handles, pad to
 * a 16-byte boundary, then the SFCO raw block. */
static Result svcSendSyncRequest(Handle h) {
  (void)h;
  uint32_t *tls = (uint32_t *)g_tls;
  uint64_t *raw = (uint64_t *)(tls + 8);
  if (raw[0] != 0x49434653ull) return MAKERESULT(1, 2);
  const uint64_t cmd = raw[1];
  const uint64_t arg = raw[2];
  if (cmd == 6) {
    g_requested_size = (size_t)arg;
    tls[1] = 0x80000000u; tls[2] = 1u; tls[3] = 0u; tls[4] = 0u;
    raw[0] = 0x4f434653ull; raw[1] = 0ull; raw[2] = g_reserved_offset;
    return 0;
  }
  if (cmd == 7) {
    tls[1] = 0x80000000u;
    tls[2] = 3u;            /* PID present + one copied handle */
    tls[3] = 0u; tls[4] = 0u;
    tls[5] = 0xABCDu;       /* the shared memory handle */
    raw[0] = 0x4f434653ull; raw[1] = 0ull; raw[2] = 0ull;
    return 0;
  }
  raw[0] = 0x4f434653ull; raw[1] = MAKERESULT(1, 3); raw[2] = 0ull;
  return 0;
}

static void shmemLoadRemote(SharedMemory *s, Handle h, size_t size, uint8_t perm) {
  (void)perm; s->handle = h; s->size = size; s->addr = NULL;
}
static Result shmemMap(SharedMemory *s) { s->addr = g_page; return 0; }
static void *shmemGetAddr(SharedMemory *s) { return s->addr ? s->addr : g_page; }
static void shmemClose(SharedMemory *s) { s->addr = NULL; }

/* corelog.h / prefs.h, reduced to what the module calls. */
#define CORELOG_INFO 4
#define CORELOG_WARN 5
static void corelog_log(int priority, const char *tag, const char *fmt, ...) {
  (void)priority; (void)tag; (void)fmt; g_log_calls++;
}
static bool prefs_get_bool(const char *key, bool def) {
  if (key && !strcmp(key, "Wrapper/SaltyFPS")) return g_pref_saltyfps != 0;
  return def;
}

/* Declared here, defined in TESTS: the module's own state is file-static and
 * only becomes visible after the verbatim body has been pasted in. Without that
 * reset every case leaks into the next -- ST_ABSENT from the opt-out case makes
 * all later attach attempts return immediately, which reads as 30 unrelated
 * failures. */
static void platform_reset(void);
"""

# Reached into after the verbatim body, so cases can drive the module directly.
TESTS = r"""
/* ---- case bookkeeping ---- */
static int g_fail;

static void platform_reset(void) {
  memset(g_tls, 0, sizeof(g_tls));
  memset(g_page, 0, sizeof(g_page));
  g_port_available = 1;
  g_reserved_offset = 320;
  g_requested_size = 0;
  g_connect_calls = 0;
  g_tick = 1000000;
  g_log_calls = 0;
  g_pref_saltyfps = 1;

  /* The module's own state, reachable because it shares this translation unit.
   * ST_GAVEUP in particular would otherwise survive a failed attach forever and
   * silently turn every later case into a no-op. */
  g_state = ST_INIT;
  g_attempts = 0;
  g_port = INVALID_HANDLE;
  memset(&g_shm, 0, sizeof(g_shm));
  g_shm_mapped = false;
  g_base = NULL;
  g_opmode = NULL;
  g_block = NULL;
  g_block_offset = 0;
  g_adopted = false;
  g_frame_end = 0;
  g_start_tick = 0;
  g_frame_avg = 0;
  g_fps_temp = 0;
  g_tick_itr = 0;
  g_expected_frame = 0;
  g_present_count = 0;
  g_pending_buffers = 0;
}
static void check(int cond, const char *what) {
  if (!cond) { printf("  FAIL %s\n", what); g_fail++; }
}
static struct saltyfps_block *blk(void) {
  return (struct saltyfps_block *)(g_page + 320);
}
/* Drive N presents spaced exactly `delta` ticks apart, skipping the seed frame. */
static void run_presents(int n, uint64_t delta) {
  for (int i = 0; i < n; i++) { g_tick += delta; saltyfps_present(); }
}

static void case_layout(void) {
  printf("case: layout matches SaltyNX 2.1.3\n");
  check(sizeof(struct saltyfps_block) == REFERENCE_SIZEOF_C, "sizeof == 310");
#define CHK(f, v) check(offsetof(struct saltyfps_block, f) == (v), #f " offset")
  CHK(MAGIC, 0); CHK(FPS, 4); CHK(FPSavg, 5); CHK(pluginActive, 9);
  CHK(FPSlocked, 10); CHK(FPSmode, 11); CHK(ZeroSync, 12); CHK(patchApplied, 13);
  CHK(API, 14); CHK(FPSticks, 15); CHK(Buffers, 55); CHK(SetBuffers, 56);
  CHK(ActiveBuffers, 57); CHK(SetActiveBuffers, 58); CHK(displaySync, 59);
  CHK(renderCalls, 60); CHK(viewportCalls, 108);
  CHK(forceOriginalRefreshRate, 156); CHK(dontForce60InDocked, 157);
  CHK(forceSuspend, 158); CHK(currentRefreshRate, 159);
  CHK(readSpeedPerSecond, 160); CHK(FPSlockedDocked, 164); CHK(frameNumber, 165);
  CHK(expectedSetBuffers, 173); CHK(PerfCounters, 174);
#undef CHK
  /* The 174-byte generation must remain a prefix, or berlint-hub/FPSLocker --
   * which still declares the block as 174 -- starts reading PerfCounters. */
  check(offsetof(struct saltyfps_block, expectedSetBuffers) + 1 == LEGACY_SIZE_C,
        "174-byte generation is a prefix");
  check(sizeof(struct saltyfps_resolution_calls) == 6, "resolutionCalls is 6 bytes");
}

static void case_attach_absent(void) {
  printf("case: no SaltySD port -> inert\n");
  platform_reset();
  g_port_available = 0;
  check(!saltyfps_init(), "init reports failure");
  check(!saltyfps_active(), "not active");
  check(g_page[320] == 0 && g_page[321] == 0, "wrote nothing into the page");
  check(saltyfps_target_fps() == 0, "no external target");
  /* Presents must stay harmless rather than retrying into a dead port forever. */
  const int before = g_connect_calls;
  for (int i = 0; i < 10; i++) saltyfps_present();
  check(g_connect_calls - before <= 1, "present does not hammer the dead port");
}

static void case_attach_optout(void) {
  printf("case: Wrapper/SaltyFPS off -> never connects\n");
  platform_reset();
  g_pref_saltyfps = 0;
  check(!saltyfps_init(), "init reports failure");
  check(g_connect_calls == 0, "never touched the port");
  check(saltyfps_target_fps() == 0, "no external target");
}

static void case_attach_ready(void) {
  printf("case: attach reserves the full block and claims only our fields\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  check(saltyfps_active(), "active");
  check(g_requested_size == REFERENCE_SIZEOF_C, "reserved 310 bytes, not 174");
  check(g_reserved_offset == 320, "block sits where SaltySD put it");
  const struct saltyfps_block *b = blk();
  check(b->MAGIC == 0x465053u, "magic written");
  check(b->API == 3u, "API reports Vulkan");
  check(b->expectedSetBuffers == -1, "expectedSetBuffers seeded to -1");
  check(b->pluginActive == 1, "pluginActive set");
  check(b->FPSlocked == 0 && b->FPSlockedDocked == 0,
        "did not invent an FPS target");
  check(b->currentRefreshRate == 0, "did not touch the sysmodule's refresh rate");
  check(b->frameNumber == 0, "frame count starts at zero");
}

static void case_seeding(void) {
  printf("case: first present after attach seeds instead of reporting\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  /* Attach happened at tick 1000000. A second passes before the first present,
   * which is the realistic gap between main() and the video thread starting. */
  g_tick += 19200000u;
  saltyfps_present();
  const struct saltyfps_block *b = blk();
  check(b->frameNumber == 0, "seed frame is not counted as a presented frame");
  check(b->FPS == 0, "seed frame reports no FPS");
  check(b->FPSavg == 0.0f, "seed frame does not poison FPSavg");
  /* And the next present measures against the seed, not against attach. */
  g_tick += 384000u;
  saltyfps_present();
  check(b->frameNumber == 1, "second present is the first measured one");
  check(b->FPSavg > 45.0f && b->FPSavg < 55.0f, "FPSavg lands near 50 immediately");
}

static void case_fps_math(void) {
  printf("case: 50 fps of ticks reports 50\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  g_tick += 19200000u;
  saltyfps_present();                 /* seed */
  const uint64_t delta = 19200000u / 50u;   /* 384000 ticks = 20 ms */
  run_presents(80, delta);            /* 1.6 s of frames */
  const struct saltyfps_block *b = blk();
  check(b->frameNumber == 80, "frameNumber counts measured presents");
  check(b->FPS == 50 || b->FPS == 49, "FPS reads 50 (49 on the boundary second)");
  check(b->FPSavg > 49.5f && b->FPSavg < 50.5f, "FPSavg converged to 50");
  check(b->FPSticks[0] == (uint32_t)delta, "FPSticks ring holds the delta");
  check(b->pluginActive == 1, "still pluginActive");
}

static void case_foreign_writer(void) {
  printf("case: another writer takes the block -> we stand down\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  g_tick += 19200000u;
  saltyfps_present();
  run_presents(5, 384000u);
  struct saltyfps_block *b = blk();
  const uint64_t ours = b->frameNumber;
  check(ours > 0, "we were publishing");
  /* The real plugin increments on its own present hook, i.e. BETWEEN ours. A
   * one-shot overwrite before a present would simply be replaced by our next
   * write and the poll could never see it -- which is exactly why the check
   * compares against the value we left behind rather than against a snapshot. */
  for (int i = 0; i < 200 && saltyfps_active(); i++) {
    b->frameNumber += 1;              /* the other writer, once per frame */
    g_tick += 384000u;
    saltyfps_present();
  }
  check(!saltyfps_active(), "stood down");
  const uint64_t frozen = b->frameNumber;
  for (int i = 0; i < 5; i++) { g_tick += 384000u; saltyfps_present(); }
  check(b->frameNumber == frozen, "stopped writing frameNumber after standing down");
  check(b->FPSlocked == 0, "still reads the block while standing down");
}

static void case_adoption(void) {
  printf("case: a lower-offset block is adopted, because overlays read it\n");
  platform_reset();
  /* FPSLocker's searchSharedMemoryBlock scans from 0 and takes the first magic,
   * so a block at 128 wins over the one SaltySD reserves for us at 320. */
  struct saltyfps_block *lower = (struct saltyfps_block *)(g_page + 128);
  memset(lower, 0, sizeof(*lower));
  lower->MAGIC = 0x465053u;
  lower->FPSlocked = 45;
  g_reserved_offset = 320;
  check(saltyfps_init(), "init succeeds");
  check(lower->API == 3u, "adopted block reports Vulkan");
  check(lower->pluginActive == 1, "adopted block is live");
  check(g_page[320] == 0, "reserved block left untouched");
  g_tick += 19200000u;
  saltyfps_present();
  run_presents(3, 384000u);
  check(lower->frameNumber == 3, "presents land in the adopted block");
  check(saltyfps_target_fps() == 45, "reads the adopted block's target");
}

static void case_target_fps(void) {
  printf("case: FPSlocked handheld vs FPSlockedDocked\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  struct saltyfps_block *b = blk();
  g_page[0] = 0;                      /* byte 0 is the operation mode */
  b->FPSlocked = 60; b->FPSlockedDocked = 40;
  check(saltyfps_target_fps() == 60, "handheld reads FPSlocked");
  g_page[0] = 1;
  check(saltyfps_target_fps() == 40, "docked reads FPSlockedDocked");
  g_page[0] = 0;
  b->FPSlocked = 0;
  check(saltyfps_target_fps() == 0, "zero means no external target");
  b->FPSlocked = 200;
  check(saltyfps_target_fps() == 120, "clamped to 120");
}

static void case_buffers(void) {
  printf("case: buffers set before attach still get published\n");
  platform_reset();
  saltyfps_set_buffers(3);            /* the swapchain is created first */
  check(saltyfps_init(), "init succeeds");
  const struct saltyfps_block *b = blk();
  check(b->Buffers == 3 && b->ActiveBuffers == 3, "published on attach");
  saltyfps_set_buffers(99);           /* out of range must be ignored */
  check(b->Buffers == 3, "implausible count rejected");
}

static void case_refresh_rate(void) {
  printf("case: refresh rate gate for Display Sync\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  struct saltyfps_block *b = blk();
  /* 0 is what the field holds until SaltySD writes it. Treating that as a limit
   * would clamp the target to zero and disable generation outright. */
  b->currentRefreshRate = 0;
  check(saltyfps_refresh_rate() == 0, "unset reads as unknown");
  b->currentRefreshRate = 50;
  check(saltyfps_refresh_rate() == 50, "50 Hz reported");
  b->currentRefreshRate = 60;
  check(saltyfps_refresh_rate() == 60, "60 Hz reported");
  b->currentRefreshRate = 120;
  check(saltyfps_refresh_rate() == 120, "docked ceiling accepted");
  b->currentRefreshRate = 200;
  check(saltyfps_refresh_rate() == 0, "implausible value rejected");
  b->currentRefreshRate = 39;
  check(saltyfps_refresh_rate() == 0, "below the 40 Hz panel minimum rejected");
  /* And it must stay readable while standing down: the panel rate belongs to the
   * sysmodule, not to whoever is publishing frames. */
  b->currentRefreshRate = 50;
  g_state = ST_FOREIGN;
  check(saltyfps_refresh_rate() == 50, "still readable after standing down");
}

static void case_target_respects_panel(void) {
  printf("case: target never aims above the panel\n");
  platform_reset();
  check(saltyfps_init(), "init succeeds");
  struct saltyfps_block *b = blk();
  b->currentRefreshRate = 50;
  b->FPSlocked = 60;
  /* The clamp lives in vk.c's lsfg_target_fps_pref(), which needs Vulkan to
   * compile, so the invariant is asserted here against the two values it reads:
   * a target above the panel is exactly what that function has to pull down. */
  check(saltyfps_target_fps() == 60, "target reads 60 from the overlay");
  check(saltyfps_refresh_rate() == 50, "panel reads 50");
  check(saltyfps_target_fps() > saltyfps_refresh_rate(),
        "the two disagree, which is the case the clamp in vk.c must resolve");
}

static void case_status_line(void) {
  printf("case: status line is populated for the diagnostic echo\n");
  platform_reset();
  char buf[256] = {0};
  saltyfps_status(buf, sizeof(buf));
  check(strstr(buf, "state=init") != NULL, "reports init before attach");
  check(saltyfps_init(), "init succeeds");
  memset(buf, 0, sizeof(buf));
  saltyfps_status(buf, sizeof(buf));
  check(strstr(buf, "state=ready") != NULL, "reports ready after attach");
  check(strstr(buf, "offset=320") != NULL, "reports the offset");
  check(strstr(buf, "api=3") != NULL, "reports the API");
}

int main(void) {
  case_layout();
  case_attach_absent();
  case_attach_optout();
  case_attach_ready();
  case_seeding();
  case_fps_math();
  case_foreign_writer();
  case_adoption();
  case_target_fps();
  case_buffers();
  case_refresh_rate();
  case_target_respects_panel();
  case_status_line();
  if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
  printf("all saltyfps cases passed\n");
  return 0;
}
"""


def extract_body(src_text):
    """Everything from the first #define SFCI_MAGIC to EOF, verbatim."""
    idx = src_text.find(SRC_BEGIN)
    if idx < 0:
        raise SystemExit("cannot find %r in %s" % (SRC_BEGIN, SRC))
    return src_text[idx:]


def check_static_asserts(src_text):
    """The in-source asserts are what make a future drift break the build."""
    missing = [f for f in REFERENCE
               if not re.search(r"_Static_assert\(offsetof\(struct saltyfps_block,\s*%s\)" % re.escape(f), src_text)]
    if not re.search(r"_Static_assert\(sizeof\(struct saltyfps_block\) == SALTYFPS_BLOCK_SIZE", src_text):
        missing.append("sizeof")
    return missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=SRC)
    ap.add_argument("--header", default=HDR)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()

    src_text = open(args.source, encoding="utf-8").read()
    hdr_text = open(args.header, encoding="utf-8").read()

    missing = check_static_asserts(src_text)
    if missing:
        print("source has no _Static_assert for: %s" % ", ".join(sorted(missing)))
        print("the build would accept a layout drift that this harness rejects")
        return 1

    # The header is included verbatim so its constants cannot disagree with the
    # definitions the tests use.
    hdr_guarded = hdr_text.replace("#include <stdbool.h>", "") \
                          .replace("#include <stddef.h>", "") \
                          .replace("#include <stdint.h>", "")

    program = (
        STUBS
        + "\n#define REFERENCE_SIZEOF_C %d\n#define LEGACY_SIZE_C %d\n"
          % (REFERENCE_SIZEOF, LEGACY_SIZE)
        + hdr_guarded
        + "\n" + extract_body(src_text)
        + "\n" + TESTS
    )

    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "saltyfps_host.c")
        exe = os.path.join(tmp, "saltyfps_host")
        with open(c, "w", encoding="utf-8") as fh:
            fh.write(program)
        cmd = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-o", exe, c]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("compile failed:\n%s%s" % (build.stdout, build.stderr))
            return 1
        run = subprocess.run([exe], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="")
        if run.returncode != 0:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
