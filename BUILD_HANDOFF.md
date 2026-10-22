# Handoff: TEST3 spinrelax + NXVK build

## Follow-up candidate (2026-10-22)

- Requested action: rebuild the Switch package against the updated NXVK supplied by TEST3 branch `arena/8b264db8-test3`.
- NXVK source: `berlint-hub/nxvk` branch `arena/92b2b9eb-nxvk`, commit `c3745fe5b797dde04745dae90fb96cda7add03c6`; Mesa 26.2.2; source build run `39667587272`.
- New `nxvk-vulkan-tegra-x1.tar.gz` SHA-256: `3942e4318ebbbd772d2d76ccfef4f376e8e194c942d4eba80444ea5e9abcc65f`.
- Core remains the same spinrelax image, SHA-256 `b2dd3b6bf7c7a90fdfc80456b1b373d4b2152e74057b6b68bc03b801535c5f01`; the TEST3 ref is now at `b5c2ab61775e6b68aa1c1ed41eedd082802cde0e`.
- NetherSX2_nx build commit `1815b417ba455a51cb21d54afea88bf7ee58ca92`; GitHub Actions run `39795109390` completed successfully (host-tests and Switch build).
- Artifact `nethersx2-switch` is available from the Actions run page below; uploaded ZIP size: 43,864,473 bytes. This is the new-driver package for the planned on-device A/B test.

### Device log from the new driver

- Diagnostic build `1815b41-run39795109390` boots Gran Turismo 3 (PAL) on Tegra X1/NVK gm20b. `active=1`, `nethfps_rc=0`, `shared_ready=1`; SaltyNX Core 2.1.11 accepts the client and returns the shared-memory handle. Vsync remains OFF and the core reports PAL mode.
- The first run is cold: NVK rejects the old pipeline cache UUID and removes the old cache files; the shader cache starts empty. Early results (24.77 FPS, then 39.69/47.80) and shader/pipeline hitches are warm-up, not a steady-state driver comparison.
- Once warm, long windows repeatedly settle around 50.17–50.29 FPS, consistent with the PAL cap. Heavy scenes still dip into the mid/high 40s and show CPU work on EE/GS/MTVU; queue-submit/present and GPU-fence waits are generally small. This does not prove a speedup versus the previous driver; the prior run also reached about 50.2 FPS and there is no matched A/B capture.
- The shared target works, but it is not perfectly steady: `fpslocker_target` and SaltyNX refresh changes include 40/45/50/55/60 (and brief startup 25). That suggests the 16-present (~0.3 s at 50 FPS) estimator window plus two one-second observations can follow short scene/load dips. If visible refresh hunting is confirmed, test a longer estimator window and stronger hysteresis before changing driver code.

### Save-state failure (reproduced with display sync disabled)

- Device reproduced the app-level crash with `SyncFrameLimitToDisplay=false` (`display-sync enabled=0`). This shows the live SaltyNX IPC/shared-memory polling and FPSLocker target publication were inactive; it does not rule out a regression in the wrapper/Vulkan hooks or other code shipped in the same FPSLocker-era build.
- The core starts `SaveStateSlot0`, saves SPU2/PAD, logs `Saving GS`, then the host exception handler records a data abort: `pc=0x1ef7877c80`, `far=0x40`, `esr=0x92000005`. This is a null-adjacent translation fault during graphics-state serialization, not a console reset. `_SC_PHYS_PAGES` reports 3,185/3,285 MiB used, but the wrapper preallocates a ~2.9 GiB heap, so that figure is not free-heap headroom; an unchecked allocation failure cannot be ruled in or out from this log. The PC is outside the logged `libemucore.so` base.
- The core log also warns `MTVU speedhack is enabled, saved states may not be stable`; retain this as log context only. User explicitly rules out the core and Vulkan, so do not make MTVU/NVK the next investigation.
- The pre-test build reported `State saved` immediately after requesting the save, while core serialization continued. The current test waits before showing the toast; the new wrapper markers will establish whether that wait actually returns after serialization.

### Build/source audit (2026-10-22)

