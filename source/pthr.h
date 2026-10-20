/* pthr.h -- bionic-to-newlib pthread wrappers
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __PTHR_H__
#define __PTHR_H__

#include <pthread.h>
#include <stdint.h>
#include <time.h>

// Android's pthread_attr_t/mutex_t/cond_t reinterpreted: the first machine
// word of the game's storage holds a pointer to the real newlib object.
typedef struct {
  pthread_attr_t *real_ptr; // overlays bionic `uint32_t flags`
  int32_t magic;            // overlays bionic `void *stack_base` low word
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} pthread_attr_t_bionic;

// Bionic's LP64 mutex (40 bytes) / cond (48) storage carries {real_ptr,
// magic} in its first 16 bytes: the per-call "initialized yet?" check is one
// atomic load (the game locks/signals thousands of times per frame).
// Statically initialized storage is zero past the kind word, so magic == 0
// means uninitialized.
typedef struct { pthread_mutex_t *real_ptr; uint32_t magic; } pthread_mutex_t_bionic;
typedef struct { pthread_cond_t  *real_ptr; uint32_t magic; } pthread_cond_t_bionic;

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param);
int pthread_join_soloader(pthread_t thread, void **value_ptr);
int pthread_detach_soloader(pthread_t thread);
int pthread_equal_soloader(pthread_t t1, pthread_t t2);
pthread_t pthread_self_soloader(void);
int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void));
int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param);
void pthr_shutdown(void);

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr);

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex);
int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime);
int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond);
int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond);

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex);
int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex);

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr);
int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize);

// Core threads require writable bionic-compatible TLS.
void pthr_install_fake_tls(void);

void pthr_pin_bg_core(void);

void pthr_set_priority(int priority);

void pthr_pin_ee_core(void);

// Role-based affinity keyed on the name the core hands to prctl(PR_SET_NAME).
// No-op when Wrapper/ThreadPinning is "auto", and for names with no role.
void pthr_pin_by_name(const char *name);

// Affinity for threads created by statically linked third-party code -- in
// practice Mesa/NVK's. Reached only through __wrap_pthread_create.
void pthr_pin_driver_core(void);

// Our own creation sites use this so they bypass the wrap and keep whatever
// affinity they arrange themselves. See pthr.c.
int pthr_create_self(pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start)(void *), void *arg);

// Counting replacement for the core's sched_yield. The core imports it as its
// only spin primitive, so how often each thread calls it is what separates
// "MTVU is computing" from "MTVU is spinning idle waiting for the EE".
int pthr_sched_yield(void);

void pthr_ensure_fake_tls(void);

// Number of heavy roles whose CPU time is tracked (EE, GS, MTVU). Outside the
// diagnostic guard because the slot table itself is always compiled.
#define PTHR_CPU_ROLES 3

// Formats "EE 97% GS 61% MTVU 95%": scheduled CPU time of the three heavy
// roles as a share of wall time since the previous call, from
// svcGetInfo(InfoType_ThreadTickCount). "-" = thread has not named itself yet,
// "?" = its handle no longer answers (thread exited). Returns 0 if no role is
// registered at all. Meant for the perf window line in the Vulkan diag log.
int pthr_cpu_time_report(char *buf, size_t len);

#ifdef NETHERSX2_VK_DIAGNOSTIC
// Per-frame sampling of the same three roles, for the hitch line in vk.c.
// pthr_cpu_time_report() consumes its own baselines (it is a rate), which is
// wrong for a single frame: a 82 ms frame is 1.4% of a 6 s window, so the
// window percentage cannot say whether that frame was CPU-bound at all. These
// two read absolute tick counts and leave every baseline alone, so both can be
// used from the same window without disturbing each other.
typedef struct {
  uint64_t ticks[PTHR_CPU_ROLES];  // absolute ThreadTickCount, 0 when unavailable
  int      have[PTHR_CPU_ROLES];   // role registered and its handle answered
} CpuTimeSample;

void pthr_cpu_time_sample(CpuTimeSample *out);
// Formats "EE 51.2 GS 12.0 MTVU 8.4 ms" for the tick delta between two samples.
// Returns 0 when no role produced a usable delta.
int pthr_cpu_time_ms(const CpuTimeSample *from, const CpuTimeSample *to,
                     char *buf, size_t len);
#endif

#endif
