<div align=center>

<img src="extras/banner.png" alt="Banner" width="30%">

</div>
<h1 align=center>NetherSX2 · Switch Port</h1>

A wrapper/port of NetherSX2 to the Nintendo Switch.
It loads the original Android emulator core `libemucore.so`, 
patches it, and runs it inside a minimal Android-like
environment natively.

Everything ships as a **single `NetherSX2.nro`**: it bundles one emulator core
(NetherSX2 v2.2n-`4248` with the Tegra X1 patches), the Vulkan/NVK emulator and
that build's data files, and extracts them to the SD card at launch. There is no
core-version setting — the bundle carries exactly one core. An SDL cover-art
launcher chooses the game and per-game settings, then chainloads the emulator.

Vulkan (NVK) is the only graphics backend in the shipped bundle, so there is no
renderer setting to choose. The native Deko3D and OpenGL targets are still in
the tree and still build (`make RENDERER=DK` / `RENDERER=GL`), but they are not
bundled and the launcher does not offer them.

The launcher supports multiple library folders across SD, USB mass storage
(FAT32, exFAT, NTFS), and SMB shares. The Recent, All games, and Favorites tabs
organize the library; press **Y** for a game's menu and use the settings icon
for global options.

The PS2 theme uses an animated background. Classic keeps the same menu colors
and displays the selected game's artwork behind the library. SteamGridDB cover
downloads let you choose a cover and then an optional background. **Settings >
Library & storage > Download covers** automatically downloads missing covers and
backgrounds while keeping existing local artwork.

**L + R + Plus** to open the quick menu for save states, controller rebinding,
selectable per-game PNACH cheat codes, RetroAchievements progress,
frame-limiter control, reset, and exit.
The cheat browser reads `/switch/nethersx2/cheats/<CRC>.pnach`, recognizes PNACH
`[sections]` and legacy `// code name` blocks.

No emulator core, BIOS, or game assets are included in this repository.

### How to install

1. Copy `NetherSX2.nro` into `/switch/` on your SD card.
2. Put your own PS2 BIOS dump into `/switch/nethersx2/bios/`.

The launcher creates the rest of the folder tree on first run:

```
/switch/NetherSX2.nro
/switch/nethersx2/
  .emu/         <- extracted when selected
  bios/         <- your PS2 BIOS dump           (you supply)
  resources/    <- shaders / GameIndex / fonts  (auto-extracted per core)
  covers/       <- cached covers and background artwork
  gamecfg/      <- per-game launcher settings
  lsfg/         <- optional user-supplied Lossless.dll
  forwarders/   <- HOME shortcut launch records
  hidden.ini    <- hidden library entries
  launcher.ini  <- the launcher's saved config
  nethersx2-core.log <- the core's own console; see Diagnostics
```

### Notes

This will not run in applet/album mode — it needs the full memory of a game
override. Launch it by holding **R** while opening an installed title, or use a
forwarder.

Threads are pinned by role. The core names its own threads through
`prctl(PR_SET_NAME)` — `"CPU Thread"` (EE), `"GS"` (MTGS), `"MTVU"` (VU1),
`"SPU2"`, `"CDVD"`, `"ISO Decompress"` and `"ImGuiFullscreen Texture Loader"` —
and the wrapper turns those names into CPU affinity: EE alone on core 0, GS and
MTVU each on their own core, and SPU2 together with the audio callback and input
on core 3 at priority 44, between audio's 43 and input's 45, because Horizon
never preempts between equal priorities. GS and MTVU are pinned *exclusively*
only when four cores are granted, which means launching through a HOME Menu
shortcut; launched from the album the grant is three cores, so they keep a
migratable mask and a light thread can still move off a busy peer. **Settings >
Thread pinning** turns this off (`Auto`), restoring the older round-robin that
hands out cores in thread-creation order. Every decision is logged to
`/switch/nethersx2/nethersx2-core.log` as
`thread "GS" -> core 1, mask 0x2, priority 30`. The three emulation threads run
at priority 30, above everything else in the process, because Horizon never
preempts between equal priorities and libnx hands each thread its creator's.

Core 3 carries the 48 kHz audio callback (priority 43) and input polling (45),
which leaves it mostly idle, so the threads that `libnvk.a` and
`libnvk_support.a` create for themselves — shader compilation, disk cache — are
moved there at priority 47, below both. They are statically linked into the
emulator, so they inherit the process default affinity, which is every granted
core; without this a driver thread can land on core 0 and interrupt the EE
mid-frame. That is done with `-Wl,--wrap=pthread_create`: our own four creation
sites call `pthr_create_self()` (which goes straight to `__real_pthread_create`)
so they keep the affinity they arrange themselves, and everything else arriving
at `__wrap_pthread_create` is third-party. Each moved thread logs
`driver thread -> core 3, mask 0x8, priority 47`, capped at eight lines. On a
three-core grant there is no spare core, so nothing is moved.

