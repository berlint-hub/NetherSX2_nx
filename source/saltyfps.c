/* saltyfps.c -- publish our frame pacing to the SaltySD shared block.
 *
 * Protocol, all of it read out of the sources rather than guessed:
 *
 *   SaltyNX 2.1.3 (masagrator/SaltyNX a3fee3f), saltysd_core/source/NX-FPS.cpp:
 *     SaltySD_CheckIfSharedMemoryAvailable(&offset, sizeof(NxFpsSharedBlock))
 *     Shared = shmemGetAddr(shm) + offset;  Shared->MAGIC = 0x465053;
 *     Shared->expectedSetBuffers = -1;
 *   and NX-FPS-Common.hpp PostFrame() for how FPS / FPSavg / frameNumber are
 *   derived, which is reproduced here step for step.
 *
 *   FPSLocker (source/Utils.hpp searchSharedMemoryBlock) scans the whole 4 KiB
 *   page from offset 0 in 4-byte steps and takes the FIRST block whose magic
 *   matches. So if the SaltyNX plugin already reserved a block at a lower
 *   offset than ours, writing to ours would be invisible to every overlay.
 *   We therefore adopt the lowest existing block and write there.
 *
 *   Adoption has a hazard: if the plugin is alive and counting, two writers
 *   fight over frameNumber and the readout turns to noise. PostFrame() is only
 *   reached from hooks installed via nn::ro::LookupSymbol, which cannot resolve
 *   in a homebrew NRO, so this should never trigger -- but "should" is not a
 *   measurement, so the block is polled and we stand down the moment someone
 *   else moves frameNumber.
 *
 * The IPC is the legacy SFCI shape that SaltySD still speaks. libnx 4.x dropped
 * ipcPrepareHeader/ipcParse, and pulling in FPSLocker's copy of that header
 * would mean vendoring 700 lines to send four commands, so the request header
 * is written by hand here. For a request with SendPid set and no buffers,
 * handles or statics the header is exactly three words plus two the kernel
 * fills with the PID, padded to a 16-byte boundary -- raw data at TLS+32.
 *
 * Fields we must NOT write, because an overlay owns them: FPSlocked,
 * FPSlockedDocked, SetBuffers, SetActiveBuffers, displaySync,
 * forceOriginalRefreshRate, dontForce60InDocked, forceSuspend. And
 * currentRefreshRate belongs to the SaltySD sysmodule, which is the only thing
 * on the console that can actually change the panel.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "saltyfps.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "corelog.h"
#include "prefs.h"
#include "util.h"   /* armGetTlsRw */

#define SFCI_MAGIC 0x49434653u
#define SFCO_MAGIC 0x4f434653u

#define IPC_CMD_CHECK_SHARED_MEMORY 6
#define IPC_CMD_GET_SHARED_MEMORY   7

/* CNTVCT on Horizon runs at 19.2 MHz; SaltyNX hardcodes that as
 * systemtickfrequency and divides it by the frame delta to get FPS. Read it
 * from libnx the way libc_shim.c does instead of assuming, so a future console
 * with a different counter rate still reports true FPS. */
static uint64_t saltyfps_tick_hz(void) {
  static uint64_t cached = 0;
  if (!cached) cached = armGetSystemTickFreq();
  return cached ? cached : 19200000u;
}

struct saltyfps_resolution_calls {
  uint16_t width;
  uint16_t height;
  uint16_t calls;
};

/* Byte-for-byte NX-FPS-Common.hpp. The struct is packed and therefore
 * deliberately misaligned: FPSavg sits at byte 5 and frameNumber at 165.
 * uint8_t stands in for C++ bool, which is one byte there too. */
