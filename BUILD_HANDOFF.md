# Handoff: build frame-pacing change

## Current repository state

- Repository: `berlint-hub/NetherSX2_nx`
- Fixed session branch: `arena/2e44eb02-nethersx2-nx`
- Implementation commit pushed to that branch: `b28b349` (`Add refresh-synced frame pacing`)
- Worktree was clean after the commit. Do not switch branches or make a PR to `main`.
- No `.nro` was produced in the sandbox.

## What was tried / why Actions has not run

1. `bash build_all.sh` stops before compilation because the local APK core is absent:
   `../NetherSX2-v2.2n-4248/lib/arm64-v8a/libemucore.so`.
   The sandbox also has no devkitA64 cross compiler, libnx libraries, or Mesa NVK archive set, so a local Switch link is not possible.
2. `gh auth status` says the configured GitHub account is `berlint-hub`, but
   `gh workflow run build.yml --ref arena/2e44eb02-nethersx2-nx` returns:
   `HTTP 403: Resource not accessible by integration` on the Actions workflow-dispatch endpoint.
3. The push of `b28b349` succeeded, but it did not create a new Actions run. Do not mistake the old successful run `39510587447` for a build of this change: that run built commit `6245e78`.
4. No credentials were requested or stored. Reconnect GitHub in Arena with permission to dispatch Actions workflows, or have an authorized repository user start the workflow in GitHub's Actions UI. A PAT/token pasted into chat is not needed and must not be requested.

## Immediate Actions build instructions

After GitHub Actions permissions are fixed:

```sh
gh auth status
gh workflow run build.yml --ref arena/2e44eb02-nethersx2-nx
gh run list --branch arena/2e44eb02-nethersx2-nx --limit 5 \
  --json databaseId,headSha,status,conclusion,workflowName,createdAt
```

Confirm the new run's `headSha` is `b28b349...` (or a later commit containing it). The workflow has two jobs: `host-tests` and `switch`. Poll the concrete run using `gh run view <RUN_ID> --json status,conclusion,headSha,jobs` until `status` is `completed`; do not rely solely on `gh run watch` returning. If a job fails, use annotations (workflow logs/artifact blob downloads may be inaccessible from this sandbox):

```sh
gh api repos/berlint-hub/NetherSX2_nx/actions/runs/<RUN_ID>/jobs --jq '.jobs[].id'
gh api repos/berlint-hub/NetherSX2_nx/check-runs/<JOB_ID>/annotations \
  --jq '.[]|"[\(.annotation_level)] \(.message)"'
```

On success, the Actions artifact is named `nethersx2-switch`; the workflow uploads `NetherSX2.nro`, `NetherSX2-diag.nro`, `NetherSX2_nx_vk.nro`, `NetherSX2_nx_vk_diag.nro`, and `nxvk-BUILDINFO.txt`. Download the NRO in the GitHub Actions UI if this sandbox cannot reach the artifact blob host.

The `switch` job clones the pinned `TEST3` branch `arena/8b264db8-test3`, verifies the pinned NXVK SHA (`dae2897b939a8a7d578f7b02cc35b64bcbfa159ec7dbc3205b26e51bf7e6492b`) and the core hash pinned in `source/deko3d/dk_profiles.h`, then builds the Vulkan-only shipped NRO and the final launcher bundle. It also runs `tools/check_ini_keys.py` against the bundled core. The runner provisions devkitPro and the inputs, so no local APK or token should be needed once Actions dispatch is authorized.

## Important backend coverage caveat

This repository's current production bundle is Vulkan-only. The `switch` job compiles the Vulkan production and diagnostic NROs, which exercise the shared SaltySD client, preferences, frontend polling, and Vulkan swapchain path. The host-test job compiles the Deko3D presentation harness (including `source/deko3d/dk_presentation.cpp`), but it does **not** build the complete `RENDERER=DK` emulator. The changed `dk_core.cpp` / `dk_gpu.cpp` therefore need a separate full Deko3D target build if the request is to validate that optional backend too. That target needs devkitA64/libnx/deko3d plus the three archives/objects listed in the Makefile under `DEKO3D_LIBS` (`liblsfg.a`, `liblsfg_spirv_cross.a`, `uam_isolated.o`); current `.github/workflows/build.yml` deliberately does not configure/build that target.

## Already-passed local checks

- All five Deko3D host harnesses passed: DMA/transfer (17), render-target (25), vertex format (16), render state (59), presentation (269).
- `source/saltysd_display.c` and `source/prefs.c` passed host GCC syntax checks with `-Wall -Wextra -Werror` against the available libnx headers.
- A focused prefs-save override test passed; `git diff --check` was clean before commit.