The diagnostic build also counts `sched_yield`, the only spin primitive the core
imports. It reports per thread — `sched_yield: thread "MTVU" called it 10000x,
process total 10000x` — at 100, 1000, 10000 calls and then every 100000, so
the log timestamps give a rate. That is what separates "MTVU is computing" from
"MTVU is spinning idle waiting for the EE to queue work", which the two look
identical for as a core sitting at 100% and which call for opposite fixes. The
per-thread count is in TLS and exact; the process total is a raced increment and
can lose counts, which is fine for telling hundreds per second from hundreds of
thousands and avoids a lock on the hottest path in the process.

**Settings > VU cycle steal** is a binary patch for games whose VU1 thread runs
flat out, such as Gran Turismo 3 distance views. It repurposes the
`EECycleSkip` multiply inside `VU_Thread::ExecuteVU` — one instruction at
`core+0x4b2500`, `MUL w8, w8, w10` — so the cycles VU1 burned are credited to
the EE clock instead of being multiplied by the skip factor: level 1 credits
half of them (`LSR w8, w8, #1`), level 2 all of them (`MOV w8, w8`). It needs
MTVU on, replaces EE cycle skip (keep that Off), applies at core load so it
needs a game restart, and only the 4248 core is supported. The site was found
by instruction signature, not by copying an offset from another build: scanning
this image for `MUL w8, w8, w10` gives 26 hits and exactly one has
`MOVZ #3000` / `CMP #3000` / `LDRB [x21,#5]` within reach. `patch_game()`
verifies the original word before writing and logs a warning instead of
patching if it does not match, so a wrong core image is a no-op, not corruption.

A PS2 BIOS dump is required and must come from your own console; the launcher
warns at startup when `bios/` is empty.

Casual RetroAchievements are available from **Settings > RetroAchievements**.
Sign in once in the launcher, then enable achievements before starting a game.
Successful sign-in stores the returned account token in `launcher.ini`.
Achievement progress and descriptions can be viewed during play from
**L + R + Plus > Achievements**.

The emulator renders through NVK, the Mesa Vulkan driver for the Switch's
Tegra X1 GPU, linked statically from the `nxvk` portlib package. Because
Horizon has no Vulkan loader, that package exports a single entry point,
`vk_icdGetInstanceProcAddr`; `source/vk_loader_shim.c` (generated by
`tools/gen_nvk_loader_shim.py`) provides the entry points the wrapper calls by
forwarding through it, and `source/nvk_libc_stubs.c` supplies the POSIX calls
Mesa expects that newlib does not have. The SDL launcher uses its own graphics
runtime and exits before the emulator starts.

Three things about the Horizon environment need papering over for the core to
work at all, all of them in the POSIX shim rather than in the emulator:

- **Memory.** NVK sizes its one `VkMemoryHeap` from `sysconf(_SC_PHYS_PAGES)`,
  which newlib does not implement. `source/nvk_libc_stubs.c` answers it with
  `InfoType_TotalMemorySize` — the *total* figure, as `_SC_PHYS_PAGES` means on
  every POSIX system. Reporting the free remainder instead gives NVK a heap of
  about 1 MiB (the process has almost nothing left once the newlib heap, the
  core image and its `svcMapProcessCodeMemory` mappings are in place), and the
  allocator then refuses everything.
- **CPU topology.** The core links Facebook's `cpuinfo`, which reads
  `/sys/devices/system/cpu/*` and `/proc/cpuinfo`. Horizon has neither, so
  `source/libc_shim.c` serves both from a small synthetic-file table, sized
  from the core mask Horizon actually granted. It is reachable from `open()`
  *and* `fopen()`, because `cpuinfo` uses `open`/`__read_chk`/`close` while
  other consumers use stdio.
- **File I/O latency.** Every `write()` is an IPC round trip to the SD card,
  costing around 0.8 ms. newlib's default 1 KiB stream buffer turned creating
  an 8 MB memory card into thousands of them — 6.7 s per card. `fopen_fake()`
  installs a 256 KiB buffer for reads and a 64 KiB one for writes; the smaller
  write buffer bounds how much unflushed data a hard kill can lose.

