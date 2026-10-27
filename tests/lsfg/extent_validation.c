#include "lsfg_extent.h"

#include <stdio.h>

static int check(const char *name, int condition) {
  if (condition)
    return 0;
  fprintf(stderr, "FAIL: %s\n", name);
  return 1;
}

int main(void) {
  int failures = 0;
  LsfgMipExtent mip = lsfg_mip_extent(106, 60, 6);
  failures += check("1/8 flow scale keeps deepest mip nonzero",
                    mip.width == 1 && mip.height == 1);

  mip = lsfg_mip_extent(160, 90, 6);
  failures += check("720p 1/8 flow pyramid retains floor-sized mip",
                    mip.width == 2 && mip.height == 1);

  mip = lsfg_mip_extent(1, 1, 0);
  failures += check("base extent stays unchanged",
                    mip.width == 1 && mip.height == 1);

  mip = lsfg_mip_extent(64, 32, 32);
  failures += check("oversized mip shift is safe and nonzero",
                    mip.width == 1 && mip.height == 1);

  LsfgMipExtent dispatch = lsfg_mip_dispatch(106, 60, 6, 7, 3);
  mip = lsfg_mip_extent(106, 60, 6);
  failures += check("106x60 level 6 image clamps to 1x1",
                    mip.width == 1 && mip.height == 1);
  failures += check("106x60 level 6 dispatch skips zero-area mip",
                    dispatch.width == 0 && dispatch.height == 0);

  dispatch = lsfg_mip_dispatch(106, 60, 5, 7, 3);
  mip = lsfg_mip_extent(106, 60, 5);
  failures += check("106x60 level 5 image remains 3x1",
                    mip.width == 3 && mip.height == 1);
  failures += check("106x60 level 5 dispatch is 1x1",
                    dispatch.width == 1 && dispatch.height == 1);

  dispatch = lsfg_mip_dispatch(40, 30, 5, 7, 3);
  failures += check("40x30 level 5 dispatch skips zero-area mip",
                    dispatch.width == 0 && dispatch.height == 0);

  dispatch = lsfg_mip_dispatch(106, 60, 0, 7, 3);
  failures += check("base-level dispatch remains 14x8",
                    dispatch.width == 14 && dispatch.height == 8);

  if (failures)
    return 1;
  puts("LSFG mip extents stay valid at 1/8 flow resolution");
  return 0;
}