- User explicitly rules out the core and Vulkan as the cause; keep the next investigation on the wrapper/overlay path and do not continue proposing core/driver tuning as the next test.
- Wrapper test commit `d47e6e3` passed host tests but its first Switch job stopped before compilation: TEST3's branch had moved to `1e4fe452...` and supplied NXVK SHA `1b935a4a...` instead of pinned `3942e431...`. Commit `76020bd` pins TEST3 commit `b5c2ab61775e6b68aa1c1ed41eedd082802cde0e`; rebuild run `39816644241` then passed both host-tests and Switch build. Artifact `nethersx2-switch`, ID `12443670792`, size 43,864,698 bytes.
- The original overlay path invoked `saveStateSlot`, immediately closed the menu, and posted `State saved`; when OSD restoration is active, `quick_menu_close()` also calls `nl.applySettings()`. The flush-wait test was ineffective: in run `39819404433`, `flush wait returned` at 1792691080.310, before core serialization began at .313. That log showed display sync active (`enabled=1`), unlike the earlier disabled-sync reproduction.
- Isolation build commit `8e11faf` passed CI run `39826020821` (host tests and Switch build); artifact `nethersx2-switch`, ID `12446418050`, 43,864,915 bytes. The user's device log from this build shows `calling saveStateSlot` at 1792694945.863, return at .866, status OSD at .869, then core `Saving state` at .894 and `Saving GS` at .918; data abort follows (`pc=0x1d4cfc2cd0`, `far=0x40`, `esr=0x92000006`). The quick menu was deliberately left open, so `quick_menu_close()` and its possible `nl.applySettings()` are not required to trigger this crash. Display sync was active and the reported PAL cadence settled around 50 FPS.
- Next wrapper-only isolation removes the post-save `quick_menu_status()` OSD/JNI call; keep the menu's existing selected Save item visible, and make no claim that the save completed. This tests whether that immediate post-request OSD call interacts with queued save startup. The variant is commit `e217b6d`; CI run `39835583880` passed host tests and Switch build. Artifact `nethersx2-switch`, ID `12451081527`, size 43,864,897 bytes. On device, check whether it still aborts before the menu is manually closed; do not redirect the work to core or Vulkan.
- Compared the two latest Switch builds: run `39789988729` (`2c4bac7`, NXVK archive SHA `1480973e...`) and run `39795109390` (`1815b41`, NXVK archive SHA `3942e431...`). Between these commits the only runtime-input change is the pinned NXVK package; the NetherSX2 runtime source files (`main.c`, Vulkan hooks, display-sync module, prefs, Deko3D, libc/thread shims) are byte-identical. Core input is also identical: spinrelax SHA `b2dd3b6b...`.
- The NXVK source delta is limited to `src/vulkan/wsi/wsi_switch.c` (equivalent block-linear offset math rewritten as shifts/masks plus NEON 16-byte copies/prefetch for scanout) and `src/util/disk_cache.c` (Horizon disk-cache worker count becomes `nr_cpus - 1`, clamped 1..4, instead of fixed 4). Neither touches the emulator's save-state implementation; the disk-cache worker change is the more plausible concurrency-related driver delta, but there is no evidence yet that it caused this fault.
- The save-slot dispatch in `source/main.c` is unchanged from the `a78cb58` baseline. In the current build with `SyncFrameLimitToDisplay=false`, display-sync configure leaves it disabled; polling returns immediately; present sampling returns immediately; and shutdown has no shared-memory mapping to restore. This rules out active SaltySD IPC/writes, not a regression in the compiled present hook or another component shipped in the same era.
- User identified an earlier candidate package: run `39749459178`, commit `1b2cb55`, artifact ID `12412542836` (successful build, 43,864,005 bytes). It already contains FPSLocker/display-sync publishing code, but predates later IPC/renderer changes and uses the stock core (`be64362a...`) plus older NXVK (`dae2897b...`); current uses spinrelax (`b2dd3b6b...`) plus newer NXVK. If this package is confirmed as the last save-state-known-good, it is a useful historical behavioral baseline, but not a one-variable A/B.
- The nearest pre-display-sync build (`6245e78`, run `39510587447`) has the same stock-core/older-NXVK mismatch, so it is likewise not a clean binary A/B. No available note yet confirms save-state success on runs `397494`, `397899`, or `397951`; do not treat compile/IPC success as save-state validation.
- The `397899`/`397951` and older `397494` package comparisons remain historical audit data only. User explicitly rules out core and Vulkan; keep the active investigation on the save overlay/wrapper lifecycle. The user-identified `39749459178` artifact remains available as a behavior reference if needed.
- Device log received for the correct isolation build `e217b6d-run39835583880` (run `39835583880`, artifact ID `12451081527`): wrapper markers confirm `saveStateSlot` returned and the overlay was left open; importantly, there is no `Save requested; wait before closing the menu` toast. Nevertheless the core then logs `Saving state to slot 1`, `Saving SPU2`, `Saving PAD`, `Saving GS`, and the same null-adjacent abort follows (`pc=0x1cfba83c90`, `far=0x40`, `esr=0x92000005`). Therefore the post-save `quick_menu_status()`/OSD JNI call is not required for the crash. This is now a valid device result, unlike the previous `8e11faf` log.
- Device log for `4be8de4-run39841744352` confirms the expected `quick-menu input/update suspended` marker, followed by `Saving SPU2`, `Saving PAD`, `Saving GS`, then the same abort (`pc=0x0cb26a4c80`, `far=0x40`, `esr=0x92000005`). Thus neither the post-save OSD/JNI status call nor subsequent quick-menu input/update handling is necessary for the crash. The fault still appears about 70 ms after the request; the overlay screen was left static and no core save-completion marker occurred.
- The correct `0c3d7e0-run39845036159` device log contains `host input/poll loop paused for 500 polls` at 1792700292.646; no resume marker appears because the fatal abort follows about 105 ms later. Core proceeds through `Saving SPU2`, `Saving PAD`, `Saving GS`, creates worker thread #4, then aborts (`pc=0x1b1538dd50`, `far=0x40`, `esr=0x92000005`). This proves the crash persists while the wrapper host input/poll loop is parked; together with earlier tests, OSD status, quick-menu event handling, and ongoing host-loop callbacks are not required. No device test has yet passed.
- Next diagnostic adds wrapper-only trace points around core thread #4's pthread bridge: trampoline entry, fake TLS address, pre-target handoff, target return, and parent-side `pthread_create` result. Commit `eb7cbf5`; CI run `39848078078` passed host-tests and Switch build. Artifact `nethersx2-switch`, ID `12456406478`, 43,865,763 bytes. Expected header: `eb7cbf5-run39848078078`. This distinguishes a fault in the wrapper's thread setup from a fault after it hands off the asynchronous save worker; it is instrumentation, not a fix.

