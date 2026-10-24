/* vk.c -- Vulkan (Mesa NVK) bridge. See vk.h for the why.
 *
 * Three functional shims sit between the core and NVK:
 *   1. vkCreateAndroidSurfaceKHR -> NVK's VI surface (the core has no VI path).
 *   2. vkQueuePresentKHR         -> count presented frames (main.c's liveness /
 *                                   CPU-boost-off trigger; the GL swap counter
 *                                   never moves on the Vulkan path).
 *   3. vkGetPhysicalDeviceMemoryProperties2 -> clamp each reported
 *                                   VK_EXT_memory_budget budget to at least its
 *                                   corresponding Vulkan heap size. Diagnostic
 *                                   builds log NVK's value before the clamp and
 *                                   the value exposed to the core afterward.
 * Plus vkCreateInstance / vkEnumerateInstanceExtensionProperties, which swap the
 * core's VK_KHR_android_surface for NVK's real VK_NN_vi_surface.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "vk.h"
#include "../saltysd_display.h"

#ifdef USE_VULKAN

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include "../util.h"
#include "../config.h"
#include "../prefs.h"
#include "../lsfg/lsfg_bridge.h"
#include "../lsfg/lsfg_schedule.h"
#include "../pthr.h"
#include "../libc_shim.h"
#include "../corelog.h"

// The Mesa NVK portlib package (nxvk.pc, built from berlint-hub/nxvk) ships only
// libnvk.a + libnvk_support.a, and libnvk.a defines exactly one entry point:
// vk_icdGetInstanceProcAddr. There is no Vulkan loader on Horizon, so nothing
// supplies the plain vkGetInstanceProcAddr that vk_gipa_hook() below resolves
// every instance function through. Without this shim the VK link fails with an
// undefined symbol.
//
// Guarded because the older 23-archive layout in vulkan/lib does export
// vkGetInstanceProcAddr, and a second strong definition would collide with it.
// NETHERSX2_NVK_ICD_ONLY is set by the Makefile only for the portlib link path.
#ifdef NETHERSX2_NVK_ICD_ONLY
#include <vulkan/vk_icd.h> // vk_icdGetInstanceProcAddr
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  return vk_icdGetInstanceProcAddr(instance, name);
}
#endif

volatile int vk_present_count = 0;

static PFN_vkGetPhysicalDeviceMemoryProperties2 real_memprops2 = NULL;
static VkPhysicalDevice real_memprops2_pd = VK_NULL_HANDLE;

#ifdef NETHERSX2_VK_DIAGNOSTIC
static Mutex vk_diag_mutex;
static FILE *vk_diag_file;

static FILE *
vk_diag_open_locked(void) {
  if (!vk_diag_file) {
    mkdir(DATA_ROOT, 0777);
    vk_diag_file = fopen(DATA_ROOT "/nethersx2-vulkan.log", "a");
    if (vk_diag_file)
      setvbuf(vk_diag_file, NULL, _IOLBF, 0);
  }
  return vk_diag_file;
}

void
vk_diag_reset(void) {
  mutexLock(&vk_diag_mutex);
  if (vk_diag_file) {
    fclose(vk_diag_file);
    vk_diag_file = NULL;
  }
  mkdir(DATA_ROOT, 0777);
  vk_diag_file = fopen(DATA_ROOT "/nethersx2-vulkan.log", "w");
  if (vk_diag_file) {
    setvbuf(vk_diag_file, NULL, _IOLBF, 0);
    fprintf(vk_diag_file, "NetherSX2 Vulkan diagnostic %s (build %s)\n",
            NETHERSX2_VERSION, NETHERSX2_BUILD_ID);
    fflush(vk_diag_file);
    fsync(fileno(vk_diag_file));
  }
  mutexUnlock(&vk_diag_mutex);
}

void
vk_diag_note(const char *format, ...) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  mutexLock(&vk_diag_mutex);
  FILE *file = vk_diag_open_locked();
  if (file) {
    fprintf(file, "[%lld.%03lld] ", (long long)time.tv_sec,
            (long long)(time.tv_nsec / 1000000));
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    /* Keep routine diagnostics readable without syncing every Vulkan call;
     * persist errors and unresolved GIPA lookups because they identify failures. */
    fflush(file);
    if (strstr(format, "error") || strstr(format, "failed") ||
        strstr(format, "unresolved"))
      fsync(fileno(file));
  }
  mutexUnlock(&vk_diag_mutex);
}

void
vk_diag_exception(uint64_t pc, uint64_t far, uint32_t esr,
                  uint64_t sp, uint64_t frame_pointer, uint64_t link_register,
                  uint32_t thread_handle, unsigned bridge4_stage,
                  uint32_t bridge4_handle) {
  char pc_map[192], lr_map[192], report[1024];
  corelog_describe_address(pc_map, sizeof(pc_map), (const void *)(uintptr_t)pc);
  corelog_describe_address(lr_map, sizeof(lr_map),
                           (const void *)(uintptr_t)link_register);
  s32 preferred_core = -1;
  u64 affinity_mask = 0;
  const Result affinity_result = svcGetThreadCoreMask(
      &preferred_core, &affinity_mask, (Handle)thread_handle);
  const int length = snprintf(
      report, sizeof(report),
      "NetherSX2 fatal CPU exception\n"
      "pc=0x%016llx map=%s\n"
      "far=0x%016llx esr=0x%08x\n"
      "sp=0x%016llx fp=0x%016llx\n"
      "lr=0x%016llx map=%s\n"
      "thread_handle=0x%08x name=%s affinity=%s preferred_core=%d mask=0x%llx\n"
      "bridge4_stage=%u bridge4_handle=0x%08x\n",
      (unsigned long long)pc, pc_map,
      (unsigned long long)far, esr,
      (unsigned long long)sp, (unsigned long long)frame_pointer,
      (unsigned long long)link_register, lr_map, thread_handle,
      pthr_debug_current_name(),
      R_SUCCEEDED(affinity_result) ? "available" : "unavailable",
      preferred_core, (unsigned long long)affinity_mask,
      bridge4_stage, bridge4_handle);
  const int fd = open(DATA_ROOT "/nethersx2-exception.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd >= 0) {
    if (length > 0)
      (void)write(fd, report, (size_t)length < sizeof(report) ?
                             (size_t)length : sizeof(report));
    fsync(fd);
    close(fd);
  }
}
#endif

typedef struct {
  VkQueue queue;
  uint32_t family;
} QueueRecord;

#define MAX_TRACKED_QUEUES 8

static VkInstance       tracked_instance;
static VkPhysicalDevice tracked_physical_device;
static VkDevice         tracked_device;
static uint32_t         tracked_instance_api = VK_API_VERSION_1_0;
static QueueRecord      tracked_queues[MAX_TRACKED_QUEUES];
static uint32_t         tracked_queue_count;
static uint32_t         lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;

static VkSwapchainKHR tracked_swapchain;
static VkExtent2D     tracked_swapchain_extent;
static int            tracked_swapchain_lsfg_compatible;
static VkFence        low_latency_pending_fence;
static uint64_t       low_latency_next_vblank_ns;

static LsfgNxRuntime *lsfg_runtime;
static int lsfg_init_attempted;
static int lsfg_device_capable;
static int lsfg_session_prepared;
static int lsfg_enabled_requested;
static int lsfg_runtime_available;
/* Measure source timing before LSFG; two 60 Hz FIFO presents would halve a
 * native 50/60 FPS game. */
static uint64_t lsfg_last_source_present_ns;
static double lsfg_source_interval_ns;
static unsigned lsfg_source_samples;
/* Consecutive source presents at or above the target. See the fractional
 * warm-up below for why this has to be a run and not a single sample. */
static unsigned lsfg_at_cap_run;
static unsigned lsfg_high_fps_slow_samples;
static int lsfg_previous_requested;
static int lsfg_rate_decision = -1; /* -1: measuring, 0: generate, 1: passthrough */
/* Fractional generation: interpolate on only some presents so the output rate
 * lands on a target instead of always doubling. Kept as plain numbers so the
 * schedule can be exercised outside the emulator. */
static int    lsfg_fractional_ready;   /* source estimate frozen */
static double lsfg_frozen_source_fps;  /* measured before any generation */
static double lsfg_gen_phase;          /* accumulator in [0,1) */
static int    lsfg_native_gap;         /* a present the bridge never saw */

static PFN_vkCreateDevice real_create_device;
static PFN_vkGetDeviceQueue real_get_device_queue;
static PFN_vkGetDeviceQueue2 real_get_device_queue2;
static PFN_vkCreateSwapchainKHR real_create_swapchain;
static PFN_vkGetSwapchainImagesKHR real_get_swapchain_images;
static PFN_vkDestroySwapchainKHR real_destroy_swapchain;
static PFN_vkDestroyDevice real_destroy_device;
static PFN_vkQueueSubmit real_queue_submit;
static PFN_vkQueueSubmit2 real_queue_submit2;
static PFN_vkAcquireNextImageKHR real_acquire_next_image;
static PFN_vkWaitForFences real_wait_for_fences;
#ifdef NETHERSX2_VK_DIAGNOSTIC
static unsigned vk_diag_submit_calls;
static unsigned vk_diag_submit2_calls;
static unsigned vk_diag_acquire_calls;
static unsigned vk_diag_swapchain_image_calls;
/* Windowed counters replace per-submit/per-fence log spam. */
static unsigned long long vk_diag_pace_samples;
static unsigned long long vk_diag_pace_waits;
static uint64_t vk_diag_pace_ns;
static uint64_t vk_diag_pace_max_ns;
static unsigned long long vk_diag_pace_skips;
static unsigned long long vk_diag_pace_late;
static unsigned long long vk_diag_sync_fence_waits;
static uint64_t vk_diag_sync_fence_ns;
static uint64_t vk_diag_sync_fence_max_ns;
static uint64_t frame_ll_pace_ns;
static uint64_t frame_ll_fence_ns;
#endif


static int lsfg_requested(void) {
  return __atomic_load_n(&lsfg_enabled_requested, __ATOMIC_ACQUIRE);
}

static uint64_t lsfg_monotonic_ns(void);

static volatile int vk_low_latency_cached;
static volatile int vk_display_sync_suppressed_low_latency;

void vk_low_latency_configure(int enabled) {
  __atomic_store_n(&vk_low_latency_cached, enabled != 0, __ATOMIC_RELEASE);
  __atomic_store_n(&vk_display_sync_suppressed_low_latency, 0, __ATOMIC_RELEASE);
}

static int vk_low_latency_enabled(void) {
  /* Display sync needs FIFO's vblank back-pressure. If SaltyNX comes up late,
   * suppress the explicit pacer before selecting the first swapchain as well.
   * NVK_LOW_LATENCY is startup-only: never mutate process environment here. */
  if (saltysd_display_sync_active()) {
    __atomic_store_n(&vk_display_sync_suppressed_low_latency, 1,
                     __ATOMIC_RELEASE);
    return 0;
  }
  /* Once a swapchain has been handed to display sync, do not re-enable a
   * wall-clock pacer if a transient rescan briefly loses the shared block. */
  if (__atomic_load_n(&vk_display_sync_suppressed_low_latency,
                      __ATOMIC_ACQUIRE))
    return 0;
  return __atomic_load_n(&vk_low_latency_cached, __ATOMIC_ACQUIRE);
}

static unsigned low_latency_rate_hz(void) {
  unsigned rate = saltysd_display_sync_service_rate();
  if (rate < 20 || rate > 240) rate = 60;
  return rate;
}

static uint64_t low_latency_period_ns(void) {
  return UINT64_C(1000000000) / low_latency_rate_hz();
}

/* Sleep only when the current frame can still meet its panel deadline. Once a
 * vblank has passed, waiting for the following one adds a whole refresh period
 * to an already-late frame (about 18 ms at 50 Hz), which turns brief overload
 * into a sustained low-FPS cadence. The wrapper requests IMMEDIATE when
 * available, then FIFO; the pinned low-latency WSI preserves IMMEDIATE and
 * maps its FIFO fallback to MAILBOX, both at swap interval zero. */
static uint64_t low_latency_pace_delay_ns(uint64_t now, uint64_t next_vblank,
                                          uint64_t period, uint64_t lead,
                                          unsigned long long *skipped,
                                          unsigned long long *late) {
  if (!next_vblank || !period)
    return 0;
  const uint64_t target = next_vblank > lead ? next_vblank - lead : next_vblank;
  if (now >= next_vblank) {
    if (skipped)
      *skipped += 1 + (now - next_vblank) / period;
    return 0;
  }
  if (now >= target) {
    if (late) ++*late;
    return 0;
  }
  return target - now;
}

/* Pace each GPU submit against the next panel vblank. The existing SaltyNX
 * display-sync query supplies the panel
 * refresh rate; leave a 2 ms submission lead-in for NVK and the VI queue. */
static uint64_t low_latency_pace_before_submit(void) {
  if (!vk_low_latency_enabled() || !low_latency_next_vblank_ns)
    return 0;
  const uint64_t now = lsfg_monotonic_ns();
  const uint64_t lead = UINT64_C(2000000);
  const uint64_t period = low_latency_period_ns();
#ifdef NETHERSX2_VK_DIAGNOSTIC
  ++vk_diag_pace_samples;
  const uint64_t wait_ns = low_latency_pace_delay_ns(
      now, low_latency_next_vblank_ns, period, lead,
      &vk_diag_pace_skips, &vk_diag_pace_late);
#else
  const uint64_t wait_ns = low_latency_pace_delay_ns(
      now, low_latency_next_vblank_ns, period, lead, NULL, NULL);
#endif
  if (!wait_ns)
    return 0;
  svcSleepThread((int64_t)wait_ns);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t after = lsfg_monotonic_ns();
  const uint64_t actual_ns = after > now ? after - now : wait_ns;
  ++vk_diag_pace_waits;
  vk_diag_pace_ns += actual_ns;
  if (actual_ns > vk_diag_pace_max_ns) vk_diag_pace_max_ns = actual_ns;
  frame_ll_pace_ns += actual_ns;
  return actual_ns;
#else
  return wait_ns;
#endif
}

