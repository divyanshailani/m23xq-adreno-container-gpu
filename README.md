# m23xq Adreno Container GPU

Real GPU acceleration (Adreno 619) inside a Droidspaces Ubuntu 24.04 container on a
Samsung Galaxy F23 5G (SM-E236B, Snapdragon 750G), with the end goal of a
GPU-accelerated Linux desktop mirrored to a Mac.

## Hardware / platform facts (verified on-device)

| Item | Value |
|---|---|
| SoC | Snapdragon 750G (SM7225) |
| GPU | Adreno 619 (A6xx family, freedreno device id 619) |
| Kernel | Custom Samsung 4.19 (m23xq) + rsuntk KernelSU manual-hook build |
| Android | 14 / OneUI 6.1, rooted |
| Container runtime | Droidspaces v6.5.0 (LXC-style namespaces) |
| Container | Ubuntu 24.04.4 LTS aarch64 |

### Key kernel constraint

On this 4.19 Samsung kernel the Adreno GPU is exposed through the **KGSL** driver
(`/dev/kgsl-3d0`), NOT through a DRM render node. The `/dev/dri/card0` +
`renderD128` nodes belong to `msm_drm` / `qcom,mdss_mdp` — the **display
controller** (DSI panel pipe), not the GPU. This means:

- `fastfetch` inside the container will never show "Adreno 619" via the DRM path
  (there is no DRM GPU node on this kernel — this is expected, not a bug)
- GPU access for real work goes through **Mesa freedreno (GL) / Turnip (Vulkan)
  with the KGSL backend**

## Status

See [docs/experiments-log.md](docs/experiments-log.md) for the full try/error history.

- [x] Phase 0: Recon — kgsl node mirrored into container, DRM nodes identified as display-only
- [x] Phase 1: Mesa freedreno/Turnip (KGSL backend) install + Vulkan smoke test — **Adreno 619 enumerated via Turnip**
- [x] Phase 2: freedreno FD619 GL 4.6 + ES 3.2; Vulkan compute bench 89.6 GFLOPS @ 800MHz with clock keeper (EXP-002/003/004)
- [ ] Phase 3: X/Wayland desktop in container (Termux:X11 or Anland as display)
- [ ] Phase 4: Mirror the desktop to a Mac (VNC / waypipe / screen sharing) with GPU accel

## Why a repo

This is a multi-phase try/error project. Each experiment, failure, and fix gets
logged in [docs/experiments-log.md](docs/experiments-log.md) so we never re-walk
a dead end.