## Previous result

- Repository: `berlint-hub/NetherSX2_nx`
- Fixed branch: `arena/2e44eb02-nethersx2-nx`
- Build commit: `2c4bac706902c40d42d8776141f59720adeb2ad9`
- GitHub Actions run: `39789988729` — **host-tests and Switch build both passed**.
- The successful run produced the `nethersx2-switch` artifact. This sandbox cannot retrieve it because GitHub redirects downloads to `productionresultssa15.blob.core.windows.net`, outside the allowed network hosts. Download it from the run's Artifacts section:
  https://github.com/berlint-hub/NetherSX2_nx/actions/runs/39789988729
- Device test with this build was successful for the IPC/shared-memory path: `active=1`, `nethfps_rc=0`, `shared_ready=1`; the published FPSLocker target reached 50 while PAL presentation settled around 50.2 FPS. On-device validation of the exact NXVK/core performance and physical panel response remains useful.

## Exact inputs in the build

- Core: `TEST3/4248/arm64-v8a/libemucore_tegrax1_spinrelax.so`
  - SHA-256: `b2dd3b6bf7c7a90fdfc80456b1b373d4b2152e74057b6b68bc03b801535c5f01`
  - The build's profile check verified all 29 GSDevice and 31 HostDisplay vtable entries against `source/deko3d/dk_profiles.h`.
- NXVK: `TEST3/nxvk-vulkan-tegra-x1.tar.gz`
  - SHA-256: `1480973e71cfd54eb430e7c6b48f9c5ce60a674e54bb6d44d0c1c8de412de0de`
  - NXVK commit `b53eb3308c510dde96930c07090a907af92a696d`, Mesa 26.2.2, build run `39497258880`.
- Both are pinned in the repository and checked by CI. The resulting production bundle was 47,486,407 bytes; the Vulkan emulator NRO was 16,857,987 bytes and the Vulkan diagnostic NRO was 16,882,563 bytes.
- Device log build `2c4bac7-run39789988729` reports `nethfps_rc=0`, `shared_ready=1`, and later `fpslocker_target=50` while PAL presentation settles near 50.2 FPS. This confirms the 32-byte `GetSharedMemoryHandle` request fixes the IPC handshake and publishes the emulator cadence without enabling Vsync.
- On first launch, NVK rejected and removed the old pipeline/shader caches because the driver UUID changed. Startup shader compilation caused early hitches; later performance settled near 50 FPS.

## Build history and checks

- Run `39789339947` passed host tests but stopped before compilation because the supplied TEST3 NXVK tarball had SHA-256 `1480973e...` while the repository still pinned the older `dae2897b...` archive.
- Commit `2c4bac7` updated the NXVK pin, selected the requested spinrelax core, and updated its profile hash. Run `39789988729` then passed the complete Switch build and all host tests.
- Additional local checks passed: spinrelax core profile/vtable validation, launcher INI-key compatibility against the new core, input SHA verification, and `git diff --check`.
- The sandbox has no local devkitA64/libnx/NVK toolchain, so the NRO was compiled on GitHub Actions rather than locally.
- Use `gh run download 39789988729 --repo berlint-hub/NetherSX2_nx` from a network that can reach GitHub's artifact blob host, or download `nethersx2-switch` in the Actions UI.

## Scope

This is the Vulkan/NVK production build. It keeps the previously implemented PAL/NTSC core limits and output-side display-sync behavior. The IPC/shared-memory publication has now been confirmed on-device; actual panel response should still be confirmed separately. The build does not establish RAM-bandwidth gains from the spinrelax core/NXVK combination.