static double lsfg_measured_source_fps(void) {
  if (lsfg_source_samples < 8 || !(lsfg_source_interval_ns > 0.0))
    return 0.0;                    /* not enough history to trust yet */
  return 1e9 / lsfg_source_interval_ns;
}

static volatile int lsfg_fractional_mode_cached = -1;
static volatile int lsfg_target_fps_cached = -1;

static int lsfg_mode_is_fractional(void) {
  int cached = __atomic_load_n(&lsfg_fractional_mode_cached, __ATOMIC_ACQUIRE);
  if (cached < 0) {
    const int value = !strcmp(prefs_get_string("Wrapper/LSFGMode", "auto"),
                              "fractional");
    int expected = -1;
    (void)__atomic_compare_exchange_n(&lsfg_fractional_mode_cached, &expected,
                                      value, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE);
    cached = __atomic_load_n(&lsfg_fractional_mode_cached, __ATOMIC_ACQUIRE);
  }
  return cached;
}

static double lsfg_target_fps_pref(void) {
  int target = __atomic_load_n(&lsfg_target_fps_cached, __ATOMIC_ACQUIRE);
  if (target < 0) {
    target = prefs_get_int("Wrapper/LSFGTargetFPS", 60);
    if (target < 30) target = 30;
    if (target > 120) target = 120;
    int expected = -1;
    (void)__atomic_compare_exchange_n(&lsfg_target_fps_cached, &expected,
                                      target, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE);
    target = __atomic_load_n(&lsfg_target_fps_cached, __ATOMIC_ACQUIRE);
  }
  return (double)target;
}

int vk_lsfg_is_available(void) {
  return __atomic_load_n(&lsfg_runtime_available, __ATOMIC_ACQUIRE);
}

int vk_lsfg_is_enabled(void) {
  return lsfg_requested();
}

int vk_lsfg_is_high_fps_passthrough(void) {
  if (!lsfg_requested())
    return 0;
  /* In fractional mode there is no wholesale passthrough state: a source at or
   * above the target simply never earns a generated frame. Report that, so the
   * quick menu still says the same true thing it said before. */
  if (lsfg_mode_is_fractional())
    /* Deliberately not gated on lsfg_fractional_ready: that flag now only latches
     * once the source is BELOW the target, so requiring it here would make this
     * branch unreachable and the quick menu would stop reporting passthrough
     * while warming up at the cap -- exactly when reporting it is informative. */
    return lsfg_frozen_source_fps > 0.0 &&
           lsfg_frozen_source_fps >= lsfg_target_fps_pref();
  return lsfg_rate_decision == 1;
}

int vk_lsfg_is_fractional(void) {
  return lsfg_requested() && lsfg_mode_is_fractional();
}

void vk_lsfg_request_enabled(int enabled) {
  __atomic_store_n(&lsfg_enabled_requested, enabled != 0, __ATOMIC_RELEASE);
}

static const char *lsfg_dll_path(void) {
  return DATA_ROOT "/lsfg/Lossless.dll";
}

static int file_readable(const char *path) {
  if (!path || !path[0]) return 0;
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fclose(f);
  return 1;
}

static void lsfg_destroy_runtime(void) {
  if (lsfg_runtime) {
    lsfg_nx_destroy(lsfg_runtime);
    lsfg_runtime = NULL;
  }
}

static void remember_queue(VkQueue queue, uint32_t family) {
  if (!queue) return;
  for (uint32_t i = 0; i < tracked_queue_count; ++i) {
    if (tracked_queues[i].queue == queue) {
      tracked_queues[i].family = family;
      return;
    }
  }
  if (tracked_queue_count < MAX_TRACKED_QUEUES) {
    tracked_queues[tracked_queue_count].queue = queue;
    tracked_queues[tracked_queue_count].family = family;
    ++tracked_queue_count;
  }
}

static int find_queue_family(VkQueue queue, uint32_t *family) {
  for (uint32_t i = 0; i < tracked_queue_count; ++i) {
    if (tracked_queues[i].queue == queue) {
      if (family) *family = tracked_queues[i].family;
      return 1;
    }
  }
  return 0;
}

static int has_device_extension(VkPhysicalDevice physical_device,
                                const char *wanted) {
  uint32_t count = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, NULL) != VK_SUCCESS)
    return 0;
  VkExtensionProperties *properties = count ? malloc(sizeof(*properties) * count) : NULL;
  if (count && !properties) return 0;
  VkResult result = vkEnumerateDeviceExtensionProperties(
      physical_device, NULL, &count, properties);
  int found = 0;
  if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
    for (uint32_t i = 0; i < count; ++i) {
      if (!strcmp(properties[i].extensionName, wanted)) {
        found = 1;
        break;
      }
    }
  }
  free(properties);
  return found;
}

static int extension_enabled(const VkDeviceCreateInfo *create_info,
                             const char *wanted) {
  for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
    if (!strcmp(create_info->ppEnabledExtensionNames[i], wanted)) return 1;
  return 0;
}

static VkResult find_lsfg_main_queue_family(
    VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    uint32_t *main_family) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
  if (!count) return VK_ERROR_FEATURE_NOT_PRESENT;

  VkQueueFamilyProperties *properties = malloc(sizeof(*properties) * count);
  if (!properties) return VK_ERROR_OUT_OF_HOST_MEMORY;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, properties);

  uint32_t main = VK_QUEUE_FAMILY_IGNORED;

  /* NVK's graphics/present queue supports the fused LSFG transfer work. */
  for (uint32_t pass = 0; pass < 2 && main == VK_QUEUE_FAMILY_IGNORED; ++pass) {
    const VkQueueFlags wanted = pass ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT;
    for (uint32_t i = 0; i < create_info->queueCreateInfoCount; ++i) {
      const uint32_t family = create_info->pQueueCreateInfos[i].queueFamilyIndex;
      if (family < count && create_info->pQueueCreateInfos[i].queueCount &&
          (properties[family].queueFlags & wanted)) {
        main = family;
        break;
      }
    }
  }

  free(properties);
  *main_family = main;
  return main == VK_QUEUE_FAMILY_IGNORED ?
      VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}

static void reset_tracked_swapchain(void) {
  lsfg_destroy_runtime();
  tracked_swapchain = VK_NULL_HANDLE;
  low_latency_pending_fence = VK_NULL_HANDLE;
  low_latency_next_vblank_ns = 0;
  memset(&tracked_swapchain_extent, 0, sizeof(tracked_swapchain_extent));
  tracked_swapchain_lsfg_compatible = 0;
  lsfg_init_attempted = 0;
  lsfg_last_source_present_ns = 0;
  lsfg_source_interval_ns = 0.0;
  lsfg_source_samples = 0;
  lsfg_at_cap_run = 0;
  lsfg_high_fps_slow_samples = 0;
  lsfg_previous_requested = 0;
  lsfg_rate_decision = -1;
  lsfg_fractional_ready = 0;
  lsfg_frozen_source_fps = 0.0;
  lsfg_gen_phase = 0.0;
  lsfg_native_gap = 0;
  __atomic_store_n(&lsfg_runtime_available, 0, __ATOMIC_RELEASE);
}

static uint64_t lsfg_monotonic_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
  return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

#ifdef NETHERSX2_VK_DIAGNOSTIC
/* Periodic frame-timing summary.
 *
 * TEST3's analysis of a GT3 hardware log (docs/logs/gt3-nvk-diag-notes.md §5)
 * concludes that the CPU knobs cannot be A/B tested at all without frame timing
 * in the log: it carries no FPS and no frame time anywhere, so every comparison
 * of MTVU / Instant VU1 / VU cycle steal ends up being judged by feel. Measured
 * here rather than read from the core because this shim sees every presentation
 * exactly once, which is the number that actually reaches the screen.
 *
 * Intervals above PERF_MAX_INTERVAL_NS are counted as stalls and kept out of
 * the distribution, so a ten-second menu pause or a swapchain transition does
 * not masquerade as the worst frame of the session. */
#define PERF_RING            512u
#define PERF_REPORT_PRESENTS 300u
#define PERF_MIN_INTERVAL_NS UINT64_C(1000000)    /* 1 ms   */
#define PERF_MAX_INTERVAL_NS UINT64_C(500000000)  /* 500 ms */

static uint64_t perf_ring[PERF_RING];
static unsigned perf_ring_count;
static unsigned perf_window_presents;
static uint64_t perf_window_start_ns;
static uint64_t perf_prev_ns;
static unsigned perf_stalls;
static unsigned long long perf_generated;
static FastmemStats perf_fm_prev;
static VkDeviceSize perf_heap_window_peak;
static VkDeviceSize perf_heap_all_time_peak;
static VkDeviceSize perf_heap0_size;
static int perf_heap_sampled;

/* The directly forwarded NVK query only reads its atomic heap accounting; keep
 * this outside vk_diag_note() so no diagnostic-file mutex is held across it. */
static void perf_sample_heap_usage(void) {
  VkPhysicalDevice pd = __atomic_load_n(&real_memprops2_pd, __ATOMIC_ACQUIRE);
  if (!real_memprops2 || pd == VK_NULL_HANDLE)
    return;

  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
  };
  VkPhysicalDeviceMemoryProperties2 props = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
    .pNext = &budget,
  };
  real_memprops2(pd, &props);
  if (!props.memoryProperties.memoryHeapCount)
    return;

  const VkDeviceSize used = budget.heapUsage[0];
  perf_heap0_size = props.memoryProperties.memoryHeaps[0].size;
  if (used > perf_heap_window_peak) perf_heap_window_peak = used;
  if (used > perf_heap_all_time_peak) perf_heap_all_time_peak = used;
  perf_heap_sampled = 1;
}

/* Insertion-sort a copy and average the worst `count` entries -- the 1% low
 * expressed as a frame time. Pure and bounded (n <= PERF_RING) so the
 * percentile can be checked without a GPU. */
static double perf_worst_mean_ns(uint64_t *v, unsigned n, unsigned count) {
  if (!n || !count) return 0.0;
  for (unsigned i = 1; i < n; ++i) {
    const uint64_t key = v[i];
    unsigned j = i;
    while (j && v[j - 1] > key) { v[j] = v[j - 1]; --j; }
    v[j] = key;
  }
  if (count > n) count = n;
  uint64_t sum = 0;
  for (unsigned i = n - count; i < n; ++i) sum += v[i];
  return (double)sum / (double)count;
}

/* Which side of the CPU/GPU boundary a slow window sits on.
 *
 * The per-thread CPU counters on the second perf line say how much each thread
 * ran. They cannot say what a thread that is NOT running was waiting for, and a
 * GT3 hardware log landed exactly in that hole: 30.4 fps mean with no thread
 * above 55 % CPU -- half the frame rate at less CPU work, which is waiting, not
 * computing. These counters name the waits.
 *
 *   fence / queue-idle   wall time blocked inside a Vulkan wait, i.e. a thread
 *                        parked on the GPU. Summed over threads, so the two
 *                        percentages can together exceed 100 % when the GS and
 *                        EE threads wait at the same time.
 *   pipelines            vkCreateGraphicsPipelines count and wall time. A scene
 *                        that still compiles pipelines while it runs hitches
 *                        here rather than on the GPU, and no GS accuracy
 *                        setting will change that.
 *   cmds                 command-recording counts. Blending that has to read
 *                        the render target it is drawing shows up either as
 *                        barriers (texture-barrier path) or as copyimg/blit
 *                        (target duplicated because that path is unavailable);
 *                        GS downloads show up as copy2buf, which is what
 *                        EmuCore/GS/HWDownloadMode governs.
 *
 * Diagnostic build only, like the rest of the perf report. Polls
 * (vkWaitForFences with timeout 0) are counted but never timed: the core's
 * CPU-spin readback path calls them in a tight loop, and two clock reads per
 * iteration would cost more than the poll being measured.
 *
 * Unlike the older submit/acquire shims these do not fall back to the bare
 * vkX symbol. The portlib link path (NETHERSX2_NVK_ICD_ONLY) ships an archive
 * whose only entry point is vk_icdGetInstanceProcAddr, so a bare reference is
 * an undefined symbol at link time. A probe is installed only when the driver
 * returned non-NULL, which makes the NULL guards unreachable; they stay because
 * handing the core a shim for a function the driver does not have would be
 * worse than a branch that cannot run.
 */
static PFN_vkQueueWaitIdle real_queue_wait_idle;
static PFN_vkCreateGraphicsPipelines real_create_graphics_pipelines;
static PFN_vkCmdPipelineBarrier real_cmd_pipeline_barrier;
static PFN_vkCmdCopyImageToBuffer real_cmd_copy_image_to_buffer;
static PFN_vkCmdCopyImage real_cmd_copy_image;
static PFN_vkCmdBlitImage real_cmd_blit_image;

static unsigned long long gpu_fence_calls, gpu_fence_ns, gpu_fence_polls;
static unsigned long long gpu_idle_calls, gpu_idle_ns;
static unsigned long long gpu_pipe_calls, gpu_pipe_ns;
static unsigned long long gpu_barrier_calls, gpu_copy2buf_calls;
static unsigned long long gpu_copyimg_calls, gpu_blit_calls;

