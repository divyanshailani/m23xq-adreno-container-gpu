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

## 2026-08-27 — EXP-004: benchmark honesty review + GPU clock cap investigation — **SUCCESS**

A critique of EXP-003 (22.45 GFLOPS vs Adreno 619's ~486 GFLOPS theoretical
peak) prompted a rigorous re-examination. Findings:

### 1. The shader matters (v2 ILP benchmark)
Original shader was a serial dependency chain (latency-bound). v2 uses 8
independent accumulator chains (full ILP):

| Shader | Clock | GFLOPS |
|---|---|---|
| v1 serial (EXP-003) | 430 MHz (stock cap) | 22.45 |
| v2 ILP x8 | 430 MHz (stock cap) | 52.39 |
| v2 ILP x8 | **800 MHz (clock keeper)** | **89.63 / 89.54** (reproducible) |

### 2. The real story: Samsung's 430 MHz GPU clamp

- Silicon supports it: `lagoon-gpu.dtsi` (SM7225/Adreno 619) speed-bin 169
  table tops at **800 MHz** (850 in higher bins); `/sys/.../gpu_available_frequencies`
  lists 800 MHz on the live device.
- But `max_gpuclk` ships as **430 MHz** and a Samsung userspace component
  (thermal policy / power HAL — thermal-engine.conf itself contains only modem
  sections) actively *resets* `max_gpuclk` back to 430 after writes, though
  `devfreq/max_freq` stays as set.
- KonaBess-style dtb editing is unnecessary for us: we own the kernel source
  and the runtime sysfs cap is writable by root. The reset is periodic, not
  enforcement of a fused limit.

### 3. The fix: GPU clock keeper (`boot-completed.d/91-gpu-clock-keeper.sh`)

A 2-second loop re-asserting `max_gpuclk=800MHz` (+ devfreq max), started at
boot via the ksud boot-completed stage (same mechanism as the integrity stack).
Runs on the phone (host side), not in the container. Stop anytime with
`touch /data/adb/gpu-clock-stop` or by removing the script.

**Result: 89.6 GFLOPS sustained — 4x EXP-003's number and ~18% of the 486
GFLOPS theoretical peak.** For a latency-bound FMA-loop shader on a mobile
iGPU with no vendor tuning, this is a sane fraction of peak (peak assumes
perfectly pipelined independent FMAs; real dependency chains + shared memory
bandwidth + mobile thermals land well under it). The GB/s figure (0.31–0.53)
confirms the shader is compute-bound, not memory-bound — as designed.

### Thermal note
GPU temp during sustained 800 MHz load: 47°C → 65°C after ~2 minutes.
Comfortably below throttle territory (~95°C); the stock cooling handles it.

## 2026-08-27 — EXP-005: container GPU overhead + Samsung CPU cap check — **SUCCESS (both answers)**

Two questions before Phase 3: (a) does Droidspaces' "mirror GPU nodes" mode add
overhead vs bare-metal GPU access? (b) has Samsung capped the CPU?

### (a) Container GPU overhead: ~0%

Method: the GPU path is a privileged ioctl pipe (`/dev/kgsl-3d0`); any per-call
container overhead (namespace/device-cgroup checks) would show up as per-dispatch
cost that grows relative to compute as workload shrinks. So: run the v2 ILP
benchmark at three workload sizes spanning 1000x:

| Workload | Per-dispatch compute | GFLOPS |
|---|---|---|
| Full: 96 MB buffers, 200 iters | ~550 ms | 89.5 |
| Small: 1 MB buffers, 2000 iters | ~10 ms | 89.11 |
| Micro: 64 KB buffers, 5000 iters | ~0.2 ms | 84.82 |

Throughput is flat across the range (only -5% at the extreme micro end, where
per-dispatch CPU-side submission cost is ~µs and starts to matter for *any*
Vulkan app, container or not). If the container added meaningful per-io or
per-dispatch overhead, the micro case would have collapsed — it didn't.
**The mirrored /dev/kgsl-3d0 behaves like a direct pipe; container tax on the
GPU path is not measurable.**

### (b) CPU cap: none

- `policy0` (A55 little, cpus 0-5): `scaling_max_freq` = **1804800** (1.8 GHz — full spec)
- `policy6` (A77 big, cpus 6-7): `scaling_max_freq` = **2208000** (2.2 GHz — full spec)
- Under a sustained busy-loop load on all 8 cores, both clusters *reach* their
  maxes simultaneously: little = 1804800, big = 2208000. Verified live.
