/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Same-device LSFG-VK bridge for the Switch Vulkan renderer.
 *
 * The implementation is C++20, but this deliberately small C ABI keeps the
 * existing Vulkan hook in source/hooks/vk.c independent of the vendored C++
 * backend. LSFG owns only its private images, pipelines and synchronization;
 * every NetherSX2 Vulkan handle is borrowed.
 */
#ifndef NETHERSX2_LSFG_BRIDGE_H
#define NETHERSX2_LSFG_BRIDGE_H

#ifdef USE_VULKAN

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgNxRuntime LsfgNxRuntime;

typedef struct LsfgNxCreateInfo {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;

  VkSwapchainKHR swapchain;
  VkExtent2D extent;
  const VkImage *swapchain_images;
  uint32_t swapchain_image_count;

  const char *shader_dll_path;
  float flow_scale;
  bool performance_mode;
} LsfgNxCreateInfo;

LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *info);
void lsfg_nx_destroy(LsfgNxRuntime *runtime);

/*
 * Returns true when the bridge consumed the presentation and wrote result.
 * generated_frame is set only after an interpolated output was accepted by the
 * swapchain present call; the first warm-up frame may be capture-only. Optional
 * present_ns/present_calls outputs aggregate time and call count spent inside
 * the driver's QueuePresentKHR calls (null in non-diagnostic builds). Runtime
 * faults are reported as errors and deliberately do not fall back to ordinary
 * presentation while the user has LSFG enabled.
 */
bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue,
                     const VkPresentInfoKHR *present_info, VkResult *result,
                     bool *generated_frame, uint64_t *present_ns,
                     uint32_t *present_calls);

/*
 * Capture the source frame into the bridge's ping-pong slot and present it
 * unchanged: no interpolation, one output frame instead of two.
 *
 * Fractional generation skips interpolation on most presents, but it must not
 * skip the capture. The bridge interpolates between the previous source slot
 * and the current frame, so a present it never saw leaves that slot holding a
 * frame from several presents ago, and the next interpolation would span the
 * whole gap and place the generated frame in the wrong slot of the timeline.
 * Optional present_ns/present_calls outputs report driver present-call time.
 */
bool lsfg_nx_present_capture(LsfgNxRuntime *runtime, VkQueue queue,
                             const VkPresentInfoKHR *present_info, VkResult *result,
                             uint64_t *present_ns, uint32_t *present_calls);

#ifdef __cplusplus
}
#endif

#endif /* USE_VULKAN */
#endif /* NETHERSX2_LSFG_BRIDGE_H */