/* Wall time the presenting thread spent inside the three calls that can block on
 * the GPU without ever touching a fence. This is the field that was missing both
 * times a diagnosis went wrong here: "no thread is above 55% and the frame rate
 * is low, so something must be waiting on the GPU" was settled by looking at
 * fences, which measured 0.1% of the window -- but a FIFO swapchain with no free
 * image blocks its caller inside vkAcquireNextImageKHR, and a compositor that
 * cannot take the frame blocks it inside vkQueuePresentKHR, and neither of those
 * is a fence wait. While the internal resolution was reduced, GT3 was bound by
 * CPU work in the GS thread and these stayed small. At 2x internal resolution
 * the GPU has four times the pixels of native to fill, so the wall can move to
 * the GPU side, and this is the only place in the log where that would show. */
static unsigned long long gpu_acq_calls, gpu_acq_ns;
static unsigned long long gpu_sub_calls, gpu_sub_ns;
static unsigned long long gpu_pres_calls, gpu_pres_ns;

/* Per-frame hitch attribution.
 *
 * Everything above is a per-window total and a window is 300 frames / ~6 s, so a
 * single long frame is invisible in it: the worst frame of the GT3 run measured
 * on build e0e44b4 was 82.4 ms (1% low 22.65 fps) inside a window reporting
 * EE 68% / GS 58% / pipelines 3.1 ms total / fence 8.1 ms total / 13046 copies.
 * 82.4 ms is 1.4% of that window, so no percentage in it could have shown the
 * hitch even if the frame had been 100% CPU-bound on one thread. The window
 * lines answer "what is this stretch of racing limited by"; they structurally
 * cannot answer "where did that one stuttering frame go".
 *
 * So the same quantities are also differenced per frame here and the longest
 * frame of each window is dumped with its own breakdown. That line separates the
 * four sinks a frame can disappear into: a Vulkan call that blocked, a file read
 * that blocked (FileIoStats), CPU time on one of the three heavy roles
 * (CpuTimeSample), or none of those -- which is itself the answer, because it
 * leaves the driver's own threads and the core's unnamed ones.
 *
 * The frame boundary is a present, which this shim already sees exactly once.
 * PERF_HITCH_NS is the reporting threshold: a PAL frame is 20 ms, so 40 ms is two
 * dropped frames, the shortest hitch that is reliably visible while racing. */
#define PERF_HITCH_NS UINT64_C(40000000)  /* 40 ms */

/* Wall time inside the calls that can block the presenting thread. acquire and
 * submit accumulate within the frame they are called in. present is measured in
 * vkQueuePresentKHR_native, which for frame N runs *after* perf_observe_present
 * of frame N, so at attribution time frame_pres_ns still holds the previous
 * present's duration -- which is exactly the part of the interval just measured
 * that the present call consumed. (The LSFG bridge path bypasses
 * vkQueuePresentKHR_native, so with frame generation enabled this term reads 0;
 * every hardware run so far has lsfg off.) */
static uint64_t frame_acq_ns, frame_sub_ns, frame_pres_ns;

/* Window counters as of the previous present, so a frame's own share can be
 * differenced out of them. Reset alongside the counters in perf_report(), which
 * zeroes them at the end of every window. */
static unsigned long long prev_barrier, prev_copyimg, prev_copy2buf, prev_blit;
static unsigned long long prev_fence_calls, prev_idle_calls, prev_pipe_calls;
static uint64_t prev_fence_ns, prev_idle_ns, prev_pipe_ns;
/* Absolute (never reset by anyone), so these are kept as sample pairs. */
static FileIoStats prev_io;
static CpuTimeSample prev_cpu;

/* The longest frame of the current window and what it consisted of. */
static uint64_t hitch_ns;
static unsigned hitch_count;
static uint64_t hitch_acq_ns, hitch_sub_ns, hitch_pres_ns;
static uint64_t hitch_ll_pace_ns, hitch_ll_fence_ns;
static unsigned long long hitch_barrier, hitch_copyimg, hitch_copy2buf, hitch_blit;
static unsigned long long hitch_fence_calls, hitch_idle_calls, hitch_pipe_calls;
static uint64_t hitch_fence_ns, hitch_idle_ns, hitch_pipe_ns;
static FileIoStats hitch_io_from, hitch_io_to;
static CpuTimeSample hitch_cpu_from, hitch_cpu_to;
static int hitch_have;

static void perf_note_frame(uint64_t interval) {
  const uint64_t acq = frame_acq_ns, sub = frame_sub_ns, pres = frame_pres_ns;
  const uint64_t pace = frame_ll_pace_ns, sync_fence = frame_ll_fence_ns;
  frame_acq_ns = frame_sub_ns = frame_pres_ns = 0;
  frame_ll_pace_ns = frame_ll_fence_ns = 0;

  const unsigned long long d_barrier  = gpu_barrier_calls   - prev_barrier;
  const unsigned long long d_copyimg  = gpu_copyimg_calls   - prev_copyimg;
  const unsigned long long d_copy2buf = gpu_copy2buf_calls  - prev_copy2buf;
  const unsigned long long d_blit     = gpu_blit_calls      - prev_blit;
  const unsigned long long d_fcall    = gpu_fence_calls     - prev_fence_calls;
  const unsigned long long d_icall    = gpu_idle_calls      - prev_idle_calls;
  const unsigned long long d_pcall    = gpu_pipe_calls      - prev_pipe_calls;
  const uint64_t d_fence_ns = gpu_fence_ns - prev_fence_ns;
  const uint64_t d_idle_ns  = gpu_idle_ns  - prev_idle_ns;
  const uint64_t d_pipe_ns  = gpu_pipe_ns  - prev_pipe_ns;
  prev_barrier     = gpu_barrier_calls;
  prev_copyimg     = gpu_copyimg_calls;
  prev_copy2buf    = gpu_copy2buf_calls;
  prev_blit        = gpu_blit_calls;
  prev_fence_calls = gpu_fence_calls;
  prev_idle_calls  = gpu_idle_calls;
  prev_pipe_calls  = gpu_pipe_calls;
  prev_fence_ns    = gpu_fence_ns;
  prev_idle_ns     = gpu_idle_ns;
  prev_pipe_ns     = gpu_pipe_ns;

  if (interval < PERF_HITCH_NS) {
    /* Still advance the two absolute baselines, so a frame that does hitch is
     * differenced against its own immediate predecessor and not against the
     * last hitch. */
    fileio_stats_snapshot(&prev_io);
    pthr_cpu_time_sample(&prev_cpu);
    return;
  }

  ++hitch_count;
  FileIoStats io_now;
  CpuTimeSample cpu_now;
  fileio_stats_snapshot(&io_now);
  pthr_cpu_time_sample(&cpu_now);
  if (interval > hitch_ns) {
    hitch_ns          = interval;
    hitch_acq_ns      = acq;
    hitch_sub_ns      = sub;
    hitch_pres_ns     = pres;
    hitch_ll_pace_ns  = pace;
    hitch_ll_fence_ns = sync_fence;
    hitch_barrier     = d_barrier;
    hitch_copyimg     = d_copyimg;
    hitch_copy2buf    = d_copy2buf;
    hitch_blit        = d_blit;
    hitch_fence_calls = d_fcall;
    hitch_idle_calls  = d_icall;
    hitch_pipe_calls  = d_pcall;
    hitch_fence_ns    = d_fence_ns;
    hitch_idle_ns     = d_idle_ns;
    hitch_pipe_ns     = d_pipe_ns;
    hitch_io_from     = prev_io;
    hitch_io_to       = io_now;
    hitch_cpu_from    = prev_cpu;
    hitch_cpu_to      = cpu_now;
    hitch_have        = 1;
  }
  prev_io  = io_now;
  prev_cpu = cpu_now;
}

static void gpu_note_time(unsigned long long *calls, unsigned long long *ns,
                          uint64_t t0, uint64_t t1) {
  __atomic_fetch_add(calls, 1, __ATOMIC_RELAXED);
  if (t1 > t0) __atomic_fetch_add(ns, t1 - t0, __ATOMIC_RELAXED);
}

static void gpu_note_count(unsigned long long *calls) {
  __atomic_fetch_add(calls, 1, __ATOMIC_RELAXED);
}

static VkResult VKAPI_CALL
vkWaitForFences_shim(VkDevice device, uint32_t fence_count, const VkFence *fences,
                     VkBool32 wait_all, uint64_t timeout) {
  if (!real_wait_for_fences) return VK_ERROR_INITIALIZATION_FAILED;
  if (timeout == 0) {
    gpu_note_count(&gpu_fence_polls);
    const VkResult r = real_wait_for_fences(device, fence_count, fences,
                                            wait_all, timeout);
    if (r == VK_SUCCESS && low_latency_pending_fence) {
      for (uint32_t i = 0; i < fence_count; ++i) {
        if (fences[i] == low_latency_pending_fence &&
            real_wait_for_fences(device, 1, &low_latency_pending_fence,
                                 VK_TRUE, 0) == VK_SUCCESS) {
          low_latency_pending_fence = VK_NULL_HANDLE;
          break;
        }
      }
    }
    return r;
  }
  const uint64_t t0 = lsfg_monotonic_ns();
  const VkResult r = real_wait_for_fences(device, fence_count, fences, wait_all, timeout);
  gpu_note_time(&gpu_fence_calls, &gpu_fence_ns, t0, lsfg_monotonic_ns());
  if (r == VK_SUCCESS && low_latency_pending_fence) {
    for (uint32_t i = 0; i < fence_count; ++i) {
      if (fences[i] == low_latency_pending_fence) {
        const VkFence pending = low_latency_pending_fence;
        if (real_wait_for_fences(device, 1, &pending, VK_TRUE, 0) == VK_SUCCESS)
          low_latency_pending_fence = VK_NULL_HANDLE;
        break;
      }
    }
  }
  return r;
}

static VkResult VKAPI_CALL
vkQueueWaitIdle_shim(VkQueue queue) {
  if (!real_queue_wait_idle) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t t0 = lsfg_monotonic_ns();
  const VkResult r = real_queue_wait_idle(queue);
  gpu_note_time(&gpu_idle_calls, &gpu_idle_ns, t0, lsfg_monotonic_ns());
  return r;
}

static VkResult VKAPI_CALL
vkCreateGraphicsPipelines_shim(VkDevice device, VkPipelineCache cache,
                               uint32_t count, const VkGraphicsPipelineCreateInfo *infos,
                               const VkAllocationCallbacks *alloc, VkPipeline *out) {
  if (!real_create_graphics_pipelines) return VK_ERROR_INITIALIZATION_FAILED;
  const uint64_t t0 = lsfg_monotonic_ns();
  const VkResult r = real_create_graphics_pipelines(device, cache, count, infos, alloc, out);
  /* One call can carry several pipelines; count both so a batch of one and a
   * batch of eight are distinguishable in the report. */
  gpu_note_time(&gpu_pipe_calls, &gpu_pipe_ns, t0, lsfg_monotonic_ns());
  if (count > 1) __atomic_fetch_add(&gpu_pipe_calls, count - 1, __ATOMIC_RELAXED);
  return r;
}

static void VKAPI_CALL
vkCmdPipelineBarrier_shim(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                          VkDependencyFlags flags, uint32_t mem_count, const VkMemoryBarrier *mem,
                          uint32_t buf_count, const VkBufferMemoryBarrier *buf,
                          uint32_t img_count, const VkImageMemoryBarrier *img) {
  if (!real_cmd_pipeline_barrier) return;
  gpu_note_count(&gpu_barrier_calls);
  real_cmd_pipeline_barrier(cb, src, dst, flags, mem_count, mem, buf_count, buf, img_count, img);
}

static void VKAPI_CALL
vkCmdCopyImageToBuffer_shim(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                            VkBuffer dst, uint32_t region_count,
                            const VkBufferImageCopy *regions) {
  if (!real_cmd_copy_image_to_buffer) return;
  gpu_note_count(&gpu_copy2buf_calls);
  real_cmd_copy_image_to_buffer(cb, src, src_layout, dst, region_count, regions);
}

static void VKAPI_CALL
vkCmdCopyImage_shim(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                    VkImage dst, VkImageLayout dst_layout, uint32_t region_count,
                    const VkImageCopy *regions) {
  if (!real_cmd_copy_image) return;
  gpu_note_count(&gpu_copyimg_calls);
  real_cmd_copy_image(cb, src, src_layout, dst, dst_layout, region_count, regions);
}

static void VKAPI_CALL
vkCmdBlitImage_shim(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                    VkImage dst, VkImageLayout dst_layout, uint32_t region_count,
                    const VkImageBlit *regions, VkFilter filter) {
  if (!real_cmd_blit_image) return;
  gpu_note_count(&gpu_blit_calls);
  real_cmd_blit_image(cb, src, src_layout, dst, dst_layout, region_count, regions, filter);
}

/* Signature locks. A shim that only forwards its arguments would still compile
 * with a narrower parameter type -- the forward just widens it -- and the ABI
 * mismatch would only show up as corruption on hardware, which is how the
 * svcGetThreadCoreMask argument-order mistake looked. Assigning each shim to
 * its own PFN type makes that an incompatible-pointer-types error here. */
__attribute__((used)) static const PFN_vkWaitForFences gpu_sig_fence = vkWaitForFences_shim;
__attribute__((used)) static const PFN_vkQueueWaitIdle gpu_sig_idle = vkQueueWaitIdle_shim;
__attribute__((used)) static const PFN_vkCreateGraphicsPipelines gpu_sig_pipe = vkCreateGraphicsPipelines_shim;
__attribute__((used)) static const PFN_vkCmdPipelineBarrier gpu_sig_barrier = vkCmdPipelineBarrier_shim;
__attribute__((used)) static const PFN_vkCmdCopyImageToBuffer gpu_sig_copy2buf = vkCmdCopyImageToBuffer_shim;
__attribute__((used)) static const PFN_vkCmdCopyImage gpu_sig_copyimg = vkCmdCopyImage_shim;
__attribute__((used)) static const PFN_vkCmdBlitImage gpu_sig_blit = vkCmdBlitImage_shim;

