/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Safe, host-testable mip extents for the LSFG Vulkan shader pyramid. */
#ifndef NETHERSX2_LSFG_EXTENT_H
#define NETHERSX2_LSFG_EXTENT_H

#include <stdint.h>

typedef struct {
  uint32_t width;
  uint32_t height;
} LsfgMipExtent;

/* Vulkan image dimensions must stay non-zero. Small flow resolutions can reach
 * a zero-sized mip when repeatedly shifted (e.g. 106x60 >> 6 at 1/8 flow
 * scale). Clamp each mip axis to one pixel; avoid undefined shifts as well. */
static inline LsfgMipExtent lsfg_mip_extent(uint32_t width, uint32_t height,
                                            uint32_t level) {
  LsfgMipExtent mip = {1, 1};
  if (level >= 32)
    return mip;
  width >>= level;
  height >>= level;
  mip.width = width ? width : 1;
  mip.height = height ? height : 1;
  return mip;
}

/* Dispatch extents must keep the zero that a fully-shifted level produces:
 * image dimensions clamp to >=1, but dispatch counts must not be derived from
 * that clamped size. If either shifted axis is zero, the mip has no area and
 * the whole stage is skipped (0x0 groups). */
static inline LsfgMipExtent lsfg_mip_dispatch(uint32_t width, uint32_t height,
                                              uint32_t level, uint32_t add,
                                              uint32_t shift) {
  LsfgMipExtent dispatch = {0, 0};
  if (level >= 32 || shift >= 32)
    return dispatch;
  width >>= level;
  height >>= level;
  if (!width || !height)
    return dispatch;
  dispatch.width = (width + add) >> shift;
  dispatch.height = (height + add) >> shift;
  return dispatch;
}

#endif /* NETHERSX2_LSFG_EXTENT_H */
