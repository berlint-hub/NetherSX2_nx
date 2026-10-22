# Handoff: TEST3 spinrelax + NXVK build

## Current result

- Repository: `berlint-hub/NetherSX2_nx`
- Fixed branch: `arena/2e44eb02-nethersx2-nx`
- Build commit: `2c4bac706902c40d42d8776141f59720adeb2ad9`
- GitHub Actions run: `39789988729` — **host-tests and Switch build both passed**.
- The successful run produced the `nethersx2-switch` artifact. This sandbox cannot retrieve it because GitHub redirects downloads to `productionresultssa15.blob.core.windows.net`, outside the allowed network hosts. Download it from the run's Artifacts section:
  https://github.com/berlint-hub/NetherSX2_nx/actions/runs/39789988729
- Device validation is still needed; this is a reproducible build candidate, not yet a hardware-validated release combination.

## Exact inputs in the build

- Core: `TEST3/4248/arm64-v8a/libemucore_tegrax1_spinrelax.so`
  - SHA-256: `b2dd3b6bf7c7a90fdfc80456b1b373d4b2152e74057b6b68bc03b801535c5f01`
  - The build's profile check verified all 29 GSDevice and 31 HostDisplay vtable entries against `source/deko3d/dk_profiles.h`.
- NXVK: `TEST3/nxvk-vulkan-tegra-x1.tar.gz`
  - SHA-256: `1480973e71cfd54eb430e7c6b48f9c5ce60a674e54bb6d44d0c1c8de412de0de`
  - NXVK commit `b53eb3308c510dde96930c07090a907af92a696d`, Mesa 26.2.2, build run `39497258880`.
- Both are pinned in the repository and checked by CI. The resulting production bundle was 47,486,407 bytes; the Vulkan emulator NRO was 16,857,987 bytes and the Vulkan diagnostic NRO was 16,882,563 bytes.

## Build history and checks

- Run `39789339947` passed host tests but stopped before compilation because the supplied TEST3 NXVK tarball had SHA-256 `1480973e...` while the repository still pinned the older `dae2897b...` archive.
- Commit `2c4bac7` updated the NXVK pin, selected the requested spinrelax core, and updated its profile hash. Run `39789988729` then passed the complete Switch build and all host tests.
- Additional local checks passed: spinrelax core profile/vtable validation, launcher INI-key compatibility against the new core, input SHA verification, and `git diff --check`.
- The sandbox has no local devkitA64/libnx/NVK toolchain, so the NRO was compiled on GitHub Actions rather than locally.
- Use `gh run download 39789988729 --repo berlint-hub/NetherSX2_nx` from a network that can reach GitHub's artifact blob host, or download `nethersx2-switch` in the Actions UI.

## Scope

This is the Vulkan/NVK production build. It keeps the previously implemented PAL/NTSC core limits and output-side display-sync behavior. The new build does not establish on-device RAM-bandwidth gains or FPSLocker behavior; test those on the Switch after downloading the artifact.
