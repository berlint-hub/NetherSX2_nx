#include <stdarg.h>
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
 * this target.
 *
 * The lines are captured rather than dropped. Some of them ARE the behaviour
 * under test: a hardware log that omits the wrapper build id or the active
 * vuCycleSteal value cannot be attributed to a commit or to a setting, which is
 * precisely what TEST3's analysis of the GT3 log (docs/logs/gt3-nvk-diag-notes.md
 * §5) lists as undeterminable. A discarding stub would let both silently
 * regress, so these assertions are what keeps them in the binary. */
#define CAP_MAX 256
static char cap[CAP_MAX][192];
static unsigned cap_n;

void corelog_log(int priority, const char *tag, const char *format, ...) {
  (void)priority; (void)tag;
  if (cap_n >= CAP_MAX) return;
  va_list ap;
  va_start(ap, format);
  vsnprintf(cap[cap_n], sizeof(cap[0]), format, ap);
  va_end(ap);
  ++cap_n;
}

static unsigned cap_count(const char *needle) {
  unsigned n = 0;
  for (unsigned i = 0; i < cap_n; ++i)
    if (strstr(cap[i], needle)) ++n;
  return n;
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

/* One patch_game() call against `version`, then assert `needle` was logged
 * `want` times. `setup` runs after prepare() so it can set prefs and corrupt the
 * image the way a real mismatch would. */
typedef void (*log_setup_fn)(void);

static void check_log(const char *name, const char *version, log_setup_fn setup,
                      const char *needle, unsigned want) {
  cap_n = 0;
  prepare(version);
  if (setup) setup();
  patch_game();
  const unsigned got = cap_count(needle);
  if (got != want) {
    fprintf(stderr, "log %s failed: \"%s\" seen %ux, expected %ux\n",
            name, needle, got, want);
    exit(EXIT_FAILURE);
  }
  checks++;
}

static void setup_vu_off(void) { prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 0); }
static void setup_vu_l1(void)  { prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 1); }
static void setup_vu_bad(void) {
  prefs_set_int("EmuCore/Speedhacks/vuCycleSteal", 1);
  *(uint32_t *)(image + 0x4b2500) = 0x910023E1;  // what TEST-2's offset holds
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

  // The lines a hardware log must contain for TEST3's §5 items 2 and 3: which
  // build produced it, and which vuCycleSteal value was active.
  check_log("build id logged on known core", "4248", NULL, "wrapper build ", 1);
  check_log("build id names the core",       "4248", NULL, "core v2.2n-4248", 1);
  check_log("3668 named",                    "3668", NULL, "core v2.2n-3668", 1);
  // Before the UNKNOWN bail-out on purpose: an unrecognised core is exactly when
  // it matters most to know which wrapper produced the log.
  check_log("build id logged on UNKNOWN core", "", NULL, "wrapper build ", 1);
  check_log("UNKNOWN core named",              "", NULL, "core UNKNOWN", 1);
  // Off must be reported, not silent -- otherwise "no line" is ambiguous between
  // "switched off" and "the site did not match".
  check_log("VU steal off is reported",  "4248", setup_vu_off, "VU cycle steal: off", 1);
  check_log("VU steal off shows value",  "4248", setup_vu_off, "vuCycleSteal=0", 1);
  check_log("VU steal applied reported", "4248", setup_vu_l1,
            "VU cycle steal level 1 applied", 1);
  check_log("wrong word warns",          "4248", setup_vu_bad, "NOT applied", 1);
  // The version gate returns first, so a 3668 core must not claim an off level.
  check_log("no off line on 3668",       "3668", setup_vu_off, "VU cycle steal: off", 0);

  free(image);
  printf("%u core patch checks passed\n", checks);
  return EXIT_SUCCESS;
}
