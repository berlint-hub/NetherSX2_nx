#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "prefs.h"

#define CHECK(condition, description) do { \
  if (!(condition)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", description, __LINE__); \
    return 1; \
  } \
} while (0)

static int write_fixture(const char *path, const char *contents) {
  FILE *file = fopen(path, "w");
  if (!file) {
    fprintf(stderr, "cannot open fixture '%s': %s\n", path, strerror(errno));
    return 0;
  }
  int ok = fputs(contents, file) >= 0;
  if (fclose(file) != 0)
    ok = 0;
  if (!ok)
    fprintf(stderr, "cannot write fixture '%s'\n", path);
  return ok;
}

int main(void) {
  char path[] = "/tmp/nethersx2-prefs-validation-XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0, "create temporary INI fixture");
  CHECK(close(fd) == 0, "close temporary INI fixture descriptor");

  CHECK(write_fixture(path,
      "# comments and whitespace are accepted\n"
      "  [EmuCore/GS]  \n"
      "low_latency = 1\n"
      "upscale_multiplier = 2.0\n"
      "[Other]\n"
      "value = sectioned\n"), "write sectioned INI fixture");
  prefs_init(path);
  CHECK(prefs_get_bool("EmuCore/GS/low_latency", false),
        "sectioned low_latency=1 maps to EmuCore/GS/low_latency");
  CHECK(prefs_get_float("EmuCore/GS/upscale_multiplier", 0.0f) == 2.0f,
        "other section keys are flattened with their section name");
  CHECK(strcmp(prefs_get_string("Other/value", ""), "sectioned") == 0,
        "a second section does not inherit the previous section");

  CHECK(write_fixture(path,
      "[EmuCore/GS]\n"
      "low_latency = 0\n"), "write sectioned false fixture");
  prefs_init(path);
  CHECK(!prefs_get_bool("EmuCore/GS/low_latency", true),
        "sectioned low_latency=0 overrides the seeded default");

  CHECK(write_fixture(path,
      "EmuCore/GS/low_latency = 1\n"
      "EmuCore/GS/upscale_multiplier = 2.0\n"),
      "write historical flat-key fixture");
  prefs_init(path);
  CHECK(prefs_get_bool("EmuCore/GS/low_latency", false),
        "historical flat low_latency key remains supported");
  CHECK(prefs_get_float("EmuCore/GS/upscale_multiplier", 0.0f) == 2.0f,
        "historical flat numeric keys remain supported");

  CHECK(!prefs_get_bool("Wrapper/nonexistent", false),
        "missing boolean returns its supplied default");
  CHECK(unlink(path) == 0, "remove temporary INI fixture");
  puts("prefs parser: sectioned and flat INI keys passed");
  return 0;
}