struct saltyfps_block {
  uint32_t MAGIC;
  uint8_t  FPS;
  float    FPSavg;
  uint8_t  pluginActive;
  uint8_t  FPSlocked;
  uint8_t  FPSmode;
  uint8_t  ZeroSync;
  uint8_t  patchApplied;
  uint8_t  API;
  uint32_t FPSticks[10];
  uint8_t  Buffers;
  uint8_t  SetBuffers;
  uint8_t  ActiveBuffers;
  uint8_t  SetActiveBuffers;
  uint8_t  displaySync;
  struct saltyfps_resolution_calls renderCalls[8];
  struct saltyfps_resolution_calls viewportCalls[8];
  uint8_t  forceOriginalRefreshRate;
  uint8_t  dontForce60InDocked;
  uint8_t  forceSuspend;
  uint8_t  currentRefreshRate;
  float    readSpeedPerSecond;
  uint8_t  FPSlockedDocked;
  uint64_t frameNumber;
  int8_t   expectedSetBuffers;
  /* PerfCounters: 15 u64 + 4 u32 of NVN query results. Untouched on the Vulkan
   * path, but included so the block is the size SaltyNX reserves. */
  uint8_t  PerfCounters[136];
} __attribute__((packed));

_Static_assert(sizeof(struct saltyfps_block) == SALTYFPS_BLOCK_SIZE,
               "NxFpsSharedBlock drifted from SaltyNX 2.1.3 (310 bytes)");
_Static_assert(offsetof(struct saltyfps_block, MAGIC) == 0, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPS) == 4, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPSavg) == 5, "layout");
_Static_assert(offsetof(struct saltyfps_block, pluginActive) == 9, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPSlocked) == 10, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPSmode) == 11, "layout");
_Static_assert(offsetof(struct saltyfps_block, ZeroSync) == 12, "layout");
_Static_assert(offsetof(struct saltyfps_block, patchApplied) == 13, "layout");
_Static_assert(offsetof(struct saltyfps_block, API) == 14, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPSticks) == 15, "layout");
_Static_assert(offsetof(struct saltyfps_block, Buffers) == 55, "layout");
_Static_assert(offsetof(struct saltyfps_block, SetBuffers) == 56, "layout");
_Static_assert(offsetof(struct saltyfps_block, ActiveBuffers) == 57, "layout");
_Static_assert(offsetof(struct saltyfps_block, SetActiveBuffers) == 58, "layout");
_Static_assert(offsetof(struct saltyfps_block, displaySync) == 59, "layout");
_Static_assert(offsetof(struct saltyfps_block, renderCalls) == 60, "layout");
_Static_assert(offsetof(struct saltyfps_block, viewportCalls) == 108, "layout");
_Static_assert(offsetof(struct saltyfps_block, forceOriginalRefreshRate) == 156, "layout");
_Static_assert(offsetof(struct saltyfps_block, dontForce60InDocked) == 157, "layout");
_Static_assert(offsetof(struct saltyfps_block, forceSuspend) == 158, "layout");
_Static_assert(offsetof(struct saltyfps_block, currentRefreshRate) == 159, "layout");
_Static_assert(offsetof(struct saltyfps_block, readSpeedPerSecond) == 160, "layout");
_Static_assert(offsetof(struct saltyfps_block, FPSlockedDocked) == 164, "layout");
_Static_assert(offsetof(struct saltyfps_block, frameNumber) == 165, "layout");
_Static_assert(offsetof(struct saltyfps_block, expectedSetBuffers) == 173, "layout");
_Static_assert(offsetof(struct saltyfps_block, PerfCounters) == 174, "layout");
/* The pre-PerfCounters generation ended here, and berlint-hub/FPSLocker still
 * declares the block as 174 bytes. Everything up to this offset is common. */
_Static_assert(offsetof(struct saltyfps_block, expectedSetBuffers) + 1 == 174,
               "the 174-byte generation must stay a prefix of the 310-byte one");

enum {
  ST_INIT = 0,   /* not attempted yet */
  ST_READY,      /* publishing */
  ST_ABSENT,     /* no SaltySD port; stay quiet */
  ST_FOREIGN,    /* someone else owns the block; stay quiet */
  ST_GAVEUP      /* attach failed too often; stay quiet */
};

static int g_state = ST_INIT;
static int g_attempts;
static Result g_last_rc;      /* why the last attach attempt failed */

static Handle g_port = INVALID_HANDLE;
static SharedMemory g_shm;
static bool g_shm_mapped;

static uint8_t *g_base;              /* start of the 4 KiB page */
static const uint8_t *g_opmode;      /* byte 0: 1 when docked */
static struct saltyfps_block *g_block;
static ptrdiff_t g_block_offset;
static bool g_adopted;               /* took over a block we did not reserve */