Frame Generation is available in **Settings > Frame Generation**. You must
provide your own `Lossless.dll` at `/switch/nethersx2/lsfg/Lossless.dll`. LSFG
must be enabled before launching a game so its shader session can be prepared.
Frame Generation starts **Off** in every game and must be enabled manually from
**L + R + Plus > Frame Generation**. Skip Duplicate Frames is automatically
enabled for prepared LSFG sessions so only unique game frames are interpolated.

**Mode** decides the schedule:

- **Auto (2x below 33 FPS)** interpolates every frame, but only while the source
  runs below ~33 FPS, and passes video through untouched above that — twice
  50 FPS is 100 and the panel is 60 Hz, so there would be nothing to show.
- **Fractional (target rate)** interpolates on only *some* frames so the output
  lands on **Target output rate** instead of always doubling. A game at 40 FPS
  aiming for 60 gets one generated frame every two; a game at 47 aiming for 50
  gets one every seventeen. Output can never exceed twice the source rate,
  because one interpolation per source frame is all the bridge produces. The
  cost is one frame of latency plus the GPU work of each generated frame.

Fractional needs the bridge to see *every* source frame even when it does not
interpolate, because it interpolates between the previous source frame and the
current one: a frame it never saw would leave the previous-frame slot stale and
the next interpolation would span the whole gap and land in the wrong place on
the timeline. So non-interpolated presents go through `lsfg_nx_present_capture()`,
which copies the frame into the ping-pong slot and presents it unchanged — an
image copy, far cheaper than interpolation. The source rate is measured during
that capture-only warm-up (16 presents) and then frozen, because once the
display saturates its FIFO back-pressure reads as a slower source, which would
raise the ratio and generate more: positive feedback.

**Flow resolution** offers Eighth (0.125) as well as Quarter and Half; the
bridge clamps the value to 0.125–0.5, so Eighth is the cheapest optical-flow
pass it accepts.

### Diagnostics

Logs go into `/switch/nethersx2/`.

`nethersx2-core.log` is written by every build. It captures the core's own
console: PCSX2 reports every initialisation failure through Bionic's logcat, and
Horizon has nowhere to print it, so without this the emulator dies without ever
saying why. It also records the reason and the calling address whenever the core
calls `abort()` or trips the stack protector, rendering an address inside
`libemucore.so` as `core+0xOFFSET`, and an address in memory this port mapped on
the core's behalf — the recompiler's RX cache, the fastmem mirror, the ashmem
PS2 RAM regions — by naming that region instead of printing a bare pointer.

The file is line-buffered and capped (2 MiB in the shipping build, 8 MiB in the
diagnostic one). Hitting the cap stops `INFO` and below but leaves the file open,
so errors and the death note are always recorded; they are flushed and `fsync`ed
as they are written, so they survive a crash.

CI additionally publishes `NetherSX2-diag.nro` — the same launcher and the same
cores, with the emulator built with `NETHERSX2_VK_DIAGNOSTIC=1`. Install it as
`/switch/NetherSX2.nro` (keeping your `nethersx2/` folder) to also get:

```
nethersx2-vulkan.log     host and Vulkan lifecycle, timestamped
nethersx2-mesa.log       NVK's own log (MESA_LOG_FILE, NVK_DEBUG=wsi,vm)
nethersx2-exception.log  pc/far/esr/sp/fp/lr, only if a CPU exception fires
```

The diagnostic build also turns PCSX2's own logging back on
(`Logging/EnableSystemConsole`, `EnableVerbose`, `EnableEEConsole`,
`EnableIOPConsole`), which the shipping build forces off — verbose emulator
output is megabytes per boot and real time on a Tegra X1. Expect
`nethersx2-core.log` to be far larger and the boot to be slower as a result;
that is the point, since the core's own message is usually the only thing that
names a fault.

The Vulkan log traces every `vkGetInstanceProcAddr` lookup the core makes and
whether it resolved, reports how many physical devices NVK returned and what
they are called, and prints the physical-memory figure NVK sizes its heap from.
A run whose log ends without `host exiting normally`
did not exit cleanly — the last line before the gap names the stage that died.

### How to build

Install the devkitPro Switch toolchain and portlibs:

```sh
pacman -S devkitA64 switch-tools libnx switch-sdl2 switch-sdl2_ttf \
          switch-sdl2_image switch-curl switch-mesa switch-libdrm_nouveau \
          switch-ntfs-3g switch-libpng deko3d
```

Install the NVK driver as a portlib package, which is what the build looks for
first (`pkg-config --exists nxvk`):

```sh
tar -xzf nxvk-vulkan-tegra-x1.tar.gz -C "$DEVKITPRO/portlibs/switch"
```

