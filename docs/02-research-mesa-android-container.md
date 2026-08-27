# Research: Mesa for Android containers (lfdevs)

Source: https://github.com/lfdevs/mesa-for-android-container

## Why this project

- Builds Mesa with the **KGSL backend** for Freedreno (GL/GLES) and Turnip
  (Vulkan), specifically for containers on Android: *"PRoot, Chroot, Droidspaces,
  and LXC containers"* — Droidspaces is explicitly supported.
- Pre-built packages for **Ubuntu 24.04 (noble)**: release
  `25.0.7-0ubuntu0.24.04.2` (apt packages) and `.tar.gz` builds with suffix
  `ubuntu_noble_arm64` (direct extraction to `/`).
- For Adreno 6xx, the Freedreno driver can serve OpenGL, OpenGL ES, AND Vulkan
  directly (no Zink translation needed) — "significantly improving GPU
  utilization".
- KGSL backend authored by Lucas Fryzek (Mesa upstream), ported/stabilized for
  containers by xMeM / Robert Kirkman.

## Compatibility notes for Adreno 619

- Tested table lists Adreno 660+ — 619 is NOT in the confirmed table.
- The project's guidance: search the GPU model in `freedreno_devices.py`; a
  complete device definition means "likely supported". Adreno 619 is a standard
  A6xx part (same generation family as 660) — expect support, verify empirically.
- Known issue pattern (their #32): on some 6xx devices (adreno 630 report),
  freedreno GL worked via kgsl but Turnip Vulkan needed the *unpatched* turnip
  variant — if standard release Vulkan fails, try the `turnip-` prefixed release.

## Install plan (Phase 1)

```bash
# inside the Ubuntu 24.04 container:
# option A: apt packages from the release
# option B: tar.gz direct extraction (preferred for first test — easy to undo):
#   tar -zxvf mesa-for-android-container_*_ubuntu_noble_arm64.tar.gz -C /
#   ldconfig
# env for every GPU-using invocation:
MESA_LOADER_DRIVER_OVERRIDE=kgsl <app>
```

Uninstall path (documented for reversibility): extract file list from the
tarball, `rm` them, then `apt install --reinstall` stock Mesa packages.

## Verification ladder (Phase 1)

1. `MESA_LOADER_DRIVER_OVERRIDE=kgsl eglinfo` — expect a freedreno device
2. `vulkaninfo --summary` — expect "Qualcomm Adreno 619" / freedreno driver
3. `vkcube` smoke test
4. Phase 2: `glmark2` / `glmark2-es2` numbers (compare against llvmpipe baseline)