/* PostFrame() state, mirroring NX_FPS_Math exactly. */
static uint64_t g_frame_end;         /* tick of the previous present */
static uint64_t g_start_tick;        /* start of the 1-second FPS window */
static uint64_t g_frame_avg;         /* EWMA of frame deltas, /10 */
static uint8_t  g_fps_temp;          /* presents since g_start_tick */
static uint8_t  g_tick_itr;          /* FPSticks ring cursor */
static uint64_t g_expected_frame;    /* frameNumber as we last left it */
static uint64_t g_present_count;
static uint8_t  g_pending_buffers;   /* swapchain image count, set before attach */

#define SALTYFPS_MAX_ATTEMPTS 64
#define SALTYFPS_FOREIGN_CHECK_MASK 127u

/* ------------------------------------------------------------------ IPC ---- */

/* Where raw command data starts, following ipcPrepareHeader's arithmetic:
 * three header words, two PID words the kernel fills, then pad to 16 bytes. */
static uint32_t *saltyfps_raw(void) {
  uint32_t *buf = (uint32_t *)armGetTlsRw();
  buf += 5;
  const uintptr_t padding = ((16u - (((uintptr_t)buf) & 15u)) & 15u) / 4u;
  return buf + padding;
}

/* Send one SFCI command with SendPid and no buffers. `arg` is the single u64
 * payload; `out_value` receives the third u64 of the reply. Returns the reply's
 * result code, or a nonzero transport failure. */
static Result saltyfps_cmd(uint64_t cmd_id, uint64_t arg, uint64_t *out_value,
                           Handle *out_handle) {
  uint32_t *tls = (uint32_t *)armGetTlsRw();
  tls[0] = 4u;          /* IpcCommandType_Request */
  tls[1] = 0x80000000u; /* the PID/handle section follows */
  tls[2] = 1u;          /* SendPid */
  /* tls[3] and tls[4] are the PID, written by the kernel on dispatch. */

  uint64_t *raw = (uint64_t *)saltyfps_raw();
  raw[0] = SFCI_MAGIC;
  raw[1] = cmd_id;
  raw[2] = arg;
  raw[3] = 0; /* reserved */

  const Result rc = svcSendSyncRequest(g_port);
  if (R_FAILED(rc)) return rc;

  /* Parse the reply the way ipcParse did. */
  uint32_t *buf = (uint32_t *)armGetTlsRw();
  const uint32_t ctrl1 = buf[1];
  buf += 2;
  if (ctrl1 & 0x80000000u) {
    const uint32_t ctrl2 = *buf++;
    if (ctrl2 & 1u) buf += 2; /* PID in the reply */
    const size_t ncopy = (ctrl2 >> 1) & 15u;
    const size_t nmove = (ctrl2 >> 5) & 15u;
    for (size_t i = 0; i < ncopy + nmove; i++) {
      const Handle h = (Handle)*buf++;
      if (i == 0 && out_handle) *out_handle = h;
    }
  }
  const uintptr_t padding = ((16u - (((uintptr_t)buf) & 15u)) & 15u) / 4u;
  const uint64_t *resp = (const uint64_t *)(buf + padding);
  if (resp[0] != SFCO_MAGIC) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  if (out_value) *out_value = resp[2];
  return (Result)resp[1];
}

/* --------------------------------------------------------------- attach ---- */

static void saltyfps_detach_locked(void) {
  if (g_shm_mapped) {
    shmemClose(&g_shm);
    g_shm_mapped = false;
  }
  g_base = NULL;
  g_block = NULL;
  g_opmode = NULL;
  if (g_port != INVALID_HANDLE) {
    svcCloseHandle(g_port);
    g_port = INVALID_HANDLE;
  }
}

