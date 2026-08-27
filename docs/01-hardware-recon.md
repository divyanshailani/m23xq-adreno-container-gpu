# Phase 0: Hardware recon (2026-08-27)

## Host side (Android)

```
/dev/dri/card0        -> msm_drm, qcom,mdss_mdp (DISPLAY controller, DSI-1 = panel)
/dev/dri/renderD128   -> msm_drm, qcom,mdss_mdp (same display pipe — NOT the GPU)
/dev/kgsl-3d0         -> the actual Adreno 619 GPU (KGSL driver)
ro.hardware.egl       -> adreno
ro.hardware.vulkan    -> adreno
```

Conclusion: this kernel (Samsung 4.19 `msm-5.4`-era tree) predates the mainline
`msm DRM` GPU support; Adreno is kgsl-only. All GPU work must use Mesa's KGSL
backends.

## Droidspaces container config (relevant keys)

```
enable_hw_access=1    # mirrors /dev nodes incl. /dev/kgsl-3d0 (verified present)
enable_gpu_mode=1     # mirrors /dev/dri/renderD128 (display pipe only on this kernel)
enable_virgl=0        # off — software virtual GL renderer, not wanted
enable_termux_x11=0   # display output option for later phases
```

After `enable_gpu_mode=1` the container sees:
- `/dev/kgsl-3d0` (group `droidspaces-gpu`) — the important one
- `/dev/dri/renderD128` — display pipe render node
- NOT `/dev/dri/card0` — can be added manually via `mknod /dev/dri/card0 c 226 0`
  (needed only for sysfs GPU enumeration tools like fastfetch; cosmetic here)

## fastfetch "no GPU" mystery — resolved

Two compounding causes:
1. Base Ubuntu server ships without libdrm/Mesa — fastfetch had no way to query
   anything. Fix: `apt install libdrm2 mesa-utils`.
2. With libdrm present, the only DRM device is the display pipe, so Mesa reports
   `llvmpipe` (software). The real GPU is on kgsl, which stock Mesa never looks at.

So: fastfetch showing llvmpipe is the *correct* result for stock Mesa on this
kernel. Seeing real Adreno output requires Phase 1 (freedreno/Turnip with the
KGSL backend), after which `eglinfo` / `vulkaninfo` / `glmark2` become the real
indicators — not fastfetch.