/* One table instead of seven near-identical if-blocks in each of the two proc
 * address hooks. The `real` slot is the address of the typed pointer above, so
 * the driver's function is stored through a PFN_vkVoidFunction* and read back
 * through its own type in the shim; both are pointers of the same size and
 * alignment, and the cast is what keeps the table short. */
typedef struct {
  const char *name;
  PFN_vkVoidFunction *real;
  PFN_vkVoidFunction shim;
} GpuProbe;

static const GpuProbe gpu_probes[] = {
  { "vkWaitForFences",           (PFN_vkVoidFunction *)&real_wait_for_fences,           (PFN_vkVoidFunction)vkWaitForFences_shim },
  { "vkQueueWaitIdle",           (PFN_vkVoidFunction *)&real_queue_wait_idle,           (PFN_vkVoidFunction)vkQueueWaitIdle_shim },
  { "vkCreateGraphicsPipelines", (PFN_vkVoidFunction *)&real_create_graphics_pipelines, (PFN_vkVoidFunction)vkCreateGraphicsPipelines_shim },
  { "vkCmdPipelineBarrier",      (PFN_vkVoidFunction *)&real_cmd_pipeline_barrier,      (PFN_vkVoidFunction)vkCmdPipelineBarrier_shim },
  { "vkCmdCopyImageToBuffer",    (PFN_vkVoidFunction *)&real_cmd_copy_image_to_buffer,  (PFN_vkVoidFunction)vkCmdCopyImageToBuffer_shim },
  { "vkCmdCopyImage",            (PFN_vkVoidFunction *)&real_cmd_copy_image,            (PFN_vkVoidFunction)vkCmdCopyImage_shim },
  { "vkCmdBlitImage",            (PFN_vkVoidFunction *)&real_cmd_blit_image,            (PFN_vkVoidFunction)vkCmdBlitImage_shim },
};

/* Returns the shim to hand back for `name`, or NULL when this probe set has
 * nothing to say about it (including when the driver returned no function). */
static PFN_vkVoidFunction
gpu_probe_lookup(const char *name, PFN_vkVoidFunction fn) {
  if (!name || !fn) return NULL;
  for (unsigned i = 0; i < sizeof(gpu_probes) / sizeof(gpu_probes[0]); ++i) {
    if (strcmp(gpu_probes[i].name, name) != 0) continue;
    *gpu_probes[i].real = fn;
    return gpu_probes[i].shim;
  }
  return NULL;
}

static void perf_report(uint64_t now) {
  const uint64_t elapsed = (now > perf_window_start_ns) ? now - perf_window_start_ns : 0;
  const double seconds = (double)elapsed / 1e9;
  const double avg_fps = (seconds > 0.0) ? (double)perf_window_presents / seconds : 0.0;

  uint64_t scratch[PERF_RING];
  unsigned n = 0;
  uint64_t worst = 0;
  for (unsigned i = 0; i < perf_ring_count; ++i) {
    const uint64_t v = perf_ring[i];
    if (v < PERF_MIN_INTERVAL_NS) continue;
    if (v > worst) worst = v;
    scratch[n++] = v;
  }
  unsigned tail = n / 100;
  if (n && !tail) tail = 1;
  const double worst_mean = perf_worst_mean_ns(scratch, n, tail);

  vk_diag_note("perf: %u presents / %.3f s = %.0f fps | worst %.1f ms | 1%% low %.0f fps"
               " | stalls %u | panel_hz=%u | lsfg %s src=%.0f lsfg_target=%.0f gen=%llu",
               perf_window_presents, seconds, avg_fps,
               (double)worst / 1e6, worst_mean > 0.0 ? 1e9 / worst_mean : 0.0,
               perf_stalls, saltysd_display_sync_service_rate(),
               !lsfg_requested() ? "off"
                   : (lsfg_mode_is_fractional() ? "fractional" : "auto"),
               lsfg_frozen_source_fps, lsfg_target_fps_pref(), perf_generated);

  // Second line of the same window: who was busy. Both halves exist because
  // the first line alone cannot say WHY a window was slow. The CPU share
  // (pthr.c) separates EE-bound / GS-bound / MTVU-bound / none-of-them, and
  // the fastmem counters (libc_shim.c) show whether page faults were part of
  // it: "lazy" faults are first touches resolved by mapping (expected to decay
  // to ~0 once a game's working set is mapped), "to core" faults went to the
  // core's handler, and of those the non-PROTECTED ones are accesses that fell
  // onto the slow path for good -- the number that must stay at 0 while
  // "live/peak" sits below the limit, and that explains a slowdown if it does
  // not.
  char cpu[96];
  if (!pthr_cpu_time_report(cpu, sizeof cpu))
    snprintf(cpu, sizeof cpu, "no role registered");
  FastmemStats fm;
  fastmem_stats_snapshot(&fm);
  vk_diag_note("perf: cpu %s | fastmem lazy %llu faults/%llu pages, limit %llu, map-fail %llu,"
               " to core %llu (protected %llu), live %u peak %u of %u",
               cpu,
               fm.lazy_faults - perf_fm_prev.lazy_faults,
               fm.lazy_pages - perf_fm_prev.lazy_pages,
               fm.limit_hits - perf_fm_prev.limit_hits,
               fm.map_failures - perf_fm_prev.map_failures,
               fm.to_core - perf_fm_prev.to_core,
               fm.to_core_prot - perf_fm_prev.to_core_prot,
               fm.live_pages, fm.peak_pages, fm.page_limit);
  perf_fm_prev = fm;

  /* Third line of the same window: where the wall time that is not CPU time
   * went. Field meanings are documented on the counter block above. */
  {
    const double win_ms = seconds * 1000.0;
    const double fence_ms = (double)gpu_fence_ns / 1e6;
    const double idle_ms = (double)gpu_idle_ns / 1e6;
    const double pipe_ms = (double)gpu_pipe_ns / 1e6;
    const double acq_ms = (double)gpu_acq_ns / 1e6;
    const double sub_ms = (double)gpu_sub_ns / 1e6;
    const double pres_ms = (double)gpu_pres_ns / 1e6;
    vk_diag_note("perf: gpu blocked fence %llu x %.1f ms (%.0f%%) + polls %llu,"
                 " queue-idle %llu x %.1f ms (%.0f%%)"
                 " | pipelines %llu x %.1f ms (%.0f%%)"
                 " | vk acquire %llu x %.1f ms (%.0f%%) submit %llu x %.1f ms (%.0f%%)"
                 " present %llu x %.1f ms (%.0f%%)"
                 " | cmds barrier %llu copy2buf %llu copyimg %llu blit %llu",
                 gpu_fence_calls, fence_ms,
                 win_ms > 0.0 ? 100.0 * fence_ms / win_ms : 0.0,
                 gpu_fence_polls,
                 gpu_idle_calls, idle_ms,
                 win_ms > 0.0 ? 100.0 * idle_ms / win_ms : 0.0,
                 gpu_pipe_calls, pipe_ms,
                 win_ms > 0.0 ? 100.0 * pipe_ms / win_ms : 0.0,
                 gpu_acq_calls, acq_ms,
                 win_ms > 0.0 ? 100.0 * acq_ms / win_ms : 0.0,
                 gpu_sub_calls, sub_ms,
                 win_ms > 0.0 ? 100.0 * sub_ms / win_ms : 0.0,
                 gpu_pres_calls, pres_ms,
                 win_ms > 0.0 ? 100.0 * pres_ms / win_ms : 0.0,
                 gpu_barrier_calls, gpu_copy2buf_calls,
                 gpu_copyimg_calls, gpu_blit_calls);
  }

  vk_diag_note("perf: pace_hz=%u calls=%llu waits=%llu total=%.1f max=%.1f ms "
               "skipped=%llu late=%llu | fence waits=%llu total=%.1f max=%.1f ms",
               low_latency_rate_hz(), vk_diag_pace_samples,
               vk_diag_pace_waits, (double)vk_diag_pace_ns / 1e6,
               (double)vk_diag_pace_max_ns / 1e6, vk_diag_pace_skips,
               vk_diag_pace_late, vk_diag_sync_fence_waits,
               (double)vk_diag_sync_fence_ns / 1e6,
               (double)vk_diag_sync_fence_max_ns / 1e6);
  perf_sample_heap_usage();
  if (perf_heap_sampled) {
    vk_diag_note("perf: heap peak %llu MiB (all-time %llu MiB), heap size %llu MiB",
                 (unsigned long long)(perf_heap_window_peak >> 20),
                 (unsigned long long)(perf_heap_all_time_peak >> 20),
                 (unsigned long long)(perf_heap0_size >> 20));
    perf_heap_window_peak = 0;
    perf_heap_sampled = 0;
  }
  vk_diag_pace_samples = vk_diag_pace_waits = vk_diag_pace_skips = vk_diag_pace_late = 0;
  vk_diag_pace_ns = vk_diag_pace_max_ns = 0;
  vk_diag_sync_fence_waits = 0;
  vk_diag_sync_fence_ns = vk_diag_sync_fence_max_ns = 0;

  /* Fourth line of the same window, emitted only when at least one frame was
   * long enough to be visible: what the longest such frame was made of. */
  if (hitch_have) {
    char hcpu[96];
    if (!pthr_cpu_time_ms(&hitch_cpu_from, &hitch_cpu_to, hcpu, sizeof hcpu))
      snprintf(hcpu, sizeof hcpu, "no role registered");
    const double io_ms = (double)(hitch_io_to.ns - hitch_io_from.ns) / 1e6;
    vk_diag_note("perf: hitch %.1f ms (%u frame(s) over %u ms this window)"
                 " | vk acquire %.1f submit %.1f present %.1f ms"
                 " | sync pace %.1f fence %.1f ms"
                 " | fence %llu x %.1f idle %llu x %.1f pipe %llu x %.1f ms"
                 " | cmds barrier %llu copyimg %llu copy2buf %llu blit %llu"
                 " | cpu %s"
                 " | io read %llu x %llu KiB blocked %.1f ms",
                 (double)hitch_ns / 1e6, hitch_count,
                 (unsigned)(PERF_HITCH_NS / UINT64_C(1000000)),
                 (double)hitch_acq_ns / 1e6, (double)hitch_sub_ns / 1e6,
                 (double)hitch_pres_ns / 1e6,
                 (double)hitch_ll_pace_ns / 1e6,
                 (double)hitch_ll_fence_ns / 1e6,
                 hitch_fence_calls, (double)hitch_fence_ns / 1e6,
                 hitch_idle_calls, (double)hitch_idle_ns / 1e6,
                 hitch_pipe_calls, (double)hitch_pipe_ns / 1e6,
                 hitch_barrier, hitch_copyimg, hitch_copy2buf, hitch_blit,
                 hcpu,
                 hitch_io_to.calls - hitch_io_from.calls,
                 (hitch_io_to.bytes - hitch_io_from.bytes) >> 10, io_ms);
  }

  gpu_fence_calls = gpu_fence_ns = gpu_fence_polls = 0;
  gpu_idle_calls = gpu_idle_ns = 0;
  gpu_pipe_calls = gpu_pipe_ns = 0;
  gpu_barrier_calls = gpu_copy2buf_calls = 0;
  gpu_copyimg_calls = gpu_blit_calls = 0;
  gpu_acq_calls = gpu_acq_ns = 0;
  gpu_sub_calls = gpu_sub_ns = 0;
  gpu_pres_calls = gpu_pres_ns = 0;
  /* The frame deltas are computed against these, so they must follow the
   * counters they mirror back to zero or the next frame's delta wraps. */
  prev_barrier = prev_copyimg = prev_copy2buf = prev_blit = 0;
  prev_fence_calls = prev_idle_calls = prev_pipe_calls = 0;
  prev_fence_ns = prev_idle_ns = prev_pipe_ns = 0;
  hitch_ns = 0;
  hitch_count = 0;
  hitch_have = 0;

  perf_window_presents = 0;
  perf_ring_count = 0;
  perf_stalls = 0;
  perf_generated = 0;
  perf_window_start_ns = 0;
  perf_prev_ns = now;
}

static void perf_observe_present(void) {
  const uint64_t now = lsfg_monotonic_ns();
  perf_sample_heap_usage();
  ++perf_window_presents;
  if (!now) return;
  if (!perf_window_start_ns) perf_window_start_ns = now;
  if (!perf_prev_ns) {
    /* First present of the run: no interval to attribute yet, but the two
     * absolute baselines must start here. Seeding them lazily inside
     * perf_note_frame() would leave them zero for the first measured frame,
     * which would then be credited with every byte read and every CPU tick
     * since boot -- and the first frames of a boot are exactly the ones long
     * enough to be reported as a hitch. */
    fileio_stats_snapshot(&prev_io);
    pthr_cpu_time_sample(&prev_cpu);
  }
  if (perf_prev_ns && now > perf_prev_ns) {
    const uint64_t interval = now - perf_prev_ns;
    perf_note_frame(interval);
    if (interval > PERF_MAX_INTERVAL_NS) ++perf_stalls;
    else if (perf_ring_count < PERF_RING) perf_ring[perf_ring_count++] = interval;
  }
  perf_prev_ns = now;
  if (perf_window_presents >= PERF_REPORT_PRESENTS) perf_report(now);
}
#endif /* NETHERSX2_VK_DIAGNOSTIC */

#ifndef NETHERSX2_VK_DIAGNOSTIC
static VkResult VKAPI_CALL
vkWaitForFences_shim(VkDevice device, uint32_t fence_count,
                     const VkFence *fences, VkBool32 wait_all,
                     uint64_t timeout) {
  if (!real_wait_for_fences) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = real_wait_for_fences(device, fence_count, fences,
                                                wait_all, timeout);
  if (result == VK_SUCCESS && low_latency_pending_fence) {
    for (uint32_t i = 0; i < fence_count; ++i) {
      if (fences[i] == low_latency_pending_fence) {
        const VkFence pending = low_latency_pending_fence;
        if (real_wait_for_fences(device, 1, &pending, VK_TRUE, 0) == VK_SUCCESS)
          low_latency_pending_fence = VK_NULL_HANDLE;
        break;
      }
    }
  }
  return result;
}
#endif