static bool saltyfps_attach(void) {
  if (++g_attempts > SALTYFPS_MAX_ATTEMPTS) {
    g_state = ST_GAVEUP;
    return false;
  }

  Result rc = svcConnectToNamedPort(&g_port, "SaltySD");
  if (R_FAILED(rc)) {
    /* Three very different failures all land here, and guessing between them
     * costs a hardware round trip each time, so the code is logged:
     *   - the port does not exist      -> SaltyNX is not running at all
     *   - the port exists but is busy  -> saltysd_proc registers it with
     *     svcManageNamedPort(&saltyport, "SaltySD", 1), i.e. max_sessions = 1
     *     ("For some reason, we only have one session maximum" upstream), and
     *     saltysd_bootstrap.elf connects to it from inside the application
     *     process, so an injected bootstrap holds the only slot
     *   - access denied                -> a homebrew process may not reach it
     * The first is terminal, the second is worth retrying, the third is not
     * ours to fix. */
    g_port = INVALID_HANDLE;
    g_last_rc = rc;
    corelog_log(CORELOG_WARN, "Wrapper",
                "saltyfps: svcConnectToNamedPort(\"SaltySD\") failed rc=0x%x "
                "(attempt %d of %d)", rc, g_attempts, SALTYFPS_MAX_ATTEMPTS);
    /* rc=0x10801 is MAKERESULT(Module_Kernel, KernelError_OutOfResource=132),
     * and that one code covers two situations with opposite fixes: either the
     * port is not visible to this process at all, or it is visible but its
     * single session slot is held by somebody else -- saltysd_proc registers it
     * with svcManageNamedPort(..., "SaltySD", 1) and saltysd_bootstrap.elf holds
     * the handle for the whole time it spends injecting saltynx_core.elf.
     * Asking the kernel about a port that cannot exist separates the two: same
     * code back means "not visible", a different code means "visible but full".
     * Done once, on the first failure, and touches no real port. */
    if (g_attempts == 1) {
      Handle dummy = INVALID_HANDLE;
      const Result drc = svcConnectToNamedPort(&dummy, "NxNull");
      corelog_log(CORELOG_INFO, "Wrapper",
                  "saltyfps: calibration -- a port that cannot exist returns "
                  "rc=0x%x, SaltySD returns rc=0x%x => %s",
                  drc, rc,
                  drc == rc ? "SaltySD is NOT VISIBLE to this process"
                            : "SaltySD IS visible, its one session slot is TAKEN");
      if (R_SUCCEEDED(drc)) svcCloseHandle(dummy);
    }
    return false;
  }
  g_last_rc = 0;

  uint64_t offset = 0;
  rc = saltyfps_cmd(IPC_CMD_CHECK_SHARED_MEMORY, sizeof(struct saltyfps_block),
                    &offset, NULL);
  if (R_FAILED(rc) || offset + sizeof(struct saltyfps_block) > SALTYFPS_SHMEM_SIZE) {
    corelog_log(CORELOG_WARN, "Wrapper",
                "saltyfps: could not reserve a shared block (rc=0x%x offset=%llu)",
                rc, (unsigned long long)offset);
    saltyfps_detach_locked();
    g_state = ST_GAVEUP;
    return false;
  }

  Handle shm_handle = INVALID_HANDLE;
  rc = saltyfps_cmd(IPC_CMD_GET_SHARED_MEMORY, 0, NULL, &shm_handle);
  if (R_FAILED(rc) || shm_handle == INVALID_HANDLE) {
    corelog_log(CORELOG_WARN, "Wrapper",
                "saltyfps: SaltySD refused the shared memory handle (rc=0x%x)", rc);
    saltyfps_detach_locked();
    g_state = ST_GAVEUP;
    return false;
  }

  shmemLoadRemote(&g_shm, shm_handle, SALTYFPS_SHMEM_SIZE, Perm_Rw);
  if (R_FAILED(shmemMap(&g_shm))) {
    corelog_log(CORELOG_WARN, "Wrapper", "saltyfps: shmemMap failed");
    svcCloseHandle(shm_handle);
    saltyfps_detach_locked();
    g_state = ST_GAVEUP;
    return false;
  }
  g_shm_mapped = true;

  g_base = (uint8_t *)shmemGetAddr(&g_shm);
  g_opmode = g_base + 0; /* byte 1 is the refresh rate; both belong to SaltySD */
  g_block_offset = (ptrdiff_t)offset;

  /* FPSLocker takes the first magic in the page. If a block already sits below
   * the one we reserved, that is the one every overlay will read, so adopt it
   * instead of writing where nobody looks. */
  struct saltyfps_block *found = NULL;
  for (ptrdiff_t scan = 0; scan < g_block_offset; scan += 4) {
    uint32_t magic = 0;
    memcpy(&magic, g_base + scan, sizeof(magic));
    if (magic == SALTYFPS_MAGIC) {
      found = (struct saltyfps_block *)(g_base + scan);
      g_block_offset = scan;
      g_adopted = true;
      break;
    }
  }
  g_block = found ? found : (struct saltyfps_block *)(g_base + g_block_offset);

  /* Zero means "not seeded yet": attach can run from main() long before the
   * first present, and crediting that whole gap to one frame would report a
   * several-second frame and drive FPSavg to zero. The first present seeds the
   * baselines and returns -- the same bug perf_observe_present() had. */
  g_frame_end = 0;
  g_start_tick = 0;
  g_frame_avg = 0;
  g_fps_temp = 0;
  g_tick_itr = 0;
  g_present_count = 0;

  if (!g_adopted) {
    memset(g_block, 0, sizeof(*g_block));
    g_block->MAGIC = SALTYFPS_MAGIC;
    g_block->expectedSetBuffers = -1;
    g_block->API = SALTYFPS_API_VULKAN;
  } else {
    /* Keep whatever the previous owner left in the overlay-owned fields, and
     * only claim the ones that describe us. */
    g_block->API = SALTYFPS_API_VULKAN;
    if (g_block->expectedSetBuffers == 0) g_block->expectedSetBuffers = -1;
  }

  if (g_pending_buffers) {
    g_block->Buffers = g_pending_buffers;
    g_block->ActiveBuffers = g_pending_buffers;
  }

  g_expected_frame = g_block->frameNumber;
  g_block->pluginActive = 1;
  g_state = ST_READY;

  corelog_log(CORELOG_INFO, "Wrapper",
              "saltyfps: publishing at offset %lld (adopted=%d) api=%u size=%u",
              (long long)g_block_offset, g_adopted ? 1 : 0,
              (unsigned)g_block->API, (unsigned)sizeof(*g_block));
  return true;
}