`nxvk.pc` carries the whole link line, including `--whole-archive` and
`-u,vk_icdGetInstanceProcAddr`. Its `prefix` is baked to
`/opt/devkitpro/portlibs/switch`, so extract it there rather than somewhere
else. An older vendored tree under `vulkan/include` and `vulkan/lib` is still
accepted as a fallback; it exports the public entry points itself, so the
`NETHERSX2_NVK_ICD_ONLY` shim and loader stay off for it.

`NXVK_EXPECTED_SHA256` in the Makefile pins the tarball this tree was validated
against, and CI fails if `berlint-hub/TEST3` serves anything else — the same
protection the core gets from `source/deko3d/dk_profiles.h`. CI clones a pinned
ref (`TEST3_REF` in `.github/workflows/build.yml`, currently
`arena/8b264db8-test3`), but a branch can still move or be force-pushed, so
without a hash the driver can change between two builds of one repo commit. It
did, twice: the tarball moved `arena/92b2b9eb-nxvk` → `arena/068e6f2f-nxvk` and
then back to `arena/92b2b9eb-nxvk` at a newer commit, each time changing the
build run and toolchain image too. Test a new driver with
`make NXVK_EXPECTED_SHA256=<sha>`, and move the pin in the Makefile only after
it has been verified on hardware.

The core is `4248/arm64-v8a/libemucore_tegrax1.so` from that ref: the stock
NetherSX2 v2.2n-4248 image plus five length-preserving Tegra X1 patches from
[mininxd/NetherSX2-patch](https://github.com/mininxd/NetherSX2-patch) — the 0.5×
GS multiplier floor removed, Full Texture Hash Cache on by default in both the
preferences and `GSSettings::Load` paths, and two preset-reset sites NOPed so
applying a performance preset no longer wipes memory cards, achievements and
control bindings. Its sixteen port-patch sites are still pristine, so
`patch_game()` in `source/hooks/patches.c` applies them at load time — the path
that table was written for, since the previously shipped "baked" cores had them
pre-applied and made it a no-op.

Extract the 4248 APK into a folder named `NetherSX2-v2.2n-4248`, or set
`CORES_DIR` to the folder containing it. `--apk-3668` is still accepted for a
two-core tree but is no longer required, and nothing bundles a 3668 image.
Alternatively, extract the supplied inputs without installing the Mesa SDK into
the toolchain:

```sh
python3 tools/prepare_build_inputs.py --apk-4248 /path/to/NetherSX2-v2.2n-4248.apk \
  --mesa-sdk /path/to/mesa-sdk.zip
export MESA_SDK_ROOT="$PWD/out/mesa-sdk"
```

CMake, Ninja, Git, Bison, Flex, Python, and Python Mako are required. The build
downloads pinned libsmb2 and libusbhsfs revisions for the launcher's storage
support, and pinned UAM and SPIRV-Cross revisions for the native Deko3D shader
compiler, which the shader-validation test uses. `build_all.sh` builds the
Vulkan emulator and the launcher, bundles both cores and their resources, then
verifies the result with `tools/check_deko3d_build.py`.

```sh
./build_all.sh
```

### Credits

* The AetherSX2 / PCSX2 developers for the emulator.
* The NetherSX2 maintainers for the patched Android builds.
* fgsfds for the Switch so-loader groundwork reused here.
* TheOfficialFloW for the original Android so-loader lineage.
* Dantiicu for Switch Vulkan driver.
* Slluxx for IconGrabber.
* [Autorun](https://github.com/autorunhq/autorun) for the launcher design reference
  and sliders settings icon.
* devkitPro and fincs for Deko3D and UAM.
* Dolphin-NX, the Dolphin Emulator Project, and PalindromicBreadLoaf for the
  runtime UAM integration and portable native LSFG library adapted here.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with the AetherSX2 or PCSX2 developers. No BIOS,
game images, or the emulator core are distributed here; you must supply your own
BIOS dump and legally-owned game images. We do not condone piracy.

Unless noted otherwise, the source in this repository is under the MIT License
(see LICENSE). The vendored LSFG-VK subset under `third_party/lsfg-vk` is
GPL-3.0-or-later. The native LSFG library under `third_party/lsfg-deko3d` is also
GPL-3.0-or-later, and its adapted UAM bridge/build integration under
`source/deko3d/compiler` is GPL-2.0-or-later. Their original notices are preserved.
The sliders settings icon from Autorun is LGPL-2.1-or-later; its notice and license are included in
`launcher/romfs/icons/`.