static uint64_t lsfg_observe_source_present(void) {
  const uint64_t now = lsfg_monotonic_ns();
  uint64_t interval = 0;
  if (now && lsfg_last_source_present_ns && now > lsfg_last_source_present_ns) {
    interval = now - lsfg_last_source_present_ns;
    /* Ignore menu stalls, swapchain transitions, and implausibly short bursts. */
    if (interval >= UINT64_C(4000000) && interval <= UINT64_C(100000000)) {
      if (!lsfg_source_samples)
        lsfg_source_interval_ns = (double)interval;
      else
        lsfg_source_interval_ns =
            lsfg_source_interval_ns * 0.875 + (double)interval * 0.125;
      if (lsfg_source_samples < 120) ++lsfg_source_samples;
    } else {
      interval = 0;
    }
  }
  lsfg_last_source_present_ns = now;
  return interval;
}

static int lsfg_source_is_high_rate(void) {
  /* Conservative boundary between 30 FPS and a slightly dipping 50/60 FPS game. */
  return lsfg_source_samples >= 8 &&
         lsfg_source_interval_ns > 0.0 &&
         lsfg_source_interval_ns < 30000000.0;
}

// surface: Android create-info -> NVK VI surface. ci->window is the core's
// ANativeWindow*, which our ANativeWindow_fromSurface_fake made == a libnx
// NWindow*, so it passes straight through to vkCreateViSurfaceNN.
static VkResult VKAPI_CALL
vkCreateAndroidSurfaceKHR_shim(VkInstance inst,
                               const VkAndroidSurfaceCreateInfoKHR *ci,
                               const VkAllocationCallbacks *alloc,
                               VkSurfaceKHR *out) {
  VkViSurfaceCreateInfoNN vi = {
      .sType  = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .pNext  = NULL,
      .flags  = 0,
      .window = (void *)ci->window, // NWindow* -- no conversion
  };
  VkResult r = vkCreateViSurfaceNN(inst, &vi, alloc, out);

#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateViSurfaceNN window=%p result=%d surface=%p",
               vi.window, r, out && r == VK_SUCCESS ? (void *)*out : NULL);
#endif

  return r;
}

// LSFG device features are prepared only for launches enabled in the SDL
// launcher. Runtime frame generation still begins Off and is controlled
// independently by the in-game overlay.
static VkResult VKAPI_CALL
vkCreateDevice_shim(VkPhysicalDevice physical_device,
                    const VkDeviceCreateInfo *create_info,
                    const VkAllocationCallbacks *alloc,
                    VkDevice *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateDevice begin physical=%p queues=%u extensions=%u lsfg_pref=%d",
               (void *)physical_device, create_info->queueCreateInfoCount,
               create_info->enabledExtensionCount, lsfg_session_prepared);
#endif
  PFN_vkCreateDevice create_fn = real_create_device ? real_create_device : vkCreateDevice;
  int prepare_lsfg = lsfg_session_prepared;
  if (prepare_lsfg && !file_readable(lsfg_dll_path())) {
    prepare_lsfg = 0;
    lsfg_session_prepared = 0;
    vk_lsfg_request_enabled(0);
  }
  VkResult result = VK_SUCCESS;
  int augmented = 0;
  int configure_lsfg = prepare_lsfg;
  uint32_t main_family = VK_QUEUE_FAMILY_IGNORED;
  VkResult configuration_error = VK_SUCCESS;

  VkDeviceCreateInfo modified = *create_info;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
      .pNext = (void *)create_info->pNext,
      .timelineSemaphore = VK_TRUE,
  };
  VkPhysicalDeviceTimelineSemaphoreFeatures *existing_timeline = NULL;
  VkPhysicalDeviceVulkan12Features *existing_vulkan12 = NULL;
  VkBool32 old_timeline_value = VK_FALSE;

  const char **extensions = NULL;

  if (configure_lsfg) {
    configuration_error = find_lsfg_main_queue_family(
        physical_device, create_info, &main_family);
    if (configuration_error != VK_SUCCESS) {
      configure_lsfg = 0;
    }
  }

  if (configure_lsfg) {
    for (const VkBaseInStructure *next = create_info->pNext;
         next; next = next->pNext) {
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
        existing_timeline = (VkPhysicalDeviceTimelineSemaphoreFeatures *)(uintptr_t)next;
        old_timeline_value = existing_timeline->timelineSemaphore;
        existing_timeline->timelineSemaphore = VK_TRUE;
        augmented = 1;
        break;
      }
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        existing_vulkan12 = (VkPhysicalDeviceVulkan12Features *)(uintptr_t)next;
        old_timeline_value = existing_vulkan12->timelineSemaphore;
        existing_vulkan12->timelineSemaphore = VK_TRUE;
        augmented = 1;
        break;
      }
    }
    if (!existing_timeline && !existing_vulkan12) {
      modified.pNext = &timeline_feature;
      augmented = 1;
    }

    if (tracked_instance_api < VK_API_VERSION_1_2 &&
        !extension_enabled(create_info, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
      if (!has_device_extension(physical_device,
                                VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) {
        configuration_error = VK_ERROR_EXTENSION_NOT_PRESENT;
        configure_lsfg = 0;
      } else {
        extensions = malloc(sizeof(*extensions) *
                            (create_info->enabledExtensionCount + 1));
        if (!extensions) {
          configuration_error = VK_ERROR_OUT_OF_HOST_MEMORY;
          configure_lsfg = 0;
        } else {
          for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i)
            extensions[i] = create_info->ppEnabledExtensionNames[i];
          extensions[create_info->enabledExtensionCount] =
              VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
          modified.enabledExtensionCount = create_info->enabledExtensionCount + 1;
          modified.ppEnabledExtensionNames = extensions;
        }
      }
    }
  }

  /* A late setup failure must leave the application's original feature chain
   * untouched when LSFG was Off and normal device creation is allowed. */
  if (!configure_lsfg) {
    if (existing_timeline)
      existing_timeline->timelineSemaphore = old_timeline_value;
    if (existing_vulkan12)
      existing_vulkan12->timelineSemaphore = old_timeline_value;
  }

  if (prepare_lsfg && !configure_lsfg) {
    result = configuration_error != VK_SUCCESS ? configuration_error :
        VK_ERROR_FEATURE_NOT_PRESENT;
  } else {
    result = create_fn(physical_device,
        configure_lsfg ? &modified : create_info, alloc, out);
  }

  if (existing_timeline) existing_timeline->timelineSemaphore = old_timeline_value;
  if (existing_vulkan12) existing_vulkan12->timelineSemaphore = old_timeline_value;
  free(extensions);

  lsfg_device_capable = 0;
  if (result == VK_SUCCESS) {
    tracked_physical_device = physical_device;
    tracked_device = *out;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    reset_tracked_swapchain();

    if (configure_lsfg && augmented) {
      lsfg_main_queue_family = main_family;
      lsfg_device_capable = 1;
    }
  }
#ifdef NETHERSX2_VK_DIAGNOSTIC
  /* lsfg_family is LSFG's queue family, not the graphics/present one: it stays
   * VK_QUEUE_FAMILY_IGNORED (prints as 4294967295) whenever lsfg_capable is 0,
   * which is normal here. Read it together with lsfg_capable, not on its own. */
  vk_diag_note("vkCreateDevice end result=%d device=%p lsfg_capable=%d "
               "lsfg_family=%u",
               result, out && result == VK_SUCCESS ? (void *)*out : NULL,
               lsfg_device_capable, lsfg_main_queue_family);
#endif
  return result;
}

static void VKAPI_CALL
vkGetDeviceQueue_shim(VkDevice device, uint32_t queue_family_index,
                      uint32_t queue_index, VkQueue *out) {
  PFN_vkGetDeviceQueue get_fn = real_get_device_queue ?
      real_get_device_queue : vkGetDeviceQueue;
  get_fn(device, queue_family_index, queue_index, out);
  if (out) remember_queue(*out, queue_family_index);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkGetDeviceQueue family=%u index=%u queue=%p",
               queue_family_index, queue_index, out ? (void *)*out : NULL);
#endif
}

static void VKAPI_CALL
vkGetDeviceQueue2_shim(VkDevice device, const VkDeviceQueueInfo2 *queue_info,
                       VkQueue *out) {
  PFN_vkGetDeviceQueue2 get_fn = real_get_device_queue2 ?
      real_get_device_queue2 : vkGetDeviceQueue2;
  get_fn(device, queue_info, out);
  if (queue_info && out) remember_queue(*out, queue_info->queueFamilyIndex);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkGetDeviceQueue2 family=%u index=%u queue=%p",
               queue_info ? queue_info->queueFamilyIndex : UINT32_MAX,
               queue_info ? queue_info->queueIndex : UINT32_MAX,
               out ? (void *)*out : NULL);
#endif
}

static void low_latency_wait_pending_fence(const char *where) {
  if (!vk_low_latency_enabled() || !low_latency_pending_fence ||
      !tracked_device || !real_wait_for_fences)
    return;
  const VkFence fence = low_latency_pending_fence;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t t0 = lsfg_monotonic_ns();
#endif
  const VkResult result = real_wait_for_fences(tracked_device, 1, &fence,
                                                VK_TRUE, UINT64_MAX);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t t1 = lsfg_monotonic_ns();
  const uint64_t waited_ns = t1 > t0 ? t1 - t0 : 0;
  ++vk_diag_sync_fence_waits;
  vk_diag_sync_fence_ns += waited_ns;
  if (waited_ns > vk_diag_sync_fence_max_ns)
    vk_diag_sync_fence_max_ns = waited_ns;
  frame_ll_fence_ns += waited_ns;
  if (result != VK_SUCCESS)
    vk_diag_note("low-latency fence wait failed where=%s result=%d wait_ms=%.1f",
                 where, result, (double)waited_ns / 1e6);
#endif
  if (result == VK_SUCCESS)
    low_latency_pending_fence = VK_NULL_HANDLE;
}

static VkResult VKAPI_CALL
vkQueueSubmit_shim(VkQueue queue, uint32_t submit_count,
                   const VkSubmitInfo *submits, VkFence fence) {
  PFN_vkQueueSubmit submit = real_queue_submit;
  if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t paced_ns = low_latency_pace_before_submit();
  const uint64_t t0 = lsfg_monotonic_ns();
#else
  (void)low_latency_pace_before_submit();
#endif
  const VkResult result = submit(queue, submit_count, submits, fence);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  {
    const uint64_t t1 = lsfg_monotonic_ns();
    if (t1 > t0) { frame_sub_ns += t1 - t0; gpu_note_time(&gpu_sub_calls, &gpu_sub_ns, t0, t1); }
  }
  const unsigned call = ++vk_diag_submit_calls;
  if (result != VK_SUCCESS)
    vk_diag_note("vkQueueSubmit failed call=%u count=%u result=%d pace_ms=%.1f",
                 call, submit_count, result, (double)paced_ns / 1e6);
#endif
  if (vk_low_latency_enabled() && result == VK_SUCCESS && fence)
    low_latency_pending_fence = fence;
  return result;
}

static VkResult VKAPI_CALL
vkQueueSubmit2_shim(VkQueue queue, uint32_t submit_count,
                    const VkSubmitInfo2 *submits, VkFence fence) {
  PFN_vkQueueSubmit2 submit = real_queue_submit2;
  if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t paced_ns = low_latency_pace_before_submit();
  const uint64_t t0 = lsfg_monotonic_ns();
#else
  (void)low_latency_pace_before_submit();
#endif
  const VkResult result = submit(queue, submit_count, submits, fence);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  {
    const uint64_t t1 = lsfg_monotonic_ns();
    if (t1 > t0) { frame_sub_ns += t1 - t0; gpu_note_time(&gpu_sub_calls, &gpu_sub_ns, t0, t1); }
  }
  const unsigned call = ++vk_diag_submit2_calls;
  if (result != VK_SUCCESS)
    vk_diag_note("vkQueueSubmit2 failed call=%u count=%u result=%d pace_ms=%.1f",
                 call, submit_count, result, (double)paced_ns / 1e6);
#endif
  if (vk_low_latency_enabled() && result == VK_SUCCESS && fence)
    low_latency_pending_fence = fence;
  return result;
}

static VkResult VKAPI_CALL
vkAcquireNextImageKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                           uint64_t timeout, VkSemaphore semaphore,
                           VkFence fence, uint32_t *image_index) {
  PFN_vkAcquireNextImageKHR acquire = real_acquire_next_image;
  if (!acquire) return VK_ERROR_INITIALIZATION_FAILED;
  /* Preserve the application's timeout contract. In particular, timeout=0 is
   * an explicit poll: don't wait for the previous frame's fence or turn
   * VK_NOT_READY into a 50 ms retry loop. The two-image low-latency path plus
   * next-frame pacing bounds queue depth without blocking a nonblocking acquire. */
  if (timeout != 0)
    low_latency_wait_pending_fence("before-acquire");
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t t0 = lsfg_monotonic_ns();
#endif
  const VkResult result = acquire(device, swapchain, timeout, semaphore, fence,
                                  image_index);
#ifdef NETHERSX2_VK_DIAGNOSTIC
  {
    const uint64_t t1 = lsfg_monotonic_ns();
    if (t1 > t0) { frame_acq_ns += t1 - t0; gpu_note_time(&gpu_acq_calls, &gpu_acq_ns, t0, t1); }
  }
  const unsigned call = ++vk_diag_acquire_calls;
  if (call <= 4 || (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR))
    vk_diag_note("vkAcquireNextImageKHR call=%u result=%d image=%u elapsed_ms=%.3f semaphore=%p fence=%p",
                 call, result,
                 image_index && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ?
                     *image_index : UINT32_MAX,
                 (double)(lsfg_monotonic_ns() - t0) / 1e6,
                 (void *)semaphore, (void *)fence);
#endif
  return result;
}

