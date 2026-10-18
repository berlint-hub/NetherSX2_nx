/* patches.c -- binary patches applied to the writable core image
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"
#include "../so_util.h"
#include "../prefs.h"
#include "../corelog.h"

extern so_module emu_mod; // libemucore.so (defined in main.c)

#define INSN_NOP        0xd503201fu
#define INSN_MOV_X9_XZR 0xaa1f03e9u
#define INSN_MOV_X8_XZR 0xaa1f03e8u

typedef struct { uint32_t vaddr, expect, insn; const char *what; } Patch;
static const Patch g_patches_4248[] = {
  { 0x83a3d0, 0x5400c3cb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x83a3e8, 0x940c7546, INSN_NOP,        "s1 bl memchr" },
  { 0x83a418, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x83a774, 0x5400a6ab, INSN_NOP,        "s2 b.lt(len)" },
  { 0x83a78c, 0x940c745d, INSN_NOP,        "s2 bl memchr" },
  { 0x83a7bc, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x83b8c0, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x83bbd0, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  { 0x83ce74, 0x54002b2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x83ce8c, 0x940c6a9d, INSN_NOP,        "vm bl memchr" },
  { 0x83ceb8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  // Emit the version tag expected by the GameDB cache loader.
  { 0x2e3fc8, 0x528c86ea, 0x528cad0a, "gamedb cache ver lo" },
  { 0x2e3fcc, 0x72a726ca, 0x72a52c8a, "gamedb cache ver hi" },
  // Center aspect-fitted content.
  { 0x61c924, 0x54000141, INSN_NOP, "center display X" },
  { 0x61c964, 0x54000221, INSN_NOP, "center display Y" },
  // Android exposes 0.25x, but this core revision clamps the GS multiplier to 0.5x.
  { 0x4bab84, 0x1e2c1002, 0x1e2a1002, "GS upscale minimum 0.25x" },
};

static const Patch g_patches_3668[] = {
  { 0x82d0c0, 0x5400c3eb, INSN_NOP,        "s1 b.lt(len)" },
  { 0x82d0d8, 0x940c68aa, INSN_NOP,        "s1 bl memchr" },
  { 0x82d108, 0xaa0b0129, INSN_MOV_X9_XZR, "s1 force match" },
  { 0x82d464, 0x5400a6cb, INSN_NOP,        "s2 b.lt(len)" },
  { 0x82d47c, 0x940c67c1, INSN_NOP,        "s2 bl memchr" },
  { 0x82d4ac, 0xaa0b0129, INSN_MOV_X9_XZR, "s2 force match" },
  { 0x82e5b4, 0x34001c58, INSN_NOP,        "s3 class-name match (cbz w24)" },
  { 0x82e8c4, 0x340003d7, INSN_NOP,        "s4 string match (cbz w23)" },
  { 0x82fb64, 0x54002c2b, INSN_NOP,        "vm b.lt(len)" },
  { 0x82fb7c, 0x940c5e01, INSN_NOP,        "vm bl memchr" },
  { 0x82fba8, 0xaa0a0108, INSN_MOV_X8_XZR, "vm force match" },
  { 0x2e3378, 0x528c26cb, 0x528d2e6b, "gamedb cache ver lo" },
  { 0x2e337c, 0x72a7068b, 0x72a52c6b, "gamedb cache ver hi" },
  { 0x62a204, 0x54000141, INSN_NOP, "center display X" },
  { 0x62a244, 0x54000221, INSN_NOP, "center display Y" },
  { 0x4ab4a8, 0x1e2c1002, 0x1e2a1002, "GS upscale minimum 0.25x" },
};

// Ignore the core's boot-time RequestStop until application shutdown.
static void *g_real_reqstop = 0;
static uintptr_t g_reqstop_slot;
volatile int g_allow_stop;

static void reqstop_hook(void *thisptr) {
  if (!g_allow_stop) return;
  ((void (*)(void *))g_real_reqstop)(thisptr);
}

CoreVersion g_core_version = CORE_VER_UNKNOWN;

int core_is_3668(void) { return g_core_version == CORE_VER_V22N_3668; }

typedef struct {
  uintptr_t object;
  uintptr_t destructor;
} MtgsOffsets;

static const MtgsOffsets g_mtgs_4248 = {0xc6adff0, 0x4add70};
static const MtgsOffsets g_mtgs_3668 = {0xc6a0e80, 0x49ed60};
static void (*g_mtgs_destructor)(void *);
static void *g_mtgs_argument;
static int g_mtgs_destructor_registered;
static int g_mtgs_destructor_called;

static const MtgsOffsets *mtgs_offsets(void) {
  return g_core_version == CORE_VER_V22N_4248 ? &g_mtgs_4248 :
         g_core_version == CORE_VER_V22N_3668 ? &g_mtgs_3668 : NULL;
}

static void mtgs_destructor_once(void *unused) {
  (void)unused;
  core_shutdown_mtgs();
}

int core_wrap_destructor(void (**destructor)(void *), void **argument) {
  const MtgsOffsets *offsets = mtgs_offsets();
  if (!offsets || !destructor || !argument)
    return 0;

  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  if ((uintptr_t)*destructor != base + offsets->destructor ||
      (uintptr_t)*argument != base + offsets->object)
    return 0;

  g_mtgs_destructor = *destructor;
  g_mtgs_argument = *argument;
  g_mtgs_destructor_registered = 1;
  *destructor = mtgs_destructor_once;
  *argument = NULL;
  return 1;
}

int core_shutdown_mtgs(void) {
  const MtgsOffsets *offsets = mtgs_offsets();
  if (!offsets || !g_mtgs_destructor_registered || !g_mtgs_destructor ||
      !g_mtgs_argument || offsets->destructor + 8 > emu_mod.load_size)
    return -1;

  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  const uint32_t *code = (const uint32_t *)(base + offsets->destructor);
  if (code[0] != 0xa9bd7bfd || code[1] != 0xf9000bf5 ||
      (uintptr_t)g_mtgs_destructor != base + offsets->destructor ||
      (uintptr_t)g_mtgs_argument != base + offsets->object)
    return -1;

  if (__atomic_exchange_n(&g_mtgs_destructor_called, 1, __ATOMIC_ACQ_REL))
    return 0;

  g_mtgs_destructor(g_mtgs_argument);
  return 1;
}

int core_shutdown_achievements(void) {
  typedef struct {
    uintptr_t shutdown;
    uint32_t downloader_load;
    uint32_t downloader_clear;
  } AchievementOffsets;
  static const AchievementOffsets offsets_4248 = {
    0x81b720, 0xf943c6c0, 0xf903c6df
  };
  static const AchievementOffsets offsets_3668 = {
    0x80fce0, 0xf9431ac0, 0xf9031adf
  };

  const AchievementOffsets *offsets =
      g_core_version == CORE_VER_V22N_4248 ? &offsets_4248 :
      g_core_version == CORE_VER_V22N_3668 ? &offsets_3668 : NULL;
  if (!offsets || offsets->shutdown + 0x12c > emu_mod.load_size)
    return -1;

  const uintptr_t base = (uintptr_t)emu_mod.load_virtbase;
  const uint32_t *code = (const uint32_t *)(base + offsets->shutdown);
  // Reject core binaries that do not match this fixed shutdown offset.
  if (code[0] != 0xd10143ff || code[1] != 0xa9027bfd ||
      code[2] != 0xa90357f6 || code[3] != 0xa9044ff4 ||
      code[0x114 / 4] != offsets->downloader_load ||
      code[0x118 / 4] != offsets->downloader_clear ||
      code[0x11c / 4] != 0xb4000080 ||
      code[0x120 / 4] != 0xf9400008 ||
      code[0x124 / 4] != 0xf9400508 ||
      code[0x128 / 4] != 0xd63f0100)
    return -1;

  return ((int (*)(void))(base + offsets->shutdown))() ? 1 : 0;
}

static int in_range(uint32_t off, uint32_t sz) {
  return (uint64_t)off + sz <= (uint64_t)emu_mod.load_size;
}

static void patch_xgkick_skip(void) {
  if (!core_is_3668() || !prefs_get_bool("EmuCore/Patches/XGKickSkip", false) ||
      prefs_get_bool("Wrapper/BootBIOS", false) ||
      !in_range(0x3cfa98, 4) || !in_range(0x3cfad8, 4))
    return;

  volatile uint32_t *first =
      (volatile uint32_t *)((uintptr_t)emu_mod.load_base + 0x3cfa98);
  volatile uint32_t *second =
      (volatile uint32_t *)((uintptr_t)emu_mod.load_base + 0x3cfad8);
  if (*first != 0x14000012 || *second != 0x140001d7)
    return;

  *first = INSN_NOP;
  *second = INSN_NOP;
}

static uint32_t a64_branch(uint32_t from, uint32_t to) {
  return 0x14000000u | (((to - from) >> 2) & 0x03ffffffu);
}

static uint32_t a64_adrp(unsigned reg, uint32_t from, uint32_t target) {
  const int64_t delta = ((int64_t)(target & ~0xfffu) -
                         (int64_t)(from & ~0xfffu)) >> 12;
  return 0x90000000u | ((uint32_t)(delta & 3) << 29) |
         ((uint32_t)((delta >> 2) & 0x7ffff) << 5) | reg;
}

static void patch_quick_menu_layout(void) {
  typedef struct {
    uint32_t center;
    uint32_t measure;
    uint32_t draw;
    uint32_t cave;
    uint32_t imgui_io;
  } QuickMenuPatch;
  static const QuickMenuPatch patch_4248 = {
    0x7df77c, 0x7df738, 0x7df820, 0x88a15c, 0xc6cb5c0
  };
  static const QuickMenuPatch patch_3668 = {
    0x7d4d8c, 0x7d4d48, 0x7d4e30, 0x87e7ec, 0xc6be458
  };
  const QuickMenuPatch *patch = core_is_3668() ? &patch_3668 :
      g_core_version == CORE_VER_V22N_4248 ? &patch_4248 : NULL;
  uint32_t stub[] = {
    0x2d7327af, 0xaa1e03f0, 0x9400001b, 0x54000161,
    0,          0,          0xbd401102, 0x1e2b3842,
    0x1e2c1003, 0x1e23084f, 0xbd401502, 0x1e2e3842,
    0x1e230849, 0x2d3327af, 0xd65f0200, 0xaa1e03f0,
    0x9400000d, 0xbd401700, 0x54000061, 0x1e2f3003,
    0x1e230800, 0xd65f0200, 0xaa1e03f0, 0x94000006,
    0xbd40170a, 0x54000061, 0x1e2f3003, 0x1e23094a,
    0xd65f0200, 0x39400348, 0x370000a8, 0x7100a11f,
    0x54000221, 0x91000748, 0x14000005, 0xf9400748,
    0xf100511f, 0x54000181, 0xf9400b48, 0xa9402909,
    0x5800014b, 0xeb0b013f, 0x540000e1, 0x5800012b,
    0xeb0b015f, 0x54000081, 0xb9401109, 0x180000ea,
    0x6b0a013f, 0xd65f03c0, 0x6874656e, 0x78737265,
    0x75715f32, 0x5f6b6369, 0x756e656d,
  };
  if (!patch || !in_range(patch->center, 4) ||
      !in_range(patch->measure, 4) || !in_range(patch->draw, 4) ||
      !in_range(patch->cave, sizeof(stub)))
    return;

  uint32_t *const center = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->center);
  uint32_t *const measure = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->measure);
  uint32_t *const draw = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->draw);
  uint32_t *const cave = (uint32_t *)((uintptr_t)emu_mod.load_base + patch->cave);
  if (*center != 0x2d7327afu || *measure != 0xbd401700u || *draw != 0xbd40170au)
    return;
  for (unsigned i = 0; i < sizeof(stub) / sizeof(*stub); i++)
    if (cave[i] != 0)
      return;

  stub[4] = a64_adrp(8, patch->cave + 16, patch->imgui_io);
  stub[5] = 0xf9400108u | (((patch->imgui_io & 0xfff) >> 3) << 10);
  memcpy(cave, stub, sizeof(stub));
  *center = a64_branch(patch->center, patch->cave) | 0x80000000u;
  *measure = a64_branch(patch->measure, patch->cave + 0x3c) | 0x80000000u;
  *draw = a64_branch(patch->draw, patch->cave + 0x58) | 0x80000000u;
}

static void detect_core_version(void) {
  g_core_version = CORE_VER_UNKNOWN;
  if (in_range(0xd4436, 24) &&
      !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xd4436), "v2.2n-4248", 10))
    g_core_version = CORE_VER_V22N_4248;
  else if (in_range(0xff01f, 24) &&
           !memcmp((const char *)((uintptr_t)emu_mod.load_base + 0xff01f), "v2.2n-3668", 10))
    g_core_version = CORE_VER_V22N_3668;
}

// VU Cycle Steal (MTVU path), 4248-only. One-word repurpose of the EECycleSkip
// multiply inside VU_Thread::ExecuteVU:
//
//   0x4b2500: 0x1B0A7D08 = MUL w8, w8, w10
//     w8  = min(average VU1 cycles, 3000)  -- MOVZ #3000 at 0x4b24f0,
//                                             CMP  #3000 at 0x4b24f8,
//                                             CSEL lo    at 0x4b24fc
//     w10 = the EECycleSkip byte           -- LDRB [x21,#5] at 0x4b24e4
//     fed by an inlined Get_vuCycles: LDAR pair at 0x4b24c0/c4, ADD chain to
//     0x4b24d8, LSR #2 at 0x4b24dc.
//
//   level 1 (0x53017D08 = LSR w8, w8, #1): the cycle counters gain cycles >> 1
//   level 2 (0x2A0803E8 = MOV w8, w8):    the counters gain the full cycles
//
// The two downstream ADD/STR pairs credit the stolen amount -- cpuRegs.cycle at
// 0x4b2504-0c and VU0.cycle at 0x4b2510-1c -- so the EE believes less time
// passed than the VU actually burned. w8 is dead afterwards (0x4b2528 rewrites
// it with LDRB w8,[x21]), so replacing the multiply is safe.
//
// Found by SIGNATURE, not by copying an offset: the equivalent patch in the
// sibling TEST-2 tree sits at 0x4A3320, but that tree builds against a
// different 12 094 744 B core and 0x4A3320 holds ADD x1,sp,#8 in ours
// (12 162 984 B). Scanning this image for MUL w8,w8,w10 (0x1B0A7D08) yields 26
// hits and exactly one has MOVZ #3000 / CMP #3000 / LDRB [x21,#5] within reach,
// so the site is unambiguous.
//
// While active this REPLACES EECycleSkip, because that multiply IS the skip --
// so EE cycle skip must be Off. MTVU only: this is VU_Thread::ExecuteVU, the
// threaded path, and it needs EmuCore/Speedhacks/vuThread on. Applied at core
// load, so changing the menu needs a game restart. It is an accuracy hack: the
// EE and VU1 disagree about elapsed time, which is the whole point.
#define VU_STEAL_4248_ADDR   0x4b2500u
#define VU_STEAL_4248_EXPECT 0x1B0A7D08u
#define VU_STEAL_4248_LVL1   0x53017D08u
#define VU_STEAL_4248_LVL2   0x2A0803E8u

static void patch_vu_steal_4248(void) {
  if (g_core_version != CORE_VER_V22N_4248)
    return;
  const int level = prefs_get_int("EmuCore/Speedhacks/vuCycleSteal", 0);
  if (level < 1 || level > 2)
    return;
  if (!in_range(VU_STEAL_4248_ADDR, 4))
    return;
  volatile uint32_t *p =
      (volatile uint32_t *)((uintptr_t)emu_mod.load_base + VU_STEAL_4248_ADDR);
  // Logged both ways on purpose: a silent no-op here would look exactly like
  // "the setting does nothing", and the only way to tell a wrong offset from a
  // wrong level is to see the word that was actually there.
  if (*p != VU_STEAL_4248_EXPECT) {
    corelog_log(CORELOG_WARN, "Wrapper",
                "VU cycle steal: core+0x%x holds 0x%08x, expected 0x%08x -- NOT applied",
                VU_STEAL_4248_ADDR, *p, VU_STEAL_4248_EXPECT);
    return;
  }
  *p = (level == 1) ? VU_STEAL_4248_LVL1 : VU_STEAL_4248_LVL2;
  corelog_log(CORELOG_INFO, "Wrapper",
              "VU cycle steal level %d applied at core+0x%x (0x%08x -> 0x%08x); "
              "keep EE cycle skip Off",
              level, VU_STEAL_4248_ADDR, VU_STEAL_4248_EXPECT, *p);
}

void patch_game(void) {
  detect_core_version();

  if (g_core_version == CORE_VER_UNKNOWN)
    return;

  const Patch *tbl; int total;
  if (core_is_3668()) {
    tbl = g_patches_3668; total = (int)(sizeof(g_patches_3668) / sizeof(*g_patches_3668));
    g_reqstop_slot = 0xb6f920;
  } else {
    tbl = g_patches_4248; total = (int)(sizeof(g_patches_4248) / sizeof(*g_patches_4248));
    g_reqstop_slot = 0xb80558;
  }

  for (int i = 0; i < total; i++) {
    const Patch *pt = &tbl[i];
    if (!in_range(pt->vaddr, 4)) {

      continue;
    }
    volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)emu_mod.load_base + pt->vaddr);
    if (*p != pt->expect) {

      continue;
    }
    *p = pt->insn;
  }

  patch_xgkick_skip();
  patch_quick_menu_layout();
  patch_vu_steal_4248();

  if (!in_range((uint32_t)g_reqstop_slot, 8))
    return;
  volatile uintptr_t *slot =
      (volatile uintptr_t *)((uintptr_t)emu_mod.load_base + g_reqstop_slot);
  g_real_reqstop = (void *)*slot;
  *slot = (uintptr_t)&reqstop_hook;

}