/* ---------------------------------------------------------------- public ---- */

bool saltyfps_init(void) {
  if (!prefs_get_bool("Wrapper/SaltyFPS", false)) {
    g_state = ST_ABSENT; /* opted out; the retry path checks the same flag */
    return false;
  }
  if (g_state == ST_READY) return true;
  if (g_state == ST_ABSENT || g_state == ST_FOREIGN || g_state == ST_GAVEUP)
    return false;
  return saltyfps_attach();
}

void saltyfps_present(void) {
  if (g_state != ST_READY) {
    if (g_state != ST_INIT) return;
    /* The sysmodule may come up after we do. Retrying on a present costs one
     * failed svcConnectToNamedPort, so space the attempts out. */
    if ((g_present_count++ & 31u) != 0u) return;
    if (!prefs_get_bool("Wrapper/SaltyFPS", false)) { g_state = ST_ABSENT; return; }
    if (!saltyfps_attach()) return;
  }

  const uint64_t endtick = armGetSystemTick();

  if (!g_frame_end) {
    /* First present after attach: no interval to report yet. */
    g_frame_end = endtick;
    g_start_tick = endtick;
    ++g_present_count;
    return;
  }

  /* Someone else moving frameNumber means the SaltyNX plugin found a hook after
  * all. Two writers produce garbage, and their numbers come from a real present
   * hook while ours come from a shim, so we yield. */
  if ((g_present_count & SALTYFPS_FOREIGN_CHECK_MASK) == 0u) {
    if (g_block->frameNumber != g_expected_frame) {
      corelog_log(CORELOG_INFO, "Wrapper",
                  "saltyfps: another writer owns the block (frameNumber %llu, "
                  "ours %llu); standing down",
                  (unsigned long long)g_block->frameNumber,
                  (unsigned long long)g_expected_frame);
      g_state = ST_FOREIGN;
      return;
    }
  }
  ++g_present_count;

  /* PostFrame(), step for step. */
  g_block->frameNumber = g_expected_frame + 1;
  g_expected_frame = g_block->frameNumber;

  const uint64_t framedelta = endtick - g_frame_end;
  g_block->FPSticks[g_tick_itr++] = (uint32_t)framedelta;
  if (g_tick_itr >= 10) g_tick_itr = 0;

  /* SaltyNX starts frameavg at 0, so its first reading is 10x too fast and
   * converges over ten frames. Seeding with the first real delta skips that
   * without changing the steady state. */
  g_frame_avg = g_frame_avg ? ((9u * g_frame_avg) + framedelta) / 10u : framedelta;
  const float fpsavg = (g_frame_avg > 0)
      ? (float)saltyfps_tick_hz() / (float)g_frame_avg
      : 0.0f;

  g_frame_end = endtick;
  g_fps_temp++;

  const uint64_t deltatick = endtick - g_start_tick;
  const uint64_t tick_hz = saltyfps_tick_hz();
  if (deltatick > tick_hz) {
    g_block->FPS = (uint8_t)(g_fps_temp - 1);
    if (deltatick > tick_hz * 2u) {
      g_start_tick = endtick;
      g_fps_temp = 0;
    } else {
      g_start_tick += tick_hz;
      g_fps_temp = 1;
    }
  }

  g_block->FPSavg = fpsavg;
  g_block->pluginActive = 1;
}