__attribute__((used)) static const PFN_vkQueueSubmit low_latency_sig_submit = vkQueueSubmit_shim;
__attribute__((used)) static const PFN_vkQueueSubmit2 low_latency_sig_submit2 = vkQueueSubmit2_shim;
__attribute__((used)) static const PFN_vkAcquireNextImageKHR low_latency_sig_acquire = vkAcquireNextImageKHR_shim;

#ifdef NETHERSX2_VK_DIAGNOSTIC
static VkResult VKAPI_CALL
vkGetSwapchainImagesKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                             uint32_t *count, VkImage *images) {
  PFN_vkGetSwapchainImagesKHR get_images = real_get_swapchain_images ?
      real_get_swapchain_images : vkGetSwapchainImagesKHR;
  VkResult result = get_images(device, swapchain, count, images);
  const unsigned call = ++vk_diag_swapchain_image_calls;
  if (call <= 4 || (result != VK_SUCCESS && result != VK_INCOMPLETE))
    vk_diag_note("vkGetSwapchainImagesKHR call=%u fill=%d result=%d count=%u",
                 call, images != NULL, result, count ? *count : 0);
  return result;
}
#endif

#ifdef NETHERSX2_VK_DIAGNOSTIC
static const char *vk_present_mode_name(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    default: return "OTHER";
  }
}
#endif

static int vk_low_latency_select_mode(VkSurfaceKHR surface,
                                      VkPresentModeKHR *out_mode) {
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_modes = tracked_instance ?
      (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(
          tracked_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR") : NULL;
  if (!get_modes)
    return 0;
  uint32_t count = 0;
  if (get_modes(tracked_physical_device, surface, &count, NULL) != VK_SUCCESS ||
      count == 0)
    return 0;
  VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
  if (!modes) return 0;
  const VkResult result = get_modes(tracked_physical_device, surface, &count, modes);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    free(modes);
    return 0;
  }
  const VkPresentModeKHR preference[] = {
    VK_PRESENT_MODE_IMMEDIATE_KHR,
    VK_PRESENT_MODE_FIFO_KHR,
  };
  int found = 0;
  for (unsigned i = 0; i < sizeof(preference) / sizeof(preference[0]); ++i) {
    for (uint32_t j = 0; j < count; ++j) {
      if (modes[j] == preference[i]) {
        *out_mode = preference[i];
        found = 1;
        break;
      }
    }
    if (found) break;
  }
  free(modes);
  return found;
}

static VkResult VKAPI_CALL
vkCreateSwapchainKHR_shim(VkDevice device,
                          const VkSwapchainCreateInfoKHR *create_info,
                          const VkAllocationCallbacks *alloc,
                          VkSwapchainKHR *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateSwapchainKHR begin old=%p extent=%ux%u format=%u usage=0x%x requested_min_images=%u mode=%u",
               (void *)create_info->oldSwapchain,
               create_info->imageExtent.width, create_info->imageExtent.height,
               create_info->imageFormat, create_info->imageUsage,
               create_info->minImageCount, create_info->presentMode);
#endif
  PFN_vkCreateSwapchainKHR create_fn = real_create_swapchain ?
      real_create_swapchain : vkCreateSwapchainKHR;

  if (create_info->oldSwapchain == tracked_swapchain) {
    low_latency_wait_pending_fence("before-swapchain-recreate");
    reset_tracked_swapchain();
  }

  /* SaltyNX may still be bringing its Core up during early wrapper startup.
   * The frontend's first nonblocking SaltySD attempt happens before the VM and
   * surface are initialized; retry once here, immediately before choosing the
   * swapchain mode. The client throttles concurrent callers to one attempt per
   * second and this does not sleep or use SaltySD_Connect's blocking retry. */
  if (!create_info->oldSwapchain)
    saltysd_display_sync_poll();

  VkSwapchainCreateInfoKHR modified = *create_info;
  /* If FPSLocker has selected the active panel cadence, present through FIFO
   * rather than inheriting the frontend's IMMEDIATE mode. A matching emulator
   * limiter alone does not phase-lock presents to vblank; FIFO does. This only
   * changes the WSI presentation mode, not the emulator's frame-rate limit. */
  const int display_sync_mode_override = saltysd_display_sync_active();
  if (display_sync_mode_override)
    modified.presentMode = VK_PRESENT_MODE_FIFO_KHR;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("display-sync swapchain query=%d active=%d connect_rc=0x%08x "
               "nethfps_rc=0x%08x legacy_rc=0x%08x port=%u service_rate=%u "
               "effective_rate=%.2f",
               saltysd_display_sync_query_status(),
               saltysd_display_sync_active() ? 1 : 0,
               saltysd_display_sync_connect_result(),
               saltysd_display_sync_nethfps_connect_result(),
               saltysd_display_sync_legacy_connect_result(),
               saltysd_display_sync_connect_port(),
               saltysd_display_sync_service_rate(),
               saltysd_display_sync_rate());
#endif
  int compatible = 0;
  const int low_latency = vk_low_latency_enabled();
  const int requested = lsfg_requested();
  const int want_lsfg = lsfg_device_capable;
  const char *dll_path = lsfg_dll_path();
  const int have_dll = want_lsfg && file_readable(dll_path);
  const unsigned fpslocker_buffer_request =
      saltysd_fpslocker_buffers_request();
  int fpslocker_buffer_override = 0;

  /* FPSLocker owns the per-title SetBuffers request. Apply it only at Vulkan
   * swapchain creation, after clamping to this surface's supported range.
   * NetherSX2 low-latency mode deliberately takes precedence; LSFG headroom is
   * added below on top of the selected base count. */
  if (fpslocker_buffer_request && !low_latency && tracked_physical_device) {
    VkSurfaceCapabilitiesKHR buffer_capabilities;
    const VkResult buffer_cap_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        tracked_physical_device, create_info->surface, &buffer_capabilities);
    if (buffer_cap_result == VK_SUCCESS) {
      uint32_t image_count = fpslocker_buffer_request;
      if (image_count < buffer_capabilities.minImageCount)
        image_count = buffer_capabilities.minImageCount;
      if (buffer_capabilities.maxImageCount &&
          image_count > buffer_capabilities.maxImageCount)
        image_count = buffer_capabilities.maxImageCount;
      modified.minImageCount = image_count;
      fpslocker_buffer_override = 1;
    }
  }

  if (requested && !low_latency && (!want_lsfg || !have_dll))
    return VK_ERROR_FEATURE_NOT_PRESENT;

  if (!low_latency && have_dll) {
    VkSurfaceCapabilitiesKHR capabilities;
    VkFormatProperties swapchain_properties;
    VkFormatProperties rgba_properties;
    VkResult cap_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        tracked_physical_device, create_info->surface, &capabilities);
    vkGetPhysicalDeviceFormatProperties(tracked_physical_device,
        create_info->imageFormat, &swapchain_properties);
    vkGetPhysicalDeviceFormatProperties(tracked_physical_device,
        VK_FORMAT_R8G8B8A8_UNORM, &rgba_properties);
    const VkImageUsageFlags transfer_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags copy_features =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const int queues_valid =
        lsfg_main_queue_family != VK_QUEUE_FAMILY_IGNORED;
    if (cap_result == VK_SUCCESS && queues_valid &&
        (capabilities.supportedUsageFlags & transfer_usage) == transfer_usage &&
        (swapchain_properties.optimalTilingFeatures & copy_features) == copy_features &&
        (rgba_properties.optimalTilingFeatures & copy_features) == copy_features) {
      modified.imageUsage |= transfer_usage;
      // Give the doubled FIFO stream all image headroom exposed by VI.
      uint32_t desired = modified.minImageCount + 2;
      if (capabilities.maxImageCount && desired > capabilities.maxImageCount)
        desired = capabilities.maxImageCount;
      if (desired > modified.minImageCount) modified.minImageCount = desired;
      modified.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      compatible = 1;
    }
  }

  if (requested && !low_latency && want_lsfg && have_dll && !compatible)
    return VK_ERROR_FORMAT_NOT_SUPPORTED;

  if (low_latency) {
    VkSurfaceCapabilitiesKHR capabilities;
    const VkResult cap_result = tracked_physical_device ?
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(tracked_physical_device,
            create_info->surface, &capabilities) : VK_ERROR_INITIALIZATION_FAILED;
    VkPresentModeKHR selected_mode = create_info->presentMode;
    const int mode_found = tracked_physical_device &&
        vk_low_latency_select_mode(create_info->surface, &selected_mode);
    if (cap_result == VK_SUCCESS) {
      uint32_t image_count = 2;
      if (image_count < capabilities.minImageCount)
        image_count = capabilities.minImageCount;
      if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;
      modified.minImageCount = image_count;
    }
    if (mode_found)
      modified.presentMode = selected_mode;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    vk_diag_note("swapchain low_latency=%d min_images=%u mode=%s caps=%d modes=%d min=%u max=%u",
                 low_latency, modified.minImageCount,
                 vk_present_mode_name(modified.presentMode), cap_result,
                 mode_found, cap_result == VK_SUCCESS ? capabilities.minImageCount : 0,
                 cap_result == VK_SUCCESS ? capabilities.maxImageCount : 0);
#endif
  }

  /* Low latency takes priority over FPSLocker buffering and the LSFG FIFO
   * path. Otherwise FPSLocker's requested base count is honored, with LSFG
   * adding its own image headroom when the surface supports it. */
  const VkSwapchainCreateInfoKHR *selected_create_info =
      (low_latency || compatible || fpslocker_buffer_override ||
       display_sync_mode_override) ? &modified : create_info;
  VkResult result = create_fn(device, selected_create_info, alloc, out);
  uint32_t actual_image_count = 0;
  VkResult actual_image_count_result = VK_ERROR_INITIALIZATION_FAILED;
  if (result == VK_SUCCESS && out && *out) {
    PFN_vkGetSwapchainImagesKHR get_images = real_get_swapchain_images ?
        real_get_swapchain_images : vkGetSwapchainImagesKHR;
    if (get_images) {
      actual_image_count_result = get_images(device, *out,
                                             &actual_image_count, NULL);
      if ((actual_image_count_result == VK_SUCCESS ||
           actual_image_count_result == VK_INCOMPLETE) && actual_image_count)
        saltysd_fpslocker_buffers_report_actual(actual_image_count);
    }
    reset_tracked_swapchain();
    tracked_swapchain = *out;
    tracked_swapchain_extent = create_info->imageExtent;
    tracked_swapchain_lsfg_compatible = compatible;
    __atomic_store_n(&lsfg_runtime_available, compatible, __ATOMIC_RELEASE);
  }
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateSwapchainKHR end result=%d swapchain=%p compatible=%d buffer_request=%u buffer_override=%d requested_min_images=%u actual_image_query=%d actual_images=%u selected_mode=%u",
               result, out && result == VK_SUCCESS ? (void *)*out : NULL,
               compatible, fpslocker_buffer_request, fpslocker_buffer_override,
               selected_create_info->minImageCount, actual_image_count_result,
               actual_image_count, selected_create_info->presentMode);
#endif
  return result;
}

static void VKAPI_CALL
vkDestroySwapchainKHR_shim(VkDevice device, VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *alloc) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkDestroySwapchainKHR swapchain=%p", (void *)swapchain);
#endif
  if (swapchain == tracked_swapchain) reset_tracked_swapchain();
  PFN_vkDestroySwapchainKHR destroy_fn = real_destroy_swapchain ?
      real_destroy_swapchain : vkDestroySwapchainKHR;
  destroy_fn(device, swapchain, alloc);
}

static void VKAPI_CALL
vkDestroyDevice_shim(VkDevice device, const VkAllocationCallbacks *alloc) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkDestroyDevice device=%p", (void *)device);
#endif
  if (device == tracked_device) {
    low_latency_wait_pending_fence("device-destroy");
    low_latency_pending_fence = VK_NULL_HANDLE;
    low_latency_next_vblank_ns = 0;
    reset_tracked_swapchain();
    tracked_device = VK_NULL_HANDLE;
    tracked_physical_device = VK_NULL_HANDLE;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    lsfg_device_capable = 0;
  }
  PFN_vkDestroyDevice destroy_fn = real_destroy_device ?
      real_destroy_device : vkDestroyDevice;
  destroy_fn(device, alloc);
}

// present counter (main.c reads vk_present_count).
static PFN_vkQueuePresentKHR real_qpresent = NULL;

static VkResult
vkQueuePresentKHR_native(VkQueue queue, const VkPresentInfoKHR *present_info) {
  PFN_vkQueuePresentKHR present = real_qpresent ? real_qpresent : vkQueuePresentKHR;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  const uint64_t t0 = lsfg_monotonic_ns();
#endif
  VkResult result = present(queue, present_info);
  /* Timestamp the completed present call rather than source-frame entry. */
  saltysd_display_sync_note_present();
  if (vk_low_latency_enabled())
    low_latency_next_vblank_ns = lsfg_monotonic_ns() + low_latency_period_ns();
#ifdef NETHERSX2_VK_DIAGNOSTIC
  {
    const uint64_t t1 = lsfg_monotonic_ns();
    if (t1 > t0) {
      frame_pres_ns += t1 - t0;
      gpu_note_time(&gpu_pres_calls, &gpu_pres_ns, t0, t1);
    }
  }
  if (vk_present_count <= 4 || result != VK_SUCCESS)
    vk_diag_note("vkQueuePresentKHR call=%d swaps=%u result=%d",
                 vk_present_count,
                 present_info ? present_info->swapchainCount : 0, result);
#endif
  return result;
}

