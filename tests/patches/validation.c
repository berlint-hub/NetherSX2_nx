#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/corelog.h"
#include "../../source/hooks.h"
#include "../../source/prefs.h"
#include "../../source/so_util.h"

so_module emu_mod;

/* patches.c reports what it did through corelog, which needs libnx and cannot
 * be compiled for the host -- the real corelog.c is deliberately not part of
 * this target. The patch functions only use it for informational lines, so a
 * stub that drops them is faithful to what the tests exercise. */
void corelog_log(int priority, const char *tag, const char *format, ...) {
  (void)priority; (void)tag; (void)format;
}

static const size_t image_size = 0xb80560;
static uint8_t *image;
static unsigned checks;

static void prepare(const char *version) {
  memset(image, 0, image_size);
  emu_mod.load_base = image;
  emu_mod.load_size = image_size;
  if (!strcmp(version, "3668"))
    memcpy(image + 0xff01f, "v2.2n-3668", 10);
  else if (!strcmp(version, "4248"))
    memcpy(image + 0xd4436, "v2.2n-4248", 10);
  *(uint32_t *)(image + 0x3cfa98) = 0x14000012;
  *(uint32_t *)(image + 0x3cfad8) = 0x140001d7;
  // VU cycle steal site, pristine: MUL w8, w8, w10 in VU_Thread::ExecuteVU.
  *(uint32_t *)(image + 0x4b2500) = 0x1B0A7D08;
  prefs_init("");
}

static void check_pair(const char *name, uint32_t first, uint32_t second) {
  patch_game();
  if (*(uint32_t *)(image + 0x3cfa98) != first ||
      *(uint32_t *)(image + 0x3cfad8) != second) {
    fprintf(stderr, "%s failed\n", name);
    exit(EXIT_FAILURE);
  }
  checks++;
}

static void check_vu(const char *name, uint32_t expected) {
  patch_game();
  const uint32_t got = *(uint32_t *)(image + 0x4b2500);
  if (got != expected) {
    fprintf(stderr, "VU cycle steal %s failed: got 0x%08x, expected 0x%08x\n",
            name, got, expected);
    exit(EXIT_FAILURE);
  }
  checks++;
}

int main(void) {
  image = malloc(image_size);
  if (!image)
    return EXIT_FAILURE;

  prepare("3668");
  if (prefs_get_bool("EmuCore/Patches/XGKickSkip", true))
    return EXIT_FAILURE;
  check_pair("default off", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_remove("EmuCore/Patches/XGKickSkip");
  check_pair("missing setting", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", false);
  check_pair("explicit off", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  check_pair("enabled", 0xd503201f, 0xd503201f);
  check_pair("already patched", 0xd503201f, 0xd503201f);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  *(uint32_t *)(image + 0x3cfa98) = 0x14000013;
  check_pair("first mismatch", 0x14000013, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  *(uint32_t *)(image + 0x3cfad8) = 0x140001d8;
  check_pair("second mismatch", 0x14000012, 0x140001d8);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  *(uint32_t *)(image + 0x3cfa98) = 0xd503201f;
  check_pair("partially patched image", 0xd503201f, 0x140001d7);

  prepare("4248");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  check_pair("unsupported core", 0x14000012, 0x140001d7);

  prepare("");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  check_pair("unknown core", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  prefs_set_bool("Wrapper/BootBIOS", true);
  check_pair("BIOS boot", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  emu_mod.load_size = 0x3cfa9b;
  check_pair("first out of range", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Patches/XGKickSkip", true);
  emu_mod.load_size = 0x3cfadb;
  check_pair("second out of range", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_string("EmuCore/Patches/XGKickSkip", "invalid");
  check_pair("invalid setting", 0x14000012, 0x140001d7);

  prepare("3668");
  prefs_set_bool("EmuCore/Gamefixes/XgKickHack", true);
  check_pair("independent XGKick sync", 0x14000012, 0x140001d7);

  // VU cycle steal (4248 only, gated on EmuCore/Speedhacks/vuCycleSteal).
  prepare("4248");
  prefs_remove("EmuCore/Speedhacks/vuCycleSteal");
  check_vu("missing setting", 0x1B0A7D08);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 1);
  check_vu("level 1 -> LSR w8,w8,#1", 0x53017D08);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 2);
  check_vu("level 2 -> MOV w8,w8", 0x2A0803E8);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 0);
  check_vu("level 0 off", 0x1B0A7D08);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 3);
  check_vu("level out of range", 0x1B0A7D08);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", -1);
  check_vu("negative level", 0x1B0A7D08);

  prepare("3668");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 2);
  check_vu("wrong core version", 0x1B0A7D08);

  prepare("");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 2);
  check_vu("unknown core version", 0x1B0A7D08);

  // The guard that makes a wrong offset or a different core image harmless:
  // verify before write, warn instead of patching.
  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 1);
  *(uint32_t *)(image + 0x4b2500) = 0x910023E1;   // what 0x4A3320 holds in TEST-2
  check_vu("unexpected original word", 0x910023E1);

  prepare("4248");
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 1);
  emu_mod.load_size = 0x4b2503;
  check_vu("site out of range", 0x1B0A7D08);

  free(image);
  printf("%u core patch checks passed\n", checks);
  return EXIT_SUCCESS;
}