void saltyfps_set_buffers(int count) {
  /* The swapchain is created long before the first present, and attach can
   * still be pending at that point, so remember the count and publish it from
   * whichever side gets there second. */
  if (count <= 0 || count > 4) return;
  g_pending_buffers = (uint8_t)count;
  if (g_state != ST_READY) return;
  g_block->Buffers = g_pending_buffers;
  g_block->ActiveBuffers = g_pending_buffers;
}

int saltyfps_target_fps(void) {
  if (g_state != ST_READY && g_state != ST_FOREIGN) return 0;
  const int docked = (g_opmode && *g_opmode == 1);
  int lock = docked ? (int)g_block->FPSlockedDocked : (int)g_block->FPSlocked;
  if (lock <= 0) return 0;
  if (lock > 120) lock = 120;
  return lock;
}

int saltyfps_refresh_rate(void) {
  if (g_state != ST_READY && g_state != ST_FOREIGN) return 0;
  const int rr = (int)g_block->currentRefreshRate;
  /* currentRefreshRate belongs to the SaltySD sysmodule. Until Display Sync or a
   * manual change sets it, the field can legitimately be 0, and clamping a target
   * to 0 would disable generation outright -- so anything implausible reads as
   * "unknown" and leaves the target alone. */
  /* 40 Hz is the lowest rate SaltySD will drive an LCD panel at and 120 Hz the
   * docked ceiling, so anything outside that is a field nobody has written yet. */
  return (rr >= 40 && rr <= 120) ? rr : 0;
}

bool saltyfps_active(void) {
  return g_state == ST_READY;
}

void saltyfps_status(char *out, size_t out_size) {
  if (!out || !out_size) return;
  const char *state =
      g_state == ST_READY   ? "ready"    :
      g_state == ST_ABSENT  ? "opted-out" :
      g_state == ST_FOREIGN ? "foreign-writer" :
      g_state == ST_GAVEUP  ? "unavailable" : "init";
  if (g_state != ST_READY && g_state != ST_FOREIGN) {
    snprintf(out, out_size, "state=%s attempts=%d/%d rc=0x%x", state,
             g_attempts, SALTYFPS_MAX_ATTEMPTS, (unsigned)g_last_rc);
    return;
  }
  snprintf(out, out_size,
           "state=%s offset=%lld adopted=%d api=%u fps=%u avg=%.1f lock=%u/%u "
           "rr=%u (usable=%d) buffers=%u/%u frames=%llu",
           state, (long long)g_block_offset, g_adopted ? 1 : 0,
           (unsigned)g_block->API, (unsigned)g_block->FPS, (double)g_block->FPSavg,
           (unsigned)g_block->FPSlocked, (unsigned)g_block->FPSlockedDocked,
           (unsigned)g_block->currentRefreshRate, saltyfps_refresh_rate(),
           (unsigned)g_block->Buffers, (unsigned)g_block->ActiveBuffers,
           (unsigned long long)g_block->frameNumber);
}