static int lsfg_try_create(VkQueue queue) {
  if (lsfg_runtime) return 1;
  if (lsfg_init_attempted || !tracked_swapchain_lsfg_compatible ||
      !tracked_device || !tracked_physical_device || !tracked_instance)
    return 0;

  lsfg_init_attempted = 1;
  uint32_t queue_family = 0;
  if (!find_queue_family(queue, &queue_family)) return 0;
  if (queue_family != lsfg_main_queue_family) return 0;

  PFN_vkGetSwapchainImagesKHR get_images = real_get_swapchain_images ?
      real_get_swapchain_images : vkGetSwapchainImagesKHR;
  uint32_t image_count = 0;
  VkResult result = get_images(tracked_device, tracked_swapchain,
                               &image_count, NULL);
  if (result != VK_SUCCESS || image_count < 3) return 0;

  VkImage *images = malloc(sizeof(*images) * image_count);
  if (!images) return 0;
  result = get_images(tracked_device, tracked_swapchain, &image_count, images);
  if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
    free(images);
    return 0;
  }

  float flow_scale = prefs_get_float("Wrapper/LSFGFlowScale", 0.25f);
  if (flow_scale != 0.125f && flow_scale != 0.25f && flow_scale != 0.5f)
    flow_scale = 0.25f;

  LsfgNxCreateInfo info = {
      .instance = tracked_instance,
      .physical_device = tracked_physical_device,
      .device = tracked_device,
      .queue = queue,
      .queue_family_index = queue_family,
      .get_instance_proc_addr = vkGetInstanceProcAddr,
      .swapchain = tracked_swapchain,
      .extent = tracked_swapchain_extent,
      .swapchain_images = images,
      .swapchain_image_count = image_count,
      .shader_dll_path = lsfg_dll_path(),
      .flow_scale = flow_scale,
      .performance_mode = prefs_get_bool("Wrapper/LSFGPerformance", true),
  };
  lsfg_runtime = lsfg_nx_create(&info);
  free(images);

  return lsfg_runtime != NULL;
}

static VkResult VKAPI_CALL
vkQueuePresentKHR_shim(VkQueue q, const VkPresentInfoKHR *pi) {
  ++vk_present_count;
#ifdef NETHERSX2_VK_DIAGNOSTIC
  /* Every present, whatever the path below decides to do with it. */
  perf_observe_present();
#endif
#ifdef NETHERSX2_VK_DIAGNOSTIC
  if (vk_present_count <= 4)
    vk_diag_note("vkQueuePresentKHR enter call=%d waits=%u swaps=%u lsfg=%d",
                 vk_present_count, pi ? pi->waitSemaphoreCount : 0,
                 pi ? pi->swapchainCount : 0, lsfg_requested());
#endif
  if (vk_low_latency_enabled())
    return vkQueuePresentKHR_native(q, pi);
  const int requested = lsfg_requested();
  uint64_t source_interval = 0;

  const int fractional = requested && lsfg_mode_is_fractional();
  const double target_fps = requested ? lsfg_target_fps_pref() : 0.0;

  /* FIFO back-pressure invalidates source timing after generation starts: the
   * display saturates, the second present blocks, and the next source present
   * arrives late, so the interval reads as a slower source. In fractional mode
   * that would be positive feedback -- slower reading, higher ratio, more
   * generation -- so the estimate is taken from capture-only presents (which
   * are true one-in-one-out source presents) and then frozen. */
  if (fractional) {
    if (!lsfg_fractional_ready) {
      source_interval = lsfg_observe_source_present();
      /* A menu or a loading screen sits at the display cap, and the estimate is
       * an EWMA carrying 0.875 of its past, so history taken there poisons the
       * latch: 16 samples into a 43 fps race it still reads ~46 out of 50, the
       * ratio comes out 1.08 and generation nearly stops. That history has to be
       * thrown away -- but only on a SUSTAINED run at or above the cap. A single
       * 50 fps frame in the middle of a race is ordinary jitter, and resetting on
       * it would mean a game that alternates 43 and 50 fps never latches at all.
       * Eight in a row is a menu; one is noise. */
      if (source_interval > 0) {
        const double raw_fps = 1e9 / (double)source_interval;
        if (raw_fps >= target_fps) {
          /* Published straight away: while at the cap nothing is generated, so
           * there is no FIFO back-pressure to distort it, and the quick menu and
           * the perf line can still say what the source is doing. The EWMA needs
           * 8 samples to report anything and the reset below keeps it below that,
           * so the raw interval is the only honest value available here. */
          lsfg_frozen_source_fps = raw_fps;
          if (++lsfg_at_cap_run >= 8) {
            lsfg_source_samples = 0;
            lsfg_source_interval_ns = 0.0;
          }
        } else {
          lsfg_at_cap_run = 0;
        }
      }
      /* Latch only once the measured source is genuinely BELOW the target.
       * Latching on a rate at or above it -- which is what a menu produces --
       * freezes the estimate at the cap, the ratio comes out exactly 1.0 and
       * fractional mode then never earns a single generated frame for the rest
       * of the session, because the estimate is by design never revisited.
       *
       * GT3 does precisely this: 50.17 fps in the menus and 42.9-43.2 fps in a
       * race, so enabling frame generation anywhere outside a race locked the
       * output to the race rate with generation permanently off. Deus Ex sits at
       * 30 fps everywhere, which is why the same build appeared to work there
       * and not here -- it was never the game, it was what the source was doing
       * during the 16 warm-up samples.
       *
       * Measuring on is safe while nothing is generated: the back-pressure
       * described above only appears once interpolation actually starts, so
       * there is no feedback loop left to run away with. */
      if (lsfg_source_samples >= 16) {
        const double measured = lsfg_measured_source_fps();
        if (measured > 0.0 && measured < target_fps) {
          lsfg_frozen_source_fps = measured;
          lsfg_fractional_ready = 1;
          lsfg_gen_phase = 0.0;
        }
      }
    }
  } else if (!requested || lsfg_rate_decision != 0) {
    source_interval = lsfg_observe_source_present();
  }

  if (requested != lsfg_previous_requested) {
    /* A quick-menu toggle is not a swapchain transition. Clear all rate and
     * fractional-scheduler state here as well, so re-enabling LSFG cannot reuse
     * a source-rate latch from the previous session/scene. */
    lsfg_last_source_present_ns = 0;
    lsfg_source_interval_ns = 0.0;
    lsfg_source_samples = 0;
    lsfg_at_cap_run = 0;
    lsfg_high_fps_slow_samples = 0;
    lsfg_fractional_ready = 0;
    lsfg_frozen_source_fps = 0.0;
    lsfg_gen_phase = 0.0;
    lsfg_native_gap = 0;
    lsfg_init_attempted = 0;
    lsfg_rate_decision = -1;
    lsfg_previous_requested = requested;
  }

  if (!requested && lsfg_runtime) {
    lsfg_destroy_runtime();
    lsfg_init_attempted = 0;
  }

  if (requested && lsfg_rate_decision < 0 && lsfg_source_samples >= 8)
    lsfg_rate_decision = lsfg_source_is_high_rate() ? 1 : 0;

  /* Fractional mode ignores the 33 FPS gate entirely: the schedule decides per
   * present how much generation the measured source rate can absorb, so a fast
   * source interpolates less often instead of being passed through wholesale. */
  if (!fractional && requested && lsfg_rate_decision == 1) {
    /* Engage LSFG after a sustained transition from 50/60 to 25/30 FPS. */
    if (source_interval >= UINT64_C(31500000))
      ++lsfg_high_fps_slow_samples;
    else if (source_interval)
      lsfg_high_fps_slow_samples = 0;
    if (lsfg_high_fps_slow_samples < 16)
      return vkQueuePresentKHR_native(q, pi);
    lsfg_rate_decision = 0;
    lsfg_high_fps_slow_samples = 0;
  }

  /* Keep native presentation active until source-rate classification completes.
   * Fractional mode has no such wait: it routes every present through the bridge
   * from the start, capture-only while measuring, so the ping-pong source slot
   * is already warm when the first interpolation is earned. */
  if (!fractional && requested && lsfg_rate_decision < 0) {
    if (requested) lsfg_native_gap = 1;
    return vkQueuePresentKHR_native(q, pi);
  }

  if (pi && pi->swapchainCount == 1 && pi->pSwapchains &&
      pi->pSwapchains[0] == tracked_swapchain &&
      requested) {
    if (!tracked_swapchain_lsfg_compatible || !lsfg_try_create(q))
      return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    const int generate = !fractional ||
        lsfg_fractional_schedule(lsfg_frozen_source_fps, target_fps,
                                 &lsfg_gen_phase, &lsfg_native_gap);
    bool generated_frame = false;
    const int consumed = generate
        ? lsfg_nx_present(lsfg_runtime, q, pi, &result, &generated_frame)
        : lsfg_nx_present_capture(lsfg_runtime, q, pi, &result);
#ifdef NETHERSX2_VK_DIAGNOSTIC
    if (generated_frame) ++perf_generated;
#endif
    if (consumed) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
      if (vk_present_count <= 4 || result != VK_SUCCESS)
        vk_diag_note("lsfg present call=%d result=%d generate=%d src=%.0ffps target=%.0ffps",
                     vk_present_count, result, generate, lsfg_frozen_source_fps,
                     target_fps);
#endif
      return result;
    }
#ifdef NETHERSX2_VK_DIAGNOSTIC
    vk_diag_note("lsfg present call=%d failed before queue", vk_present_count);
#endif
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (requested) lsfg_native_gap = 1;
  return vkQueuePresentKHR_native(q, pi);
}

// Memory-budget override: expose at least the full heap to clients. In
// diagnostic builds, capture the actual NVK heap/budget and the post-clamp
// budget once; do not duplicate NVK's heap or budget calculations here.
static void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2_shim(VkPhysicalDevice pd,
                                          VkPhysicalDeviceMemoryProperties2 *props) {
  if (!real_memprops2) return;
  VkPhysicalDevice expected_pd = VK_NULL_HANDLE;
  __atomic_compare_exchange_n(&real_memprops2_pd, &expected_pd, pd, 0,
                              __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  real_memprops2(pd, props);
  uint32_t heaps = props->memoryProperties.memoryHeapCount;
  for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
    if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
      continue;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = (void *)p;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    const VkDeviceSize nvk_budget0 = heaps ? b->heapBudget[0] : 0;
#endif
    for (uint32_t i = 0; i < heaps && i < VK_MAX_MEMORY_HEAPS; i++) {
      VkDeviceSize heap = props->memoryProperties.memoryHeaps[i].size;
      if (b->heapBudget[i] < heap) b->heapBudget[i] = heap;
    }
#ifdef NETHERSX2_VK_DIAGNOSTIC
    static Mutex log_once_mutex;
    static int logged;
    mutexLock(&log_once_mutex);
    const int log_this_call = !logged;
    logged = 1;
    mutexUnlock(&log_once_mutex);
    if (log_this_call) {
      const VkDeviceSize heap0 = heaps ? props->memoryProperties.memoryHeaps[0].size : 0;
      const VkDeviceSize shim_budget0 = heaps ? b->heapBudget[0] : 0;
      vk_diag_note("vkGetPhysicalDeviceMemoryProperties2: heaps=%u "
                   "heap0=%llu bytes (%llu MiB) NVK_budget0=%llu bytes "
                   "(%llu MiB) shim_budget0=%llu bytes (%llu MiB)",
                   heaps,
                   (unsigned long long)heap0,
                   (unsigned long long)(heap0 >> 20),
                   (unsigned long long)nvk_budget0,
                   (unsigned long long)(nvk_budget0 >> 20),
                   (unsigned long long)shim_budget0,
                   (unsigned long long)(shim_budget0 >> 20));
    }
#endif
  }
}

// device proc addr wrapper: swap in our present shim; forward everything else to
// NVK's real GDPA unchanged.
static PFN_vkVoidFunction VKAPI_CALL
vk_gdpa_hook(VkDevice dev, const char *name) {
  PFN_vkVoidFunction fn = vkGetDeviceProcAddr(dev, name);
  if (!name) return fn;
  if (!strcmp(name, "vkWaitForFences")) {
    real_wait_for_fences = (PFN_vkWaitForFences)fn;
    return fn ? (PFN_vkVoidFunction)vkWaitForFences_shim : NULL;
  }
  if (!strcmp(name, "vkQueueSubmit")) {
    real_queue_submit = (PFN_vkQueueSubmit)fn;
    return fn ? (PFN_vkVoidFunction)vkQueueSubmit_shim : NULL;
  }
  if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
    real_queue_submit2 = (PFN_vkQueueSubmit2)fn;
    return fn ? (PFN_vkVoidFunction)vkQueueSubmit2_shim : NULL;
  }
  if (!strcmp(name, "vkAcquireNextImageKHR")) {
    real_acquire_next_image = (PFN_vkAcquireNextImageKHR)fn;
    return fn ? (PFN_vkVoidFunction)vkAcquireNextImageKHR_shim : NULL;
  }
#ifdef NETHERSX2_VK_DIAGNOSTIC
  {
    PFN_vkVoidFunction probe = gpu_probe_lookup(name, fn);
    if (probe) return probe;
  }
