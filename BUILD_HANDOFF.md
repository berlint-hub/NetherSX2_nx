# Handoff: TEST3 spinrelax + NXVK build

## Follow-up candidate (2026-10-22)

- Requested action: rebuild the Switch package against the updated NXVK supplied by TEST3 branch `arena/8b264db8-test3`.
- NXVK source: `berlint-hub/nxvk` branch `arena/92b2b9eb-nxvk`, commit `c3745fe5b797dde04745dae90fb96cda7add03c6`; Mesa 26.2.2; source build run `39667587272`.
- New `nxvk-vulkan-tegra-x1.tar.gz` SHA-256: `3942e4318ebbbd772d2d76ccfef4f376e8e194c942d4eba80444ea5e9abcc65f`.
- Core remains the same spinrelax image, SHA-256 `b2dd3b6bf7c7a90fdfc80456b1b373d4b2152e74057b6b68bc03b801535c5f01`; the TEST3 ref is now at `b5c2ab61775e6b68aa1c1ed41eedd082802cde0e`.
- NetherSX2_nx build commit `1815b417ba455a51cb21d54afea88bf7ee58ca92`; GitHub Actions run `39795109390` completed successfully (host-tests and Switch build).
- Artifact `nethersx2-switch` is available from the Actions run page below; uploaded ZIP size: 43,864,473 bytes. This is the new-driver package for the planned on-device A/B test.

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
