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

#endif /* NETHERSX2_LSFG_EXTENT_H */