#endif
  if (!strcmp(name, "vkGetDeviceQueue")) {
    real_get_device_queue = (PFN_vkGetDeviceQueue)fn;
    return (PFN_vkVoidFunction)vkGetDeviceQueue_shim;
  }
  if (!strcmp(name, "vkGetDeviceQueue2")) {
    real_get_device_queue2 = (PFN_vkGetDeviceQueue2)fn;
    return (PFN_vkVoidFunction)vkGetDeviceQueue2_shim;
  }
  if (!strcmp(name, "vkCreateSwapchainKHR")) {
    real_create_swapchain = (PFN_vkCreateSwapchainKHR)fn;
    return (PFN_vkVoidFunction)vkCreateSwapchainKHR_shim;
  }
  if (!strcmp(name, "vkGetSwapchainImagesKHR")) {
    real_get_swapchain_images = (PFN_vkGetSwapchainImagesKHR)fn;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR_shim;
#else
    return fn;
#endif
  }
  if (!strcmp(name, "vkDestroySwapchainKHR")) {
    real_destroy_swapchain = (PFN_vkDestroySwapchainKHR)fn;
    return (PFN_vkVoidFunction)vkDestroySwapchainKHR_shim;
  }
  if (!strcmp(name, "vkDestroyDevice")) {
    real_destroy_device = (PFN_vkDestroyDevice)fn;
    return (PFN_vkVoidFunction)vkDestroyDevice_shim;
  }
  if (!strcmp(name, "vkQueuePresentKHR")) {
    real_qpresent = (PFN_vkQueuePresentKHR)fn;
    return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
  }
  return fn;
}

// Loader parity for the bootstrap set.
//
// A real Vulkan loader answers vkEnumeratePhysicalDevices, vkDestroyInstance and
// vkEnumeratePhysicalDeviceGroups from vkGetInstanceProcAddr(NULL, name): both
// the Khronos loader and Android's libvulkan carry a trampoline for each in
// their global command table, so an application that resolves its whole
// bootstrap set against a NULL instance -- which is what the core does -- gets a
// usable pointer and only later passes the real handle in.
//
// There is no loader here. vkGetInstanceProcAddr forwards straight to NVK's
// vk_icdGetInstanceProcAddr, which is an ICD entry point and therefore answers
// only the four genuinely global commands for a NULL instance; for everything
// else it returns NULL. The core treats NULL as fatal, so GS initialisation
// dies before vkCreateDevice is ever reached. That is the gap in the diagnostic
// log: vkCreateInstance succeeds, and then nothing.
//
// These trampolines restore the loader's contract. They resolve lazily, at call
// time, through the instance recorded by vkCreateInstance_hook -- by then it
// exists, exactly as it does when a loader trampoline runs.

static PFN_vkEnumeratePhysicalDevices real_enumerate_physical_devices = NULL;

static VkResult VKAPI_CALL
vkEnumeratePhysicalDevices_trampoline(VkInstance instance, uint32_t *count,
                                      VkPhysicalDevice *gpus) {
  VkInstance inst = instance ? instance : tracked_instance;
  PFN_vkEnumeratePhysicalDevices fn = real_enumerate_physical_devices;
  if (!fn) {
    fn = inst ? (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
                    inst, "vkEnumeratePhysicalDevices")
              : NULL;
    if (!fn)
      return VK_ERROR_INITIALIZATION_FAILED;
    real_enumerate_physical_devices = fn;
  }

  const VkResult r = fn(inst, count, gpus);

#ifdef NETHERSX2_VK_DIAGNOSTIC
  // "Did we get any adapter at all, and what does NVK call it?" Zero devices is
  // a silent GS init failure on the core side and nothing else in the log
  // distinguishes it from a crash during device creation.
  if (count) {
    vk_diag_note("vkEnumeratePhysicalDevices inst=%p result=%d count=%u%s",
                 (void *)inst, r, *count, gpus ? "" : " (count query)");
    if (gpus && r == VK_SUCCESS) {
      const PFN_vkGetPhysicalDeviceProperties gpp =
          inst ? (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(
                     inst, "vkGetPhysicalDeviceProperties")
               : NULL;
      const uint32_t shown = *count < 8 ? *count : 8;
      for (uint32_t i = 0; i < shown; i++) {
        if (gpp) {
          VkPhysicalDeviceProperties p;
          memset(&p, 0, sizeof(p));
          gpp(gpus[i], &p);
          vk_diag_note("  gpu[%u]=%s api=%u driver=0x%x type=%d", i,
                       p.deviceName, p.apiVersion, p.driverVersion,
                       (int)p.deviceType);
        } else {
          vk_diag_note("  gpu[%u]=%p (no vkGetPhysicalDeviceProperties)", i,
                       (void *)gpus[i]);
        }
      }
      if (*count > shown)
        vk_diag_note("  ... %u further gpu(s)", *count - shown);
    }
  }
#endif
  return r;
}

static void VKAPI_CALL
vkDestroyInstance_trampoline(VkInstance instance,
                             const VkAllocationCallbacks *alloc) {
  VkInstance inst = instance ? instance : tracked_instance;
  PFN_vkDestroyInstance fn = inst
      ? (PFN_vkDestroyInstance)vkGetInstanceProcAddr(inst, "vkDestroyInstance")
      : NULL;
  if (fn)
    fn(inst, alloc);
}

static VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroups_trampoline(VkInstance instance, uint32_t *count,
                                           VkPhysicalDeviceGroupProperties *groups) {
  VkInstance inst = instance ? instance : tracked_instance;
  PFN_vkEnumeratePhysicalDeviceGroups fn = inst
      ? (PFN_vkEnumeratePhysicalDeviceGroups)vkGetInstanceProcAddr(
            inst, "vkEnumeratePhysicalDeviceGroups")
      : NULL;
  if (!fn)
    return VK_ERROR_INITIALIZATION_FAILED;
  return fn(inst, count, groups);
}

// The names an ICD's GIPA declines for a NULL instance but a loader accepts.
// vkEnumeratePhysicalDevices is not here: vk_gipa_hook installs its trampoline
// directly so that it is wrapped whether the core passes NULL or the instance.
static PFN_vkVoidFunction
vk_loader_global_command(const char *name) {
  if (!name)
    return NULL;
  if (!strcmp(name, "vkDestroyInstance"))
    return (PFN_vkVoidFunction)vkDestroyInstance_trampoline;
  if (!strcmp(name, "vkEnumeratePhysicalDeviceGroups") ||
      !strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
    return (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroups_trampoline;
  return NULL;
}

// instance proc addr wrapper -- registered in the import table as
// "vkGetInstanceProcAddr". Routes the Android surface, device-proc-addr, present
// and the memory-budget override; everything else is NVK's real GIPA.
PFN_vkVoidFunction VKAPI_CALL
vk_gipa_hook(VkInstance inst, const char *name) {
  if (name) {
    if (!strcmp(name, "vkCreateInstance"))
      return (PFN_vkVoidFunction)vkCreateInstance_hook;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
      return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties_hook;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) {
      // May be asked with a NULL instance; the trampoline resolves at call time.
      real_enumerate_physical_devices =
          (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices_trampoline;
    }
    if (!strcmp(name, "vkCreateAndroidSurfaceKHR"))
      return (PFN_vkVoidFunction)vkCreateAndroidSurfaceKHR_shim;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)vk_gdpa_hook;
    if (!strcmp(name, "vkWaitForFences")) {
      real_wait_for_fences = (PFN_vkWaitForFences)vkGetInstanceProcAddr(inst, name);
      return real_wait_for_fences ? (PFN_vkVoidFunction)vkWaitForFences_shim : NULL;
    }
    if (!strcmp(name, "vkQueueSubmit")) {
      real_queue_submit = (PFN_vkQueueSubmit)vkGetInstanceProcAddr(inst, name);
      return real_queue_submit ? (PFN_vkVoidFunction)vkQueueSubmit_shim : NULL;
    }
    if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
      real_queue_submit2 = (PFN_vkQueueSubmit2)vkGetInstanceProcAddr(inst, name);
      return real_queue_submit2 ? (PFN_vkVoidFunction)vkQueueSubmit2_shim : NULL;
    }
    if (!strcmp(name, "vkAcquireNextImageKHR")) {
      real_acquire_next_image =
          (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(inst, name);
      return real_acquire_next_image ?
          (PFN_vkVoidFunction)vkAcquireNextImageKHR_shim : NULL;
    }
#ifdef NETHERSX2_VK_DIAGNOSTIC
    {
      PFN_vkVoidFunction probe =
          gpu_probe_lookup(name, vkGetInstanceProcAddr(inst, name));
      if (probe) return probe;
    }
#endif
    if (!strcmp(name, "vkCreateDevice")) {
      real_create_device = (PFN_vkCreateDevice)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkCreateDevice_shim;
    }
    if (!strcmp(name, "vkGetDeviceQueue")) {
      real_get_device_queue = (PFN_vkGetDeviceQueue)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkGetDeviceQueue_shim;
    }
    if (!strcmp(name, "vkGetDeviceQueue2")) {
      real_get_device_queue2 = (PFN_vkGetDeviceQueue2)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkGetDeviceQueue2_shim;
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
      real_create_swapchain = (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkCreateSwapchainKHR_shim;
    }
    if (!strcmp(name, "vkGetSwapchainImagesKHR")) {
      real_get_swapchain_images = (PFN_vkGetSwapchainImagesKHR)vkGetInstanceProcAddr(inst, name);
#ifdef NETHERSX2_VK_DIAGNOSTIC
      return (PFN_vkVoidFunction)vkGetSwapchainImagesKHR_shim;
#else
      return (PFN_vkVoidFunction)real_get_swapchain_images;
#endif
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
      real_destroy_swapchain = (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkDestroySwapchainKHR_shim;
    }
    if (!strcmp(name, "vkDestroyDevice")) {
      real_destroy_device = (PFN_vkDestroyDevice)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkDestroyDevice_shim;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
      real_qpresent = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(inst, name);
      return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
    }
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR")) {
      PFN_vkVoidFunction f = vkGetInstanceProcAddr(inst, name);
      if (!f) return NULL; // don't advertise what NVK doesn't have
      real_memprops2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)f;
      return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2_shim;
    }
  }

  PFN_vkVoidFunction fn = vkGetInstanceProcAddr(inst, name);
  // Restore the loader contract for the bootstrap set NVK's ICD GIPA declines.
  if (!fn)
    fn = vk_loader_global_command(name);

#ifdef NETHERSX2_VK_DIAGNOSTIC
  /* Successful proc-address lookups are routine and produced a long startup
   * trace with little diagnostic value. Keep only unresolved commands; those
   * are the actionable cases when GS initialisation fails. */
  if (!fn)
    vk_diag_note("gipa unresolved name=%s", name ? name : "(null)");
#endif
  return fn;
}

// enumerate instance extensions -- advertise VK_KHR_android_surface (which NVK
// does NOT provide) so the core's availability check passes. Swapped for the real
// VK_NN_vi_surface at vkCreateInstance below.
VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties_hook(const char *layer, uint32_t *pCount,
                                            VkExtensionProperties *pProps) {
  if (pProps == NULL) {
    VkResult r = vkEnumerateInstanceExtensionProperties(layer, pCount, NULL);
    if (r == VK_SUCCESS) (*pCount)++; // reserve a slot for the injected name
    return r;
  }
  uint32_t want = *pCount, got = want ? want - 1 : 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(layer, &got, pProps);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
  VkExtensionProperties inj = { VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 6 };
  if (got < want) { pProps[got++] = inj; *pCount = got; return VK_SUCCESS; }
  *pCount = got;
  return VK_INCOMPLETE;
}

// create instance -- swap the core's VK_KHR_android_surface for the real
// VK_NN_vi_surface so NVK accepts the instance and owns the VI surface extension.
VkResult VKAPI_CALL
vkCreateInstance_hook(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *alloc, VkInstance *out) {
#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateInstance begin api=%u app=%s engine=%s extensions=%u",
               ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0,
               ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ?
                   ci->pApplicationInfo->pApplicationName : "(null)",
               ci->pApplicationInfo && ci->pApplicationInfo->pEngineName ?
                   ci->pApplicationInfo->pEngineName : "(null)",
               ci->enabledExtensionCount);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
    vk_diag_note("instance extension[%u]=%s", i,
                 ci->ppEnabledExtensionNames[i]);
#endif
  int lsfg_launch_prepared =
      prefs_get_bool("Wrapper/LSFGEnabled", false) &&
      file_readable(lsfg_dll_path());

  uint32_t n = ci->enabledExtensionCount;
  const char **ext = malloc(sizeof(char *) * (n ? n : 1));
  for (uint32_t i = 0; i < n; i++) {
    ext[i] = strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)
                 ? ci->ppEnabledExtensionNames[i]
                 : VK_NN_VI_SURFACE_EXTENSION_NAME;
  }
  VkInstanceCreateInfo m = *ci;
  m.ppEnabledExtensionNames = ext;
  VkResult r = vkCreateInstance(&m, alloc, out);
  free((void *)ext);

  if (r == VK_SUCCESS) {
    tracked_instance = *out;
    tracked_instance_api = (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) ?
        ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    tracked_physical_device = VK_NULL_HANDLE;
    tracked_device = VK_NULL_HANDLE;
    tracked_queue_count = 0;
    lsfg_main_queue_family = VK_QUEUE_FAMILY_IGNORED;
    lsfg_device_capable = 0;
    lsfg_session_prepared = lsfg_launch_prepared;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    vk_diag_submit_calls = 0;
    vk_diag_submit2_calls = 0;
    vk_diag_acquire_calls = 0;
    vk_diag_swapchain_image_calls = 0;
#endif
    // The launcher switch is an availability gate, not the initial runtime
    // state. Every game starts with frame generation disabled until the user
    // explicitly enables it from the in-game overlay.
    vk_lsfg_request_enabled(0);
    reset_tracked_swapchain();
  }

#ifdef NETHERSX2_VK_DIAGNOSTIC
  vk_diag_note("vkCreateInstance end result=%d instance=%p lsfg_prepared=%d",
               r, out && r == VK_SUCCESS ? (void *)*out : NULL,
               lsfg_launch_prepared);
#endif

  return r;
}

#endif // USE_VULKAN
