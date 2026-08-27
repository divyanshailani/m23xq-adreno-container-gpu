# Experiments log

Format: one entry per attempt, with evidence. Never delete a failed entry —
dead ends are as valuable as successes.

## 2026-08-27 — EXP-000: fastfetch GPU visibility

**Goal:** make fastfetch show the GPU in the container.

**Result:** PARTIAL / explained-away.

- `enable_gpu_mode=0 -> 1` in container.config: mirrors renderD128. Required
  container restart to take effect.
- `mknod /dev/dri/card0 c 226 0` inside container: makes sysfs enumeration happy.
- `apt install libdrm2 mesa-utils`: needed, base image had no graphics stack.
- fastfetch then shows `GPU: Mesa llvmpipe` — the software fallback.

**Learning:** on this 4.19 kernel the DRM nodes are the display pipe (mdss_mdp),
not the GPU. Adreno lives on kgsl. Stock Mesa cannot use kgsl → llvmpipe is
expected. fastfetch is the wrong success metric; Phase 1 (lfdevs Mesa) is the
real path. See docs/01 and docs/02.

## 2026-08-27 — EXP-001: lfdevs Mesa KGSL install (pending)

Plan documented in docs/02; execution next session.

## 2026-08-27 — EXP-001: lfdevs Mesa KGSL install — **SUCCESS**

**Goal:** real Adreno 619 visible to graphics APIs in the container.

**Steps:**
1. Downloaded `mesa-for-android-container_26.3.0-devel-20260824_ubuntu_noble_arm64.tar.gz`
   inside the container (SHA-256 starts `06c31d6d...`), saved the 70-file manifest
   to `/tmp/mesa-files.txt` for clean uninstall.
2. Extracted to `/`, `ldconfig`.
3. `apt install vulkan-tools`.
4. `MESA_LOADER_DRIVER_OVERRIDE=kgsl vulkaninfo --summary`.

**Result:**

```
GPU0:
    apiVersion         = 1.3.359
    driverVersion      = 26.2.99
    vendorID           = 0x5143
    deviceID           = 0x6010900
    deviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
    deviceName         = Turnip Adreno (TM) 619
    driverID           = DRIVER_ID_MESA_TURNIP
    driverName         = turnip Mesa driver
    driverInfo         = Mesa 26.3.0-devel (git-98f3d6229d)
```

The container now has **hardware Vulkan on the real Adreno 619** through the
KGSL backend. This enumerating at all means the kgsl ioctls worked end-to-end
(device id 0x6010900 = Adreno 619).

**Next:** Phase 2 — OpenGL path (freedreno direct vs Zink-on-Turnip), glmark2
benchmark vs the llvmpipe baseline; then Phase 3 display (Termux:X11 / Anland),
Phase 4 mirror to Mac.

## 2026-08-27 — EXP-002: OpenGL stack verification — **SUCCESS**

`MESA_LOADER_DRIVER_OVERRIDE=kgsl eglinfo -B` (via GBM/surfaceless platform):

```
OpenGL core profile renderer: FD619 — version 4.6 (Core Profile) Mesa 26.3.0-devel
OpenGL compatibility profile: FD619 — 4.6 (Compatibility Profile)
OpenGL ES profile renderer: FD619 — OpenGL ES 3.2, GLSL ES 3.20
```

**Full freedreno GL stack on the real Adreno 619 — no Zink needed.** Notes:
- The X11/GLX path (xvfb + glmark2) fails with `MESA-LOADER: failed to retrieve
  device information` — expected: kgsl backend renders via GBM/surfaceless, not
  GLX. X11 apps will need Zink-on-Turnip or a Wayland/native window system (Phase 3).
- llvmpipe baseline (LIBGL_ALWAYS_SOFTWARE=1, xvfb, single scene): build ~102 FPS.

## 2026-08-27 — EXP-003: headless Vulkan compute benchmark — **SUCCESS**

vkmark KMS backend fails (`Failed to open active VT` — no VT inside the container;
expected). Wrote a custom headless Vulkan compute benchmark instead
(`scripts/gpubench.c` + `scripts/flops.comp`): 96 MB buffers x 3, workgroup 256,
200 dispatch iterations of a dependency-chain FMA loop (256 iters x 2 FLOPs per
invocation).

```
GPU0: Turnip Adreno (TM) 619
mem type: 0 (device-local preferred)
time: 114.785 s (200 iterations)
GFLOPS: 22.45 (sustained, latency-bound shader — not peak)
```

Build/run (inside container):
```
glslangValidator -V flops.comp -o flops.spv   # + spv2inc.py to make the .inc
gcc -O2 -o gpubench gpubench.c -lvulkan
MESA_LOADER_DRIVER_OVERRIDE=kgsl \
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/freedreno_icd.json ./gpubench
```

Gotchas learned (cost debugging time — remember):
- `VK_ICD_FILENAMES` must point at `freedreno_icd.json`; the loader otherwise
  only surfaces `gfxstream_vk_icd.json` (Ubuntu's mesa-vulkan-drivers provides
  the others but the default search finds gfxstream first in this image).
- The GPU needs the kgsl override AND the freedreno ICD together.
- File transfer to the container goes through `/sdcard` (visible at
  `/storage/emulated/0` inside; container /tmp is isolated).