- Samsung's extra `cpufreq_limit` node looks alarming at first:
  `ltl_max_freq=902400` (902 MHz), `ltl_min_lock=624000`, `hmp_boost_type=2`,
  plus a `user_min 1248000` request row. But the load test proves it is **not
  an active clamp** — the little cluster runs at 1804 MHz with that limit row
  present. It's scheduler-guidance bookkeeping (HMP boost hints for interactive
  ramp-up), and the `user_min` request had already been withdrawn by the time
  of the test (requests table empty).

**Conclusion: Samsung caps the GPU (430 MHz, bypassed in EXP-004) but not the
CPU.** Both CPU clusters run at full SD750G spec; the GPU clock keeper remains
the only frequency intervention needed for Phase 3/4.

## 2026-08-28 — EXP-006: X11 desktop in container — **SUCCESS**

Full display pipeline: Droidspaces container → Termux:X11 → phone screen.

- Termux:X11 nightly universal APK installed; X server started headless via
  `app_process` + shell-loader (loader.apk) with the full Java env from the
  proven integrity-stack script (BOOTCLASSPATH etc).
- **Socket placement is the trick**: X server TMPDIR = `/data/data/com.termux/files/usr/tmp`
  (the path Droidspaces' `enable_termux_x11` bridge expects); XKB_CONFIG_ROOT
  staged at `/data/local/tmp/xkb` (host-local copy — breaks the container/X-server
  start-order circularity: server no longer needs the container rootfs mounted).
- Boot order that works: stop container → start X server → start container
  (bridge bind-mounts `X5` into the container's tmpfs `/tmp/.X11-unix/`).
  WARNING: container `stop` wipes the X5 socket — restart the X server between
  stop and start.
- Result: `xset`/`xrandr` work in-container (1080x2106 @ 119.89Hz),
  **glxgears ~104 FPS on FD619 (freedreno GL 4.6, DRI3, direct rendering)**.
- **Wayland nesting proven**: `weston --backend=x11-backend.so` runs on :5 with
  FD619 GLES 3.2 — the chain for Hyprland (aquamarine has no X11 backend; it
  must nest inside Weston's Wayland).

## 2026-08-28 — EXP-007: Arch/Hyprland attempt + shutdown incident — **BLOCKED / RECOVERED**

- ArchLinuxARM aarch64 rootfs: **systemd 261 refuses kernel 4.19** (needs ≥5.10;
  systemd README v260+ confirms). PID1 exits: "Failed to determine whether /proc
  is a mount point: Protocol driver not attached".
- Workaround built: `--init` override → `custom_init=/usr/local/bin/mini-init`
  in container.config (note: config key is `custom_init`, not `init`). Container
  boots, but eth0 gets no IP (mini-init must run dhcpcd itself) — and my first
  mini-init `wait $!` loop spun the loadavg to 4300.
- **INCIDENT**: during no-shutdown-guard testing the phone powered off with zero
  signs of life. Cause never logged; a PMIC battery-latch was cleared by the
  owner (battery disconnect/reconnect). Recovery: samloader flash of the
  byte-identical custom BOOT (no `--no-reboot`) auto-reboots out of Download
  Mode — the button-less escape hatch, now in memory.
- Post-recovery: aggressive auto-reboot watchdog REMOVED; only the reboot/svc
  wrapper guards remain (boot-completed.d/95-no-shutdown.sh).
- Hyprland plan (next): lfdevs mesa-for-android-container has an official
  **archlinux_arm64** build; end-4/dots-hyprland depends on quickshell+qt6
  stack via AUR meta-packages.

## 2026-08-28 — EXP-008: weston-dmabuf patch chain + WayLandIE zero-copy GPU present — **SUCCESS (vkcube on screen)**

### Part A: the weston patch chain (Hyprland-on-X11 path, superseded)

- **Module shadowing fixed**: patched `gl-renderer.so` in `/usr/local/lib/libweston-15/`
  was never loaded — the distro module at `/usr/lib/libweston-15/` won. Fixed by
  replacing the distro file (backup `.distro-bak`) AND `LD_LIBRARY_PATH=/usr/local/lib`
  for `libweston-15.so`.
- **AQ-FALLBACK executed**: `Using rendering device (fallback): /dev/dri/renderD128` —
  weston now advertises dmabuf feedback; aquamarine builds a GBM allocator
  (`Created a GBM allocator with drm fd 17`). Hyprland reached "Output WAYLAND-1:
  initialized".
- **Three version bumps in weston source** were needed for aquamarine's newer protocol
  bindings: `wl_compositor` 5→6 (compositor.c:9971), `wl_seat` cap 7→interface version
  (input.c:4343), `xdg_wm_base` 5→6 (xdg-shell.c:49). All bind-handler safe (features
  gated by `wl_resource_get_version`).
- **Monitor fix**: `monitor=WAYLAND-1,1280x720@60,0x0,1` in hyprland.conf stopped the
  "NO PREFERRED MODE" retry loop (the X11 backend reports no modes).
- **THE WALL — zink present on EGL/X11 is broken**: every "working" screen was actually
  the X window *background pixel* (0x2288ff / 0x004400 / 0x4400 — verified by exact
  color match). Server-side `XGetImage` proved: GLX+zink presents (0xe633cc magenta ✓),
  EGL+zink presents nothing (window bg only). Root cause chain:
  Termux:X11's DRI3 `open` returns BadMatch (no DRM device to hand out) →
  kopper's swapchain silently drops presents. `LIBGL_KOPPER_DRI2`/`_DISABLE` don't help.
  GBM+zink separately fails ("failed to choose pdev") because Turnip reports
  `hasRender=0` in `VkPhysicalDeviceDrmPropertiesEXT` (probe: renderD128 is 226:128).
  EGL *device* platform + zink works — but Hyprland creates its display from the
  GBM fd, which poisons the pdev match.

### Part B: WayLandIE — dmabuf→SurfaceControl bridge (the working GPU path)

- Replaced Termux:X11 with **WayLandIE** (github.com/AstroCODEsky/WayLandIE):
  Linux Wayland server → dmabuf fd over abstract socket → Android
  SurfaceControl/Vulkan presenter. Zero CPU copies ("final-copy=forbidden").
- **macOS build ported** (build-apk.macos.sh): SDK 36 + build-tools 36.1.0 +
  NDK r29 on the SSD; aapt2/d8/javac/zipalign/apksigner replace the PowerShell flow.
- **Droidspaces netns fix**: the bridge socket `waylandie.display.bridge.v1` is an
  abstract socket — invisible from the NAT'd container. Switched arch container to
  `net_mode=host` in container.config (abstract sockets are netns-scoped).
- **Patch 1 — Android 14 SurfaceControl**: the presenter hard-required
  `ASurfaceTransaction_setBufferWithRelease` (API 35+; absent in our libandroid).
  Patched to fall back to `ASurfaceTransaction_setBuffer` + `setOnComplete`
  (API 29+), using present-completion as the release point.
- **Patch 2 — libadrenotools bundling**: built bylaws/libadrenotools with NDK r29
  (cmake + android.toolchain, BUILD_SHARED_LIBS=ON) and bundled `libadrenotools.so`
  + 4 hook libs into the APK lib dir.
- **Driver**: Turnip v26.3.0-R4 (StevenMXZ/Adreno-Tools-Drivers) installed as
  `vulkan.waylandie.a8xx.so` in `/data/user/0/io.waylandie.display/files/adrenotools-driver/`
  (chown to the app's uid — the status port reports the canonical path).
- **RESULT**: `vkcube --wsi wayland` → **62.3 fps, zero-copy dmabuf, frame 779+,
  LunarG cube visible on the phone screen**, 1280x720 → 2408x1080 @ 120 Hz.
  Bridge verdict: pass, 474 frames / 9 s, avg present 16.25 ms, failures 0.
- Artifacts: `waylandie/build-apk.macos.sh`,
  `waylandie/waylandie_display_native.android14.patched.c`.

### Next

- Hyprland as a client of the WayLandIE bridge (wl_compositor is capped at v5 in
  the bridge — same bump-to-6 patch as weston if aquamarine needs it).
- end-4/dots-hyprland rice on top.

## 2026-08-28 — EXP-009: Hyprland compositor live on the phone screen via WayLandIE — **SUCCESS**

- **The flush deadlock**: Hyprland (aquamarine) queues its xdg-shell requests but
  never flushes them — it polls the bridge socket for the `configure` event that
  can only arrive after its own requests flush. `WAYLAND_DEBUG=1` proved it:
  `get_xdg_surface`/`get_toplevel` logged as queued, no configure ever returned,
  while a minimal test client on the SAME server got configure instantly
  (all bind versions v1/v5/v6 — ruled out protocol versioning).
- **Fix — bridge ping patch**: the bridge server now sends `xdg_wm_base.ping`
  every 500 ms to all bound xdg_wm_base resources. The ping forces the nested
  compositor to answer (pong), which flushes its queued requests. After the
  patch: `New aquamarine output with name WAYLAND-1` → `New monitor: WAYLAND-1`
  → swapchain reconfigured 1280x720 XR24.
- Also bumped the bridge's `wl_compositor` global 5→6 (aquamarine's registry
  validation requires ≥6 to match its bindings).
- **RESULT**: **Hyprland desktop (teal logo wallpaper) rendering on the phone
  screen** in landscape 2408x1080, through:
  Hyprland (Arch container) → WayLandIE bridge → dmabuf → Android
  SurfaceControl/Vulkan (Turnip via AdrenoTools) → display.
  Chain verified by pixel probe: pure teal (0,212,210) logo live on screen.
- Hyprland idles at ~0% CPU (GPU-composited, unlike the llvmpipe runs).
- Artifacts: `waylandie/waylandie-wayland-bridge.patched.sh` (wl_compositor v6 +
  periodic ping patch).
- Known cosmetic: hyprland.conf `debug:verbose` option removed in 0.56 (warning
  banner on screen until config reload).

## 2026-08-28 — EXP-010: end-4 rice attempt, two device freezes, and the llvmpipe misclassification — **ROOT-CAUSED (no relaunch yet)**

### The incidents
Two full Android UI freezes (UI unresponsive, adb shell hung, device off USB)
during end-4/dots-hyprland bring-up, both recovered by the owner's battery
disconnect + samloader `flash -p BOOT` (byte-identical custom BOOT, auto-reboot
out of Download Mode — the proven escape hatch, ~50 s to ADB).

### Forensics (read-only, from /data/system/dropbox + container logs)
- `system_server_pre_watchdog@01:38` (freeze 1): **Hyprland at 94% CPU (40%
  user + 54% kernel)**, loadavg 59, `/proc/pressure/memory some=80%`,
  `/proc/pressure/io some=91%`, kswapd active, system_server at 83% with 49740
  minor faults — a classic memory-thrash livelock, not a kernel panic (KMSG
  clean, bootloader-only).
- Freeze 2's Hyprland crashed at `CBackend::create() failed!` (04:45, empty GPU
  field in crash report) — but the same launch script's earlier run is what
  spun.
- **The misclassification (self-caught, corrects EXP-009)**: the freeze-1-era
  `hyprland.log` contains `MESA: error: ZINK: failed to choose pdev` and an
  empty `GPU information:` field. The "GPU-composited, idles at 0%" claim in
  EXP-009 was wrong — **that Hyprland was llvmpipe software rendering all
  along**. vkcube (pure Vulkan) was the only true GPU render in the chain.

### Root cause
1. `zink_get_display_device()` matches the chosen Vulkan pdev against the
   Wayland `linux-dmabuf` `main_device` (bridge correctly advertises
   `/dev/kgsl-3d0`, char 511:0). Turnip-on-KGSL reports no DRM render node
   (`hasRender=0`), so the match fails → `ZINK: failed to choose pdev` →
   Mesa silently falls back to llvmpipe.
2. llvmpipe renders the desktop at 1080×2408 on CPU; with end-4's Lua config
   (3-pass blur + xray + shadows + dim), CPU pegs at 94% and buffer memory
   balloons until Android's system_server thrashes → watchdog → UI dead.
   The plain-config run merely *survived* on llvmpipe; the rice exposed it.
3. Contributing: the ii launch scripts (`ii-launch.sh`/`ii-safe.sh`) exported
   no Mesa env at all (no `GALLIUM_DRIVER`, no override), guaranteeing the
   fallback path.

### The fix (researched, verified against container contents, NOT yet launched)
- lfdevs/mesa-for-android-container README: for Adreno 6xx use
  `MESA_LOADER_DRIVER_OVERRIDE=kgsl` — freedreno GL **directly over KGSL**,
  no zink in the path (`kgsl_dri.so` is present in the container). Zink is
  only needed for a8xx parts.
- Alternative (zink path): zorrobyte/razr-fold-2026-lindroid
  `zink-kgsl-pdev-fallback.patch` — one-line zink pdev fallback; documented
  as the fix for this exact `failed to choose pdev` on turnip/KGSL.
- Verified in container: `/dev/kgsl-3d0` and `/dev/ion` present (turnip
  supports both ION and dma_heap; this 4.19 host exposes ION only — fine),
  turnip ICD installed, bridge ping patch intact.

### Safe relaunch plan (requires owner approval before any launch)
1. Stage env: `MESA_LOADER_DRIVER_OVERRIDE=kgsl` (+ lfdevs vblank hint
   `vblank_mode=3 MESA_VK_WSI_PRESENT_MODE=mailbox` if tearing).
2. **Hard caps before launch**: cgroup cpu.max + memory.max on the container
   so a software-rendering fallback can physically not wedge Android again.
3. **llvmpipe watchdog**: pre-launch GL probe must report a non-llvmpipe
   `GL_RENDERER` (expected: `freedreno` over kgsl), else abort launch.
4. Re-verify with plain config first, then end-4 execs, then full rice —
   one variable per launch, caps always on.

- execs.lua already restored (a later ii-files.sh re-copy put the original
  25-line file back; verified).

## 2026-08-28 — EXP-011: lfdevs Mesa + guarded GPU launch — freeze 3, silent-death signature — **ROOT-CAUSE NARROWED, no relaunch**

### What was built (all verified before launch)
- Installed lfdevs/mesa-for-android-container 26.3.0 (pacman -U: mesa, vulkan-freedreno,
  vulkan-mesa-implicit-layers, vulkan-mesa-layers; stock 26.2.1 cached for rollback —
  confirmed survived reboot in pacman DB).
- GL probe gate: `eglinfo -B -p wayland` with `MESA_LOADER_DRIVER_OVERRIDE=kgsl`
  → **`GL_RENDERER = FD619`** (real Adreno 619 via freedreno-over-KGSL, llvmpipe GONE).
  The probe gate correctly ABORTED the first launch attempt when stock Mesa still
  gave llvmpipe — the gate works.
- Guards: host-side watchdog (kill Hyprland on >85% sustained CPU), v1 cgroup
  cpu.shares=512 + memory.limit 2.5GB for enrollment, plain hyprland.conf (no end-4).
- Bridge lifecycle bug found+fixed during staging: the WayLandIE bridge **exits when
  its last client disconnects** (probe's eglinfo connect/disconnect killed it, and
  launch-time pgrep false-positived on dying bridges). Launch script now checks for
  a live socket dir and starts a fresh bridge, holding a client.

### Freeze 3 — signature completely different from freeze 1
- Onset: within ~15–60 s of "hyprland alive after 5s"; my next adb diagnostic
  command hung. User confirmed same UI-stuck state. Recovered via the samloader
  byte-identical BOOT flash escape (3rd successful use).
- **Silent death**: NO system_app_anr, NO system_server_pre_watchdog, NO tombstones,
  clean `Last boot reason: reboot` (freezes 1: Hyprland 94% CPU + pre-watchdog +
  ANR storm + memory/io pressure 80–91%). Freeze 2 rechecked: also silent.
  Only freeze 1 was "loud".
- Kernel evidence: KMSG bootloader-only (Samsung LAST_KMSG never carries kernel
  console on this device); pstore empty; /data/log has no new ramdump.
- f2fs/power-pull ate all writes from the final ~minutes (watchdog script+log on
  /data/local/tmp, launch scripts, hyprland-go.log) — so the watchdog likely never
  fired (or fired without observable effect) and Hyprland's dying words are lost.
- KGSL today (plain Android, clean boot): `reset_count` ticks ~6/min with
  `ft_hang_intr_status=0` and zero kgsl dmesg lines → benign power-transition
  counting; GPU healthy. fd holders: surfaceflinger/systemui/launcher (normal)
  + com.droidspaces.app.

### Working hypothesis (ranked)
1. **Kernel-level KGSL contention**: freeze 3 was the first *sustained* run with
   freedreno (container, direct /dev/kgsl-3d0 GL) + Turnip-via-AdrenoTools (bridge
   APK present path) + SurfaceFlinger all submitting to the same KGSL. A driver
   lockup there = silent total death, immune to userspace watchdogs. The successful
   1-hour Hyprland run was llvmpipe (CPU) — only the bridge APK drove the GPU.
2. Freeze 2 was probably still the llvmpipe+end-4 blur spin (freeze-1 mechanism),
   just dying too fast for watchdog records to be written.
3. Less likely: memcg enrollment interacting with ION allocations.

### Next steps (design only — nothing launched)
- **Kernel flight recorder first**: host-side daemon persisting dmesg tail +
  kgsl state to /data/local/tmp with sync, running BEFORE any GPU experiment.
- **Isolation control**: freedreno-over-bridge WITHOUT Hyprland (glmark2-es2-wayland
  or the existing gles2-test binary), timeboxed, with recorder — cleanly separates
  "freedreno via bridge" from "Hyprland" from "Hyprland+freedreno".
- Only then: Hyprland plain-config relaunch, user present.
