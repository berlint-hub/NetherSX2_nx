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
static void assign_work_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = work_list[work_rr++ % (unsigned)work_count]; const unsigned m = work_mask;
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, m);
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

// Horizon never preempts between equal priorities and libnx gives every thread
// its creator's, so the core-3 tenants carry distinct ones. Failure is harmless:
// the thread keeps the inherited priority.
// Horizon never preempts between equal priorities and libnx hands every thread
// its creator's, so co-tenants of one core need distinct ones. Lower number is
// higher priority.
#define EMU_THREAD_PRIO    30  // EE / MTGS / MTVU: above everything else
#define SPU2_THREAD_PRIO   44  // between audio (43) and input (45)
#define DRIVER_THREAD_PRIO 47  // below audio and input on the shared bg core

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
    if (R_SUCCEEDED(svcGetThreadCoreMask(&pref, &aff, CUR_THREAD_HANDLE)))
      corelog_log(CORELOG_INFO, "Wrapper",
                  "thread \"%s\" -> no dedicated role, preferred core %d, affinity 0x%llx, pool mask 0x%x",
                  name, (int)pref, (unsigned long long)aff, wm);
    else
      corelog_log(CORELOG_INFO, "Wrapper",
                  "thread \"%s\" -> no dedicated role, pool mask 0x%x", name, wm);
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
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  pthr_install_fake_tls();
  // Keep emulator workers off the EE and audio cores.
  assign_work_core();
  void *ret = s.start(s.arg);
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
  ThreadStart *s = malloc(sizeof(*s));
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
  pthread_mutex_unlock(&thread_lock);

  int ret = pthr_create_self(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
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
