/* pthr.c -- bionic-to-newlib pthread wrappers
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>
#include <switch.h>

#include "pthr.h"
#include "util.h"
#include "prefs.h"
#include "corelog.h"

// set in the trampoline when the game's renderThread_main starts; the GL
// ownership layer lets this thread keep the real context across parks, so its
// wait shims below must service handover in short slices instead of blocking
static __thread int tls_is_render_thread = 0;

int pthr_is_render_thread(void) {
  return tls_is_render_thread;
}

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// ---------------------------------------------------------------------------
// fake RW TLS: AArch64 -fstack-protector loads the canary relative to
// TPIDR_EL0, which libnx leaves pointing at read-only/zero storage. Each game
// thread gets a small zeroed block installed in TPIDR_EL0 so those loads (and
// any other bionic TLS-relative reads) land on valid, writable memory. The
// block is intentionally leaked: it must stay live for the thread's lifetime.
// ---------------------------------------------------------------------------

void pthr_install_fake_tls(void) {
  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);
}

void pthr_ensure_fake_tls(void) {
  if (armGetTlsRw() == NULL)
    pthr_install_fake_tls();
}

// ---------------------------------------------------------------------------
// lazy first-use initialization of game-owned sync objects: the hot path is
// one acquire-load of the magic word; only init and destroy take init_lock
// ---------------------------------------------------------------------------

#define PTHR_MUTEX_MAGIC 0x4D58544Du // "MTXM"
#define PTHR_COND_MAGIC  0x444E434Du // "CNDM"

static Mutex init_lock;

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_static_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
  // pairs with the release store below so real_ptr is visible once magic is
  if (__atomic_load_n(&mutex->magic, __ATOMIC_ACQUIRE) == PTHR_MUTEX_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) == PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock); // another thread won the first-use race
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else {
    // the kind word of a statically initialized bionic mutex (overlaps the
    // low half of real_ptr, which we haven't written yet)
    switch (*(int *)mutex) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));

  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    mutex->real_ptr = real;
    __atomic_store_n(&mutex->magic, PTHR_MUTEX_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);

  }
  mutexUnlock(&init_lock);
  return ret;
}

static int cond_static_init(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) == PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }

  pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
  int ret = pthread_cond_init(real, attr);

  if (ret == 0) {
    cond->real_ptr = real;
    __atomic_store_n(&cond->magic, PTHR_COND_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);

  }
  mutexUnlock(&init_lock);
  return ret;
}

// Keep the EE isolated and distribute emulator workers across remaining cores.

static Mutex core_lock;
static int      core_count = 0;
static int      ee_core    = 0;     // dedicated to the EE/VM thread (hard-pinned)
static unsigned hot_mask   = 0x1;   // all cores used for heavy emulation
static unsigned work_mask  = 0x1;   // hot_mask minus the EE core: MTGS/VU/workers
static int      bg_core    = -1;    // dedicated background core, or -1 = share work pool
static int      work_list[4];       // hot cores excluding ee_core (MTGS/VU/workers)
static int      work_count = 0;
static unsigned work_rr = 0, bg_rr = 0;

static void core_init_once(void) {
  if (core_count)
    return;
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
    mask = 0x7; // fallback: cores 0,1,2 (typical app allotment)
  int core_list[4];
  for (int c = 0; c < 4; c++)
    if (mask & (1u << c))
      core_list[core_count++] = c;

  // Reserve the TOP core for background only when 4+ are granted; with <=3 we
  // need them all for the heavy emulation threads.
  int hot_count;
  if (core_count >= 4) {
    hot_count = core_count - 1;
    bg_core   = core_list[core_count - 1];
  } else {
    hot_count = core_count;
    bg_core   = -1;
  }
  hot_mask = 0;
  for (int i = 0; i < hot_count; i++)
    hot_mask |= (1u << core_list[i]);

  ee_core = core_list[0];
  // MTGS/VU/worker pool = the hot cores EXCEPT the EE's, so heavy workers never
  // share -- nor migrate onto -- the EE core. If only one hot core exists (a 1-2
  // core grant) everything unavoidably shares it.
  work_count = 0;
  for (int i = 1; i < hot_count; i++)
    work_list[work_count++] = core_list[i];
  if (work_count == 0)
    work_list[work_count++] = ee_core;
  work_mask = (hot_count >= 2) ? (hot_mask & ~(1u << ee_core)) : hot_mask;


}

// EE/VM thread: hard-pinned to its own core (exclusive mask) -- it is the
// hottest thread and benefits most from a stable core / warm cache.
void pthr_pin_ee_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = ee_core; const unsigned m = 1u << ee_core;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);

}

// heavy emucore threads (MTGS, VU1, ring/texture workers): distinct preferred
// core round-robined over the work pool; work_mask lets a light worker migrate
// off a heavy peer's core but never onto the EE core.
// Counted under core_lock so the number in the log is exact; the driver-thread
// counter above is deliberately raced, but here the whole point of the line is
// "how many threads are there", so an approximate count would answer nothing.
static int work_assigned;

#ifdef NETHERSX2_VK_DIAGNOSTIC
// Written only by bridge thread #4; sampled by the exception handler so a
// fault inside the pre-target path is still attributable if the logger itself
// is the thing that faulted.
static volatile unsigned pthr_bridge4_stage;
static volatile unsigned pthr_bridge4_handle;

static inline void pthr_bridge4_set_stage(unsigned debug_id, unsigned stage) {
  if (debug_id == 4)
    __atomic_store_n(&pthr_bridge4_stage, stage, __ATOMIC_RELAXED);
}

static inline void pthr_bridge4_capture_handle(unsigned debug_id) {
  if (debug_id == 4)
    __atomic_store_n(&pthr_bridge4_handle, (unsigned)threadGetCurHandle(),
                     __ATOMIC_RELAXED);
}

unsigned pthr_debug_bridge4_stage(void) {
  return __atomic_load_n(&pthr_bridge4_stage, __ATOMIC_RELAXED);
}

unsigned pthr_debug_bridge4_handle(void) {
  return __atomic_load_n(&pthr_bridge4_handle, __ATOMIC_RELAXED);
}
#else
static inline void pthr_bridge4_set_stage(unsigned debug_id, unsigned stage) {
  (void)debug_id;
  (void)stage;
}

static inline void pthr_bridge4_capture_handle(unsigned debug_id) {
  (void)debug_id;
}
#endif

static int current_priority(void);

static void assign_work_core(unsigned debug_id) {
  pthr_bridge4_set_stage(debug_id, 3); // entering core_lock
  mutexLock(&core_lock);
  pthr_bridge4_set_stage(debug_id, 4); // core_lock acquired
  core_init_once();
  pthr_bridge4_set_stage(debug_id, 5); // core pool initialized
  const int core = work_list[work_rr++ % (unsigned)work_count]; const unsigned m = work_mask;
  const int n = ++work_assigned;
  pthr_bridge4_set_stage(debug_id, 6); // assignment selected
  mutexUnlock(&core_lock);
  pthr_bridge4_set_stage(debug_id, 7); // core_lock released
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
  pthr_bridge4_set_stage(debug_id, 8); // affinity syscall returned
  // This runs for EVERY thread the core creates, before it has a name -- so it
  // is the only place that can show the ones that never call prctl(PR_SET_NAME)
  // and therefore never reach pthr_pin_by_name(). Those keep this assignment for
  // good, and on a four-core grant the pool is cores 1 and 2, i.e. it includes
  // the core MTVU is hard-pinned to. A hardware log that names three threads and
  // says nothing about creation cannot tell "there are only three" from "there
  // are seven and four of them are anonymous".
  //
  // Bounded but not capped: the first 16 individually, then every 16th. A game
  // that spawns hundreds of short-lived threads therefore still leaves a count
  // in the log that is within 16 of the truth, instead of going silent at a hard
  // limit the way the driver-thread counter does. Not exact -- #100 would log as
  // #96 -- but "three or three hundred" is the question this line has to answer.
  //
  // The priority is whatever newlib's pthread_create left: the comments in this
  // file used to say "libnx hands every new thread its creator's", but current
  // libnx source (nx/source/runtime/newlib.c, __syscall_thread_create) calls
  // threadCreate(..., 0x3B, -2), i.e. a fixed 59 on the creator's core, and
  // the libnx the toolchain ships may or may not match. Printed here so the
  // log settles it: a thread that never gets a name sitting on core 1/2 at 30
  // would share the core with GS/MTVU at equal priority, and Horizon does not
  // time-slice between equal priorities; at 59 it is below everything and only
  // ever runs in their gaps.
  if (n <= 16 || (n % 16) == 0) {
    pthr_bridge4_set_stage(debug_id, 9); // entering assignment log
    corelog_log(CORELOG_INFO, "Wrapper",
                "worker thread #%d -> work pool core %d, mask 0x%x, inherited priority %d (PROVISIONAL -- if a named role line follows, that one already replaced core, mask and priority)",
                n, core, m, current_priority());
  }
  pthr_bridge4_set_stage(debug_id, 10); // assignment complete
}

// background threads (audio, etc.): the dedicated bg core if we have one, else
// share the work pool. For threads our vendored code spawns directly through
// newlib pthread_create (the audio mixer/callback) -- they never pass through
// the trampoline, so without this they'd default onto the EE core.
void pthr_pin_bg_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  int core; unsigned m;
  if (bg_core >= 0) { core = bg_core; m = 1u << bg_core; }
  else { core = work_list[bg_rr++ % (unsigned)work_count]; m = work_mask; }
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
}

// Horizon never preempts between equal priorities, so co-tenants of one core
// need distinct ones. Lower number is higher priority. What a fresh pthread
// starts with is NOT assumed here any more (see the read-back in
// assign_work_core()): every role sets its own explicitly, and failure is
// harmless -- the thread keeps whatever it arrived with.
#define EMU_THREAD_PRIO    30  // EE / MTGS / MTVU: above everything else
#define LIGHT_THREAD_PRIO  40  // CDVD / ISO Decompress / texture loader: GS and MTVU preempt them
#define SPU2_THREAD_PRIO   44  // between audio (43) and input (45)
#define DRIVER_THREAD_PRIO 47  // below audio and input on the shared bg core

// Read-back for the log lines. -1 when the kernel refuses, which the log then
// shows verbatim instead of inventing a number.
static int current_priority(void) {
  s32 prio = 0;
  return R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) ? (int)prio : -1;
}

void pthr_set_priority(int priority) {
  svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)priority);
}

// ---------------------------------------------------------------------------
// threads of statically linked third-party code (Mesa/NVK)
// ---------------------------------------------------------------------------

// libnvk.a and libnvk_support.a are linked into this binary and create their own
// worker threads for shader compilation and disk-cache work. They inherit the
// process default affinity, which is every granted core, so a driver thread can
// land on core 0 and interrupt the EE mid-frame. Core 3 is the only one with
// spare capacity -- it carries the 48 kHz audio callback and input polling and
// is otherwise idle -- so that is where they go, at a priority below both, so a
// driver thread can never cost an audio slot.
void pthr_pin_driver_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int bg = bg_core;
  mutexUnlock(&core_lock);
  if (bg < 0)
    return;   // three-core grant: no spare core, leaving the default is better
  if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, bg, 1u << bg))) {
    pthr_set_priority(DRIVER_THREAD_PRIO);
    // Bounded on purpose: Mesa creates a handful of workers, and the count is
    // the evidence that --wrap actually caught them. The counter is raced by
    // design -- the worst case is a couple of extra lines, which costs less
    // than a lock on a path that runs during shader compilation.
    static int logged;
    if (logged < 8) {
      logged++;
      corelog_log(CORELOG_INFO, "Wrapper", "driver thread -> core %d, mask 0x%x, priority %d",
                  bg, 1u << bg, DRIVER_THREAD_PRIO);
    }
  }
}

extern int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start)(void *), void *arg);

typedef struct { void *(*start)(void *); void *arg; } DriverStart;

static void *driver_trampoline(void *p) {
  DriverStart s = *(DriverStart *)p;
  free(p);
  pthr_pin_driver_core();
  return s.start(s.arg);
}

// Linked in through -Wl,--wrap=pthread_create. Anything that is not one of our
// own four creation sites arrives here, which in practice means Mesa/NVK.
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg) {
  DriverStart *s = malloc(sizeof *s);
  if (!s)
    return __real_pthread_create(thread, attr, start, arg);  // never fail a create
  s->start = start;
  s->arg   = arg;
  const int rc = __real_pthread_create(thread, attr, driver_trampoline, s);
  if (rc != 0)
    free(s);
  return rc;
}

// aaudio.c pins its mixer explicitly, main.c's emu_thread becomes the EE, and
// the trampoline below assigns work cores to the core's threads once the core
// names them through prctl(PR_SET_NAME). None of those want the driver pin.
int pthr_create_self(pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start)(void *), void *arg) {
  return __real_pthread_create(thread, attr, start, arg);
}

// ---------------------------------------------------------------------------
// sched_yield counter: spinning thread vs. working thread
// ---------------------------------------------------------------------------

// The core imports exactly one spin primitive -- sched_yield -- next to the
// real sleeping ones (pthread_cond_wait, sem_wait, nanosleep). That is the
// hybrid wait MTVU uses: spin for a while, then sleep. So "core 2 is at 100%"
// cannot by itself say whether VU1 is computing or idling waiting for the EE to
// queue work, and the two call for opposite fixes. Counting the calls per
// thread settles it; the log timestamps give the rate.
//
// Per-thread count lives in TLS and is therefore exact. The process total is a
// plain increment raced by every thread, so it can lose counts -- acceptable
// for a diagnostic that only has to distinguish hundreds per second from
// hundreds of thousands, and it avoids putting a lock on the hottest path in
// the process.
static __thread char tls_self_name[16];

const char *pthr_debug_current_name(void) {
  return tls_self_name[0] ? tls_self_name : "(unnamed)";
}

// ---------------------------------------------------------------------------
// per-thread CPU time of the three heavy roles
//
// The perf line in the Vulkan diag log (vk.c) says how many frames reached the
// screen, and the core-utilisation log says "core 2 at 95%", but neither says
// WHICH thread is the bottleneck: a core-2 figure of 95% is the same whether
// MTVU is computing or spinning on `yield`/`isb` waiting for the EE (PCSX2's
// ShortSpin()/WorkSema spin in userspace and reach sched_yield only after the
// spin budget runs out, so the sched_yield counter cannot tell the two apart).
// svcGetInfo(InfoType_ThreadTickCount, handle, -1) returns the ticks a thread
// has actually been scheduled across all cores, which does separate them: an
// EE at 97% of wall time with MTVU at 60% is EE-bound, GS at 98% is GS-bound,
// all three well below 100% means the frame is waiting on something else
// (GPU, present, I/O). Reported as a percentage of wall time per perf window.
//
// Only the thread itself can register (threadGetCurHandle() is per-thread), so
// this runs from pthr_pin_by_name(), before the "auto" test, so the numbers
// exist regardless of pinning mode. A role that gets recreated (renderer
// switch recreates GS) re-registers and its baseline resets.
typedef struct {
  const char *label;
  Handle handle;          // INVALID_HANDLE until the thread names itself
  u64 last_ticks;
} CpuTimeSlot;

static CpuTimeSlot cpu_time_slots[PTHR_CPU_ROLES] = {
  { "EE",   INVALID_HANDLE, 0 },
  { "GS",   INVALID_HANDLE, 0 },
  { "MTVU", INVALID_HANDLE, 0 },
};
static u64 cpu_time_last_wall;
static Mutex cpu_time_lock;

// InfoType 25 exists only on 13.0.0+; older firmware answers the same question
// under 0xF0000002 (InfoType_ThreadTickCountDeprecated). The sub-id UINT64_MAX
// (TickCountInfo_Total) sums all cores, which is what a migratable thread needs.
static Result cpu_time_ticks(Handle h, u64 *ticks) {
  const u32 id = hosversionAtLeast(13, 0, 0) ? InfoType_ThreadTickCount
                                             : InfoType_ThreadTickCountDeprecated;
  return svcGetInfo(ticks, id, h, TickCountInfo_Total);
}

static void cpu_time_register(const char *name) {
  int slot;
  if (!strcmp(name, "CPU Thread")) slot = 0;
  else if (!strcmp(name, "GS"))    slot = 1;
  else if (!strcmp(name, "MTVU"))  slot = 2;
  else return;
  u64 ticks = 0;
  const Handle h = threadGetCurHandle();
  if (R_FAILED(cpu_time_ticks(h, &ticks)))
    ticks = 0;
  mutexLock(&cpu_time_lock);
  cpu_time_slots[slot].handle = h;
  cpu_time_slots[slot].last_ticks = ticks;
  mutexUnlock(&cpu_time_lock);
}

int pthr_cpu_time_report(char *buf, size_t len) {
  const u64 now = armGetSystemTick();
  mutexLock(&cpu_time_lock);
  const u64 wall = (cpu_time_last_wall && now > cpu_time_last_wall)
                       ? now - cpu_time_last_wall : 0;
  cpu_time_last_wall = now;
  int n = 0, any = 0;
  for (unsigned i = 0; i < sizeof cpu_time_slots / sizeof cpu_time_slots[0]; ++i) {
    CpuTimeSlot *s = &cpu_time_slots[i];
    const char *sep = i ? " " : "";
    if (s->handle == INVALID_HANDLE) {
      n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s -", sep, s->label);
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(cpu_time_ticks(s->handle, &ticks))) {
      // The thread exited and its handle is gone (or was reused): say so
      // instead of carrying a stale number forward.
      n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s ?", sep, s->label);
      continue;
    }
    const u64 delta = ticks > s->last_ticks ? ticks - s->last_ticks : 0;
    s->last_ticks = ticks;
    any = 1;
    if (wall)
      n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s %u%%",
                    sep, s->label, (unsigned)((delta * 100 + wall / 2) / wall));
    else
      n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s ..", sep, s->label);
  }
  mutexUnlock(&cpu_time_lock);
  return any;
}

#ifdef NETHERSX2_VK_DIAGNOSTIC
void pthr_cpu_time_sample(CpuTimeSample *out) {
  if (!out)
    return;
  for (unsigned i = 0; i < PTHR_CPU_ROLES; ++i) {
    out->ticks[i] = 0;
    out->have[i] = 0;
  }
  mutexLock(&cpu_time_lock);
  for (unsigned i = 0; i < PTHR_CPU_ROLES; ++i) {
    CpuTimeSlot *s = &cpu_time_slots[i];
    if (s->handle == INVALID_HANDLE)
      continue;
    u64 ticks = 0;
    if (R_FAILED(cpu_time_ticks(s->handle, &ticks)))
      continue;
    out->ticks[i] = ticks;
    out->have[i] = 1;
  }
  mutexUnlock(&cpu_time_lock);
}

int pthr_cpu_time_ms(const CpuTimeSample *from, const CpuTimeSample *to,
                     char *buf, size_t len) {
  if (!from || !to || !buf || !len)
    return 0;
  int n = 0, any = 0;
  for (unsigned i = 0; i < PTHR_CPU_ROLES; ++i) {
    const char *sep = i ? " " : "";
    // No lock: only the static label is read here, and both samples were taken
    // by pthr_cpu_time_sample() under it.
    if (!from->have[i] || !to->have[i] || to->ticks[i] < from->ticks[i]) {
      n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s -",
                    sep, cpu_time_slots[i].label);
      continue;
    }
    any = 1;
    n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, "%s%s %.1f",
                  sep, cpu_time_slots[i].label,
                  (double)armTicksToNs(to->ticks[i] - from->ticks[i]) / 1e6);
  }
  if (any)
    n += snprintf(buf + n, n < (int)len ? len - (size_t)n : 0, " ms");
  return any;
}
#endif

static __thread unsigned long long tls_yield_count;
static unsigned long long g_yield_total;

// Both belong under the diagnostic flag because the only caller does: the
// counter still increments in production, but nothing reports it there, and a
// static helper with no caller is dead code the compiler warns about.
#ifdef NETHERSX2_VK_DIAGNOSTIC
#define PTHR_YIELD_REPORT_EVERY 100000ull

// Report early as well as often: a thread that yields rarely would otherwise
// produce its first line only after a very long time, and telling "barely
// yields" from "spins flat out" is the whole point.
static int yield_worth_reporting(unsigned long long n) {
  return n == 100 || n == 1000 || n == 10000 || (n % PTHR_YIELD_REPORT_EVERY) == 0;
}
#endif

int pthr_sched_yield(void) {
  const unsigned long long mine = ++tls_yield_count;
  ++g_yield_total;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  if (yield_worth_reporting(mine))
    corelog_log(CORELOG_INFO, "Wrapper",
                "sched_yield: thread \"%s\" called it %llux, process total %llux",
                tls_self_name[0] ? tls_self_name : "(unnamed)", mine, g_yield_total);
#else
  (void)mine;
#endif
  return sched_yield();
}

// ---------------------------------------------------------------------------
// role-based affinity, keyed on the thread names the core sets itself
// ---------------------------------------------------------------------------

// The core names its own threads through prctl(PR_SET_NAME). Disassembly of
// libemucore_tegrax1.so finds six call sites, every one passing option 15:
//   0x4adf40 "GS"                          0x7a6590 "ISO Decompress"
//   0x4b27e4 "MTVU"                        0x7f75b8 "ImGuiFullscreen Texture Loader"
//   0x699464 generic helper (the route by  0x83ccbc "CPU Thread"
//           which "CDVD"/"SPU2" arrive)
// imports.c used to discard all of it in a blind stub. Those names are the only
// handle on which thread is which: every one of them reaches us through the
// same pthread_create trampoline, so creation order is all assign_work_core()
// otherwise has to go on.
//
// Why names beat the round-robin: the round-robin hands out preferred cores in
// creation order, and CDVD, the ISO decompressor and the ImGui texture loader
// are all created BEFORE GS and MTVU. The two heaviest threads can therefore
// end up preferring the same core while a light one holds the other.
//
// GS and MTVU get an EXCLUSIVE core only when a fourth core exists to absorb
// the light threads. On the three-core album-applet grant (mask 0x7) the work
// pool is just two cores, and pinning GS and MTVU to one each would leave CDVD,
// the decompressor and the texture loader fighting whichever of the two is
// busier with nowhere else to go. There they keep the migratable work_mask, so
// a light worker can still move off a heavy peer -- the property the round-robin
// was designed around in the first place.
void pthr_pin_by_name(const char *name) {
  if (!name || !*name)
    return;
  // Recorded before the "auto" test and before the role dispatch, so the
  // sched_yield counter can name every thread the core bothers to name,
  // including the ones with no role to pin (CDVD, ISO Decompress, the ImGui
  // texture loader) and including runs with pinning switched off.
  snprintf(tls_self_name, sizeof tls_self_name, "%s", name);
  cpu_time_register(name);
  // "auto" leaves the trampoline's round-robin assignment exactly as it is.
  if (!strcmp(prefs_get_string("Wrapper/ThreadPinning", "role"), "auto"))
    return;

  mutexLock(&core_lock);
  core_init_once();
  const int ee = ee_core, bg = bg_core;
  const unsigned wm = work_mask;
  int gs, vu;
  if (work_count >= 2)      { gs = work_list[0]; vu = work_list[1]; }
  else if (work_count == 1) { gs = work_list[0]; vu = work_list[0]; }
  else                      { gs = ee;           vu = ee; }
  mutexUnlock(&core_lock);

  int core; unsigned mask; int prio = -1;
  if (!strcmp(name, "CPU Thread")) {
    core = ee; mask = 1u << ee; prio = EMU_THREAD_PRIO;
  } else if (!strcmp(name, "GS")) {
    core = gs; mask = (bg >= 0) ? (1u << gs) : wm; prio = EMU_THREAD_PRIO;
  } else if (!strcmp(name, "MTVU")) {
    core = vu; mask = (bg >= 0) ? (1u << vu) : wm; prio = EMU_THREAD_PRIO;
  } else if (!strcmp(name, "SPU2")) {
    // The core's own SPU2 thread, distinct from the aaudio mixer callback that
    // pthr_pin_bg_core() already parks on the background core. Keeping the two
    // together leaves that core to audio and input alone. Priority 44 sits
    // between audio (43) and input (45) -- Horizon never preempts between equal
    // priorities, so co-tenants of one core must differ.
    if (bg < 0)
      return;                                // no spare core: leave it in the pool
    core = bg; mask = 1u << bg; prio = SPU2_THREAD_PRIO;
  } else {
    // CDVD / ISO Decompress / ImGuiFullscreen Texture Loader / anything else:
    // I/O and light, the round-robin already placed them well enough.
    //
    // Named in the log even though nothing is changed, because the pool they
    // stay in is work_mask -- on a four-core grant that is cores 1 and 2, and
    // core 2 is the one MTVU is hard-pinned to with no escape. Whether the ISO
    // decompressor actually ends up sitting next to a 95%-busy MTVU cannot be
    // told from a log that never mentions it, so read back the affinity the
    // trampoline left the thread with instead of guessing.
    // Argument order is the REVERSE of svcSetThreadCoreMask(handle, core, mask):
    // the getter takes the outputs first. And what comes back is the thread's
    // preferred core plus its affinity mask -- where it is allowed to run, not a
    // sample of where it happened to be at this instant. The mask is the number
    // that answers the question either way.
    s32 pref = -1;
    u64 aff = 0;
    // Priority: these threads are created by the EE thread after it has taken
    // EMU_THREAD_PRIO. If newlib's pthread_create inherits the creator's
    // priority (what the older comments here assumed) they arrive at 30 -- the
    // same as GS and MTVU, which share cores 1/2 with them -- and because
    // Horizon never preempts between equal priorities, a CHD hunk
    // decompression that starts while MTVU is blocked on its semaphore keeps
    // the core until it is done, with MTVU waiting behind it. If instead
    // libnx's __syscall_thread_create uses its fixed 0x3B (59), they are
    // already below everything and nothing needs doing. The clamp below covers
    // the first case and is a no-op in the second; the "before -> after" in
    // the log says which one this build is in. Only ever lowered, never
    // raised: a thread that arrived below 40 for a reason is left alone.
    const int inherited = current_priority();
    int prio_now = inherited;
    if (inherited >= 0 && inherited < LIGHT_THREAD_PRIO &&
        R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)LIGHT_THREAD_PRIO)))
      prio_now = LIGHT_THREAD_PRIO;
    if (R_SUCCEEDED(svcGetThreadCoreMask(&pref, &aff, CUR_THREAD_HANDLE)))
      corelog_log(CORELOG_INFO, "Wrapper",
                  "thread \"%s\" -> no dedicated role, preferred core %d, affinity 0x%llx, pool mask 0x%x, priority %d -> %d",
                  name, (int)pref, (unsigned long long)aff, wm, inherited, prio_now);
    else
      corelog_log(CORELOG_INFO, "Wrapper",
                  "thread \"%s\" -> no dedicated role, pool mask 0x%x, priority %d -> %d",
                  name, wm, inherited, prio_now);
    return;
  }

  // Failure is harmless and worth a line rather than silence: the thread simply
  // keeps the affinity the trampoline gave it.
  if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask))) {
    // A rejected priority is worth seeing: it means EMU_THREAD_PRIO is outside
    // the range Horizon allows this process, and the thread silently kept the
    // inherited one.
    const char *note = "";
    if (prio >= 0 && R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)prio)))
      note = " (priority REJECTED)";
    corelog_log(CORELOG_INFO, "Wrapper", "thread \"%s\" -> core %d, mask 0x%x, priority %d%s",
                name, core, mask, prio, note);
  } else {
    corelog_log(CORELOG_WARN, "Wrapper",
                "thread \"%s\": svcSetThreadCoreMask(core %d, mask 0x%x) failed",
                name, core, mask);
  }
}

// ---------------------------------------------------------------------------
// thread creation (installs the fake TLS before running game code)
// ---------------------------------------------------------------------------

typedef struct {
  void *(*start)(void *);
  void *arg;
  unsigned debug_id;
  // Inline TLS storage is allocated by the parent and intentionally retained
  // for the worker's lifetime. The child installs it before calling free/log/
  // allocator code, avoiding libc work before its RW TLS is valid.
  uint8_t fake_tls[0x200] __attribute__((aligned(16)));
} ThreadStart;

typedef struct DetachedThread {
  pthread_t thread;
  struct DetachedThread *next;
} DetachedThread;

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;
static DetachedThread *detach_head;
static DetachedThread *detach_tail;
static pthread_t detach_reaper;
static unsigned active_core_threads;
static unsigned core_thread_created;
static int detach_reaper_started;
static int detach_reaper_stop;

static void *detach_reaper_main(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&thread_lock);
    while (!detach_head && !detach_reaper_stop)
      pthread_cond_wait(&thread_cond, &thread_lock);
    DetachedThread *item = detach_head;
    if (item) {
      detach_head = item->next;
      if (!detach_head)
        detach_tail = NULL;
    } else if (detach_reaper_stop) {
      pthread_mutex_unlock(&thread_lock);
      return NULL;
    }
    pthread_mutex_unlock(&thread_lock);

    pthread_join(item->thread, NULL);
    free(item);
  }
}

static void *thread_trampoline(void *p) {
  ThreadStart *s = p;
  void *(*start)(void *) = s->start;
  void *arg = s->arg;
  const unsigned debug_id = s->debug_id;
  void *fake_tls = s->fake_tls;

  // Install parent-allocated TLS before free(), logging, or any other libc call
  // on the new thread. The backing ThreadStart is retained because fake_tls
  // must remain live until the worker has fully exited.
  armSetTlsRw(fake_tls);
  const int trace = debug_id == 4;
  pthr_bridge4_set_stage(debug_id, 1); // TLS installed
  if (trace) {
    pthr_bridge4_capture_handle(debug_id);
    // Keep this first child-side marker text-only too: stage=1 means TLS was
    // installed but the first formatted logger call never returned.
    corelog_text(CORELOG_INFO, "Wrapper",
                 "pthread bridge #4 entered target");
    pthr_bridge4_set_stage(debug_id, 2); // entry marker logged
  }
  // Keep emulator workers off the EE and audio cores.
  assign_work_core(debug_id);
  pthr_bridge4_set_stage(debug_id, 11); // assignment returned
  if (trace) {
    // Text-only so this pre-target marker does not call vsnprintf().
    corelog_text(CORELOG_INFO, "Wrapper",
                 "pthread bridge #4 entering target routine");
    pthr_bridge4_set_stage(debug_id, 12); // pre-target marker logged
  }
  pthr_bridge4_set_stage(debug_id, 13); // immediately before start(arg)
  void *ret = start(arg);
  if (trace)
    corelog_log(CORELOG_INFO, "Wrapper",
                "pthread bridge #%u target returned=%p",
                debug_id, ret);
  // EGL ownership is thread-local.
  extern void egl_gl_ownership_release(void);
  egl_gl_ownership_release();
  pthread_mutex_lock(&thread_lock);
  active_core_threads--;
  pthread_cond_broadcast(&thread_cond);
  pthread_mutex_unlock(&thread_lock);
  return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  ThreadStart *s = calloc(1, sizeof(*s));
  if (!s)
    return ENOMEM;
  s->start = start;
  s->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  // the engine's worker/render threads recurse deeply; give them headroom
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
  }

  pthread_mutex_lock(&thread_lock);
  active_core_threads++;
  s->debug_id = ++core_thread_created;
  const unsigned debug_id = s->debug_id;
  pthread_mutex_unlock(&thread_lock);

  int ret = pthr_create_self(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (debug_id == 4)
    corelog_log(CORELOG_INFO, "Wrapper",
                "pthread bridge #%u create returned rc=%d",
                debug_id, ret);
  if (ret != 0) {
    pthread_mutex_lock(&thread_lock);
    active_core_threads--;
    pthread_cond_broadcast(&thread_cond);
    pthread_mutex_unlock(&thread_lock);
    free(s);
  }
  return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) { return pthread_join(thread, value_ptr); }

int pthread_detach_soloader(pthread_t thread) {
  if (!thread)
    return ESRCH;

  DetachedThread *item = malloc(sizeof(*item));
  if (!item)
    return ENOMEM;
  item->thread = thread;
  item->next = NULL;

  pthread_mutex_lock(&thread_lock);
  if (!detach_reaper_started) {
    int ret = pthr_create_self(&detach_reaper, NULL, detach_reaper_main, NULL);
    if (ret != 0) {
      pthread_mutex_unlock(&thread_lock);
      free(item);
      return ret;
    }
    detach_reaper_started = 1;
  }
  if (detach_tail)
    detach_tail->next = item;
  else
    detach_head = item;
  detach_tail = item;
  pthread_cond_signal(&thread_cond);
  pthread_mutex_unlock(&thread_lock);
  return 0;
}

void pthr_shutdown(void) {
  for (;;) {
    pthread_mutex_lock(&thread_lock);
    const unsigned active = active_core_threads;
    pthread_mutex_unlock(&thread_lock);
    if (!active)
      break;
    svcSleepThread(1000000000ULL);
  }

  pthread_mutex_lock(&thread_lock);
  if (!detach_reaper_started) {
    pthread_mutex_unlock(&thread_lock);
    return;
  }
  detach_reaper_stop = 1;
  pthread_cond_broadcast(&thread_cond);
  pthread_mutex_unlock(&thread_lock);

  pthread_join(detach_reaper, NULL);
  detach_reaper_started = 0;
}
pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  // newlib on devkitA64 doesn't expose pthread_getschedparam; the game only
  // reads these to echo them back, so reporting a default schedule is fine
  (void)thread;
  if (policy) *policy = 0; // SCHED_OTHER
  if (param) param->sched_priority = 0;
  return 0;
}

enum {
  PTHREAD_ONCE_SOLOADER_INIT = 0,
  PTHREAD_ONCE_SOLOADER_RUNNING = 1,
  PTHREAD_ONCE_SOLOADER_DONE = 2,
};

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return EINVAL;

  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_DONE)
    return 0;

  pthread_mutex_lock(&once_lock);
  for (;;) {
    int state = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_ONCE_SOLOADER_DONE) {
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    if (state == PTHREAD_ONCE_SOLOADER_INIT) {
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_RUNNING, __ATOMIC_RELEASE);
      pthread_mutex_unlock(&once_lock);

      (*init_routine)();

      pthread_mutex_lock(&once_lock);
      __atomic_store_n(once_control, PTHREAD_ONCE_SOLOADER_DONE, __ATOMIC_RELEASE);
      pthread_cond_broadcast(&once_cond);
      pthread_mutex_unlock(&once_lock);
      return 0;
    }

    // A peer is still inside init_routine. POSIX requires callers to wait
    // until the initializer has completed; returning early exposes partially
    // initialized game singletons to other threads.
    pthread_mutex_unlock(&once_lock);
    extern void egl_gl_ownership_park(void);
    egl_gl_ownership_park();
    pthread_mutex_lock(&once_lock);
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == PTHREAD_ONCE_SOLOADER_RUNNING)
      pthread_cond_wait(&once_cond, &once_lock);
  }
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  return mutex_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) != PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&mutex->magic, 0, __ATOMIC_RELEASE);
  pthread_mutex_t *real = mutex->real_ptr;
  mutex->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_mutex_destroy(real);
  free(real);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  // fast path: uncontended
  if (pthread_mutex_trylock(mutex->real_ptr) == 0)
    return 0;
  // contended: never block on a game mutex while holding the real GL context
  // (ABBA against the game's BeginCriticalSectionGL) -- park first
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_mutex_lock(mutex->real_ptr);
  // render thread still holding (park keeps it): the mutex holder might want
  // exactly that context, so poll with handover service instead of blocking
  for (;;) {
    if (pthread_mutex_trylock(mutex->real_ptr) == 0)
      return 0;
    struct timespec ts = { 0, 100 * 1000 }; // 0.1 ms
    nanosleep(&ts, NULL);
  }
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex || !mutex->real_ptr) return EINVAL;
  return pthread_mutex_unlock(mutex->real_ptr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) != PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&cond->magic, 0, __ATOMIC_RELEASE);
  pthread_cond_t *real = cond->real_ptr;
  cond->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_cond_destroy(real);
  free(real);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_broadcast(cond->real_ptr);
}

// render-thread wait slice: while it parks holding the GL context, its waits
// poll for handover requests at this granularity
#define RENDER_WAIT_SLICE_NS (4 * 1000 * 1000)

static void timespec_add_ns(struct timespec *ts, long ns) {
  ts->tv_nsec += ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}

static int timespec_before(const struct timespec *a, const struct timespec *b) {
  if (a->tv_sec != b->tv_sec)
    return a->tv_sec < b->tv_sec;
  return a->tv_nsec < b->tv_nsec;
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  // about to park: hand back the GL context so other threads can render
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
  // render thread still holding the GL binding: wait ONE short slice and
  // report a timeout as a spurious wakeup (legal; predicate loops re-check).
  // Never loop internally: the game may signal without holding the mutex,
  // and a signal landing between laps is lost forever -- the one-shot
  // level-ready signal then leaves the renderer asleep for good.
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, &ts);
  if (r == ETIMEDOUT) {
    return 0; // spurious wakeup (POSIX/bionic allow them)
  }
  return r;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !abstime || !egl_gl_thread_holds_context())
    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
  // same single-slice spurious-wakeup scheme as pthread_cond_wait_soloader;
  // the caller's real deadline is only ever reported once it actually passes
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  const int final_slice = !timespec_before(&ts, abstime);
  int r = pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr,
                                 final_slice ? abstime : &ts);
  if (r == ETIMEDOUT && !final_slice) {
    return 0; // spurious wakeup before the caller's deadline
  }
  return r;
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}
