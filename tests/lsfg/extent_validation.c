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

  if (failures)
    return 1;
  puts("LSFG mip extents stay valid at 1/8 flow resolution");
  return 0;
}
