# xr-workspace — Specification

> A fast, modular, SpaceWalker-equivalent virtual-monitor environment for
> Wayland/Hyprland, driven by head-tracked XR glasses (VITURE Pro and similar).

---

## 1. Vision

`xr-workspace` turns a pair of XR glasses into a floating, head-tracked
multi-monitor workspace. Real desktop outputs are captured and drawn as virtual
screens that are **pinned in world space** — you turn your head to look between
them, exactly like VITURE's *SpaceWalker* or Breezy Desktop, but native to
Wayland, dependency-light, and fast enough to run on ~9-year-old hardware.

It is designed as a **companion app**: it does not replace your compositor or
window manager. Windows continue to live on real (or headless) Wayland outputs
managed by Hyprland; `xr-workspace` mirrors those outputs into a 3D scene and
lets you control the layout from the outside (e.g. the smplOS settings app) via
a config file and a runtime control API.

---

## 2. Goals & Non-goals

### Goals
- **Floating, world-pinned virtual monitors.** One giant curved screen *or* a
  multi-monitor arc, anchored in space; head rotation looks around them.
- **Real desktop content.** Each virtual monitor shows a live capture of a real
  Wayland output (not a placeholder).
- **Move windows between monitors.** Achieved through the compositor: each
  virtual monitor maps to a real/headless output, so native Hyprland window
  movement works unchanged.
- **Modular, swappable design.** Every subsystem (pose source, capture backend,
  renderer, layout, control transport) sits behind a small interface and can be
  replaced without touching the rest.
- **Very fast on old hardware.** Target an i7 (~2016) laptop with USB-C
  DisplayPort-alt-mode out. Zero-copy capture where possible, GLES2, no
  per-frame allocations, frame-callback-paced rendering, minimal motion-to-photon
  latency.
- **External control.** Add/remove/configure monitors and set their resolution
  from other apps through a JSON config file **and** a runtime IPC API.

### Non-goals (for now)
- Becoming a standalone Wayland compositor.
- 6DoF / positional tracking (glasses are 3DoF IMU-only) — architecture leaves
  room, but the default is orientation-only.
- Windows/macOS support. Linux + Wayland only.
- Per-eye stereoscopic 3D rendering (the glasses present a single virtual
  display); SBS/lens handling is an optional later module.

---

## 3. Target hardware & performance budget

| Aspect | Target |
| --- | --- |
| CPU | Intel i7 ~2016 (Skylake/Kaby Lake), 4 cores |
| GPU | Integrated (Intel HD/Iris) or modest dGPU, GLES2-class |
| Glasses out | USB-C DP-alt-mode, 1920×1080 @ 60–72 Hz (per eye = same image) |
| Pose source | XRLinuxDriver writing `/dev/shm/breezy_desktop_imu` |
| Frame budget | ≤ 4 ms CPU + GPU per frame at 60 Hz on the iGPU |
| Memory | < 100 MB resident with 3 captured 1080p monitors |
| Startup | < 250 ms to first frame |

Performance rules:
- No heap allocation in the render loop. All geometry/matrices precomputed.
- Read pose as late as possible before the vertex transform (already done).
- Prefer **dmabuf** zero-copy import of captured frames; fall back to shm copy.
- Single GLES2 program, one VBO, one draw call per quad. Avoid state churn.
- Frame pacing via `wl_surface.frame` callbacks (no busy-wait, no double vsync).

---

## 4. Architecture

```
                       ┌─────────────────────────────────────────────┐
                       │                   App (main)                 │
                       │  arg parse · event loop · wires modules      │
                       └───┬───────────┬───────────┬─────────┬────────┘
                           │           │           │         │
              ┌────────────▼──┐ ┌──────▼──────┐ ┌──▼──────┐ ┌▼───────────┐
              │  PoseSource   │ │   Scene     │ │Renderer │ │ Control    │
              │ (IPoseSource) │ │  (layout +  │ │(GLES2)  │ │  (IPC)     │
              │               │ │  monitors)  │ │         │ │            │
              │ breezy_shm    │ │             │ │         │ │ unix sock  │
              │ (swappable)   │ │             │ │         │ │ JSON cmds  │
              └───────────────┘ └──────┬──────┘ └─────────┘ └─────┬──────┘
                                       │                          │
                              ┌────────▼─────────┐        ┌───────▼───────┐
                              │ CaptureSource[]  │        │   Config      │
                              │ (ICaptureSource) │        │  config.json  │
                              │  solid | screen- │        │  load/save    │
                              │  copy | dmabuf   │        │  watch        │
                              └──────────────────┘        └───────────────┘

   Wayland backend (display · registry · wlr-layer-shell · EGL) underpins all.
```

### Module responsibilities

| Module | Interface | Responsibility | Swappable impls |
| --- | --- | --- | --- |
| **Math** | `linalg.h` (header-only) | Quaternion/matrix math | — |
| **PoseSource** | `IPoseSource` | Provide head orientation (+ optional position) with minimal latency | `BreezyImuSource` (SHM); future: udev/hidraw, network, replay |
| **CaptureSource** | `ICaptureSource` | Provide a GL texture for one virtual monitor, refreshed per frame | `SolidColorCapture` (placeholder/test); `ScreencopyCapture` (wlr-screencopy shm); `DmabufCapture` (zero-copy) |
| **Scene** | `Scene` / `VirtualMonitor` | Hold monitor list + layout; recompute model matrices | layouts: arc, flat (one big), custom (per-monitor transform / pinning) |
| **Renderer** | `Renderer` | GLES2 draw of the scene given a view matrix | future: Vulkan backend |
| **Config** | `Config` | Parse/serialise `config.json`; defaults; hot-reload | — |
| **Control** | `ControlServer` | Runtime API over a Unix socket (JSON) | future: D-Bus transport |
| **Wayland backend** | (in app) | display/registry/outputs/layer-shell/EGL | — |

Stability rule: interfaces (`IPoseSource`, `ICaptureSource`, the control JSON
schema, and `config.json` schema) are the contract. Implementations behind them
may be freely replaced.

---

## 5. Data model

### VirtualMonitor
A single floating screen.

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | string | Stable identifier (used by the control API) |
| `source` | string | Wayland output name to capture (e.g. `HEADLESS-1`). Empty → solid colour |
| `width`,`height` | int | Capture/source resolution in pixels |
| `scale` | float | Source scale factor |
| `angle_deg` | float | Azimuth position on the arc (− left, + right) |
| `size_m` | float | Physical width in world units (height derived from aspect) |
| `curvature` | float | 0 = flat; >0 = curved screen (later) |
| `pos[3]`, `yaw_deg`, `pitch_deg` | float | Explicit transform for `custom` layout / pinning |
| `color` | rgb | Fallback colour when `source` is empty |

### Layout modes
- **arc** — monitors spread on a cylinder arc at `radius`, auto-angled.
- **flat** — a single large screen (or a flat wall of monitors) facing forward.
- **custom** — each monitor uses its explicit `pos`/`yaw`/`pitch` (full pinning).

---

## 6. Config file

**Location (in priority order):**
1. `--config <path>` CLI argument
2. `$XDG_CONFIG_HOME/xr-workspace/config.json`
3. `~/.config/xr-workspace/config.json`

Hot-reload: the file is watched; on change the scene is rebuilt without
restart. The control API can also trigger `reload`/`save`.

**Schema (`config.json`):**
```jsonc
{
  "output": "DP-3",            // physical glasses output to render onto ("" = auto/last)
  "fov_deg": 46.0,             // glasses horizontal FOV
  "pose_source": "breezy_shm", // which IPoseSource to use
  "smoothing": 0.0,            // 0 = raw, >0 = slerp factor (0..1) for jitter reduction
  "capture_protocol": "auto",  // auto | ext (ext-image-copy-capture) | wlr (wlr-screencopy)
  "prefer_dmabuf": true,       // zero-copy dmabuf/EGLImage when available, else shm
  "stereo": false,             // side-by-side stereo output (SBS 3D on the glasses)
  "ipd_m": 0.063,              // interpupillary distance for SBS eye offset (world units)
  "layout": { "mode": "arc", "radius": 2.0 },
  "control_socket": "",        // "" = $XDG_RUNTIME_DIR/xr-workspace.sock
  "monitors": [
    { "id": "left",   "source": "HEADLESS-1", "width": 1920, "height": 1080, "angle_deg": -45, "size_m": 1.0 },
    { "id": "center", "source": "HEADLESS-2", "width": 2560, "height": 1440, "angle_deg":   0, "size_m": 1.2 },
    { "id": "right",  "source": "HEADLESS-3", "width": 1920, "height": 1080, "angle_deg":  45, "size_m": 1.0 }
  ]
}
```

CLI flags remain as quick overrides (`--monitors`, `--output`, `--radius`,
`--fov`) and take precedence over the config file for those fields.

---

## 7. Control API (runtime)

**Transport:** Unix domain socket (default
`$XDG_RUNTIME_DIR/xr-workspace.sock`), newline-delimited JSON requests and
responses. Integrated into the main event loop via `poll()` alongside the
Wayland fd — no extra threads, no locking.

**Requests** (one JSON object per line):

| `cmd` | Params | Effect |
| --- | --- | --- |
| `list` | — | Return all monitors + layout |
| `get_config` | — | Return the full effective config |
| `add` | `monitor: {…}` | Add a virtual monitor |
| `remove` | `id` | Remove a monitor |
| `set` | `id`, `props: {…}` | Update fields (resolution, angle, size, source, curvature) |
| `layout` | `mode`, `radius?` | Change layout mode/params |
| `recenter` | — | Snap origin to current head direction |
| `head_tracking` | `enabled?`, `set: {…}` | Toggle / tune head-look output (mode B) |
| `profile` | `name` | Activate a named per-game head-tracking profile |
| `render` | `stereo?`, `ipd_m?`, `smoothing?`, `fov_deg?`, `capture_protocol?`, `prefer_dmabuf?` | Live-tune render + capture backend (rebuilds capture if backend changes) |
| `reload` | — | Reload `config.json` from disk and rebuild the scene |
| `save` | `path?` | Persist current state to config |
| `hypr_create_output` | — | Create a Hyprland headless output to host a virtual screen |
| `hypr_remove_output` | `name` | Remove a Hyprland headless output |
| `hypr_move_window` | `output`, `window?` | Move a window (or current workspace) onto an output |

**Response:** `{"ok": true, ...}` or `{"ok": false, "error": "<msg>"}`.

This is what the smplOS settings app talks to in order to add/remove monitors
and change their resolution live.

---

## 8. Window management & compositor integration

`xr-workspace` does not move windows itself. The model:

1. For each virtual monitor, a **real Wayland output** exists — either a
   physical output or a **headless output** created on demand via Hyprland IPC
   / `wlr-output-management`.
2. `xr-workspace` captures each such output and floats it in the 3D scene.
3. Moving a window "from one monitor to another" is a normal compositor
   operation (`hyprctl dispatch movewindow`, drag to output, etc.). Because the
   virtual monitors mirror real outputs 1:1, this Just Works.

The control API may expose helper passthroughs later (e.g. create the headless
outputs to match the configured monitors), but compositor window management
stays the source of truth.

---

## 9. Pose pipeline

- `IPoseSource::read()` returns the latest orientation quaternion (NWU→EUS
  converted) with `valid` flag; identity when the driver is disabled.
- Read happens inside the frame callback, immediately before building the view
  matrix, to minimise motion-to-photon latency.
- Optional `smoothing` applies slerp between the previous and current
  orientation to suppress IMU jitter.
- **Recenter** captures `conj(current orientation)` as the world origin so the
  monitors re-align to where you are looking (Ctrl+Shift+C / `SIGUSR1` / control
  API). IMU yaw drift is a hardware trait; recenter is the remedy.

---

## 10. Rendering pipeline

- World-anchored monitors; camera orientation = recentered head pose; view =
  inverse of the head rotation.
- Single GLES2 program, interleaved unit-quad VBO, per-monitor model matrix +
  border, two draw calls per monitor.
- Projection rebuilt only on output-size/FOV change.
- `eglSwapInterval(0)` — pacing comes from `wl_surface.frame`.
- Future modules: curved-screen tessellation, lens distortion correction, SBS.

---

## 11. Build & run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./run.sh                     # dependency check + launch (3 monitors)
./build/xr-workspace --help  # all flags
```

Runtime deps: `wayland-client`, `wayland-egl`, `egl`, `glesv2`, a
`wlr-layer-shell`-capable compositor (Hyprland), and XRLinuxDriver feeding
`/dev/shm/breezy_desktop_imu`.

---

## 12. Roadmap / milestones

| # | Milestone | Status |
| --- | --- | --- |
| M0 | Layer-shell overlay + EGL/GLES2 + IMU head tracking + arc of placeholder monitors + recenter | ✅ done |
| M1 | **Modular refactor**: math, swappable `IPoseSource`, swappable `ICaptureSource` | ✅ done |
| M2 | **Config file** (`config.json`) load + apply + CLI override | ✅ done |
| M3 | **Control API** (Unix-socket JSON): list/add/remove/set/layout/recenter | ✅ done |
| M4 | **Real capture**: `ext-image-copy-capture` / `wlr-screencopy` → live desktop on monitors (shm path) | ✅ done |
| M5 | **Zero-copy** dmabuf/EGLImage capture for old-hardware performance (auto, shm fallback) | ✅ done |
| M6 | Headless-output orchestration via Hyprland IPC (window movement story) | ✅ done |
| M7 | Pose smoothing, curved screens, optional SBS stereo | ✅ done |
| M8 | smplOS settings-app integration end-to-end (`reload`/`save`/`render` control API) | ✅ done |
| M9 | **Head-tracking output (mode B)**: emit head-look to sims via OpenTrack + per-game profiles | ✅ done |

---

## 12b. Head-tracking output (mode B — head-look to games)

Besides anchoring virtual monitors (mode A), xr-workspace can act as a **head
tracker** for flight/space sims: the same recentered IMU orientation is exported
as yaw/pitch/roll so you can glance around a cockpit (TrackIR-style).

### Pipeline

```
glasses IMU ──► xr-workspace ──UDP 6×double──► OpenTrack ──TrackIR/freetrack──► game
               (IPoseSink /                 (mapping +
                OpenTrackSink)               curves)
```

- `IPoseSink` (src/posesink.h) is the swappable output interface; `OpenTrackSink`
  (src/sink_opentrack.{h,cpp}) is the first implementation. A future `uinput`
  virtual-joystick sink can be dropped in without touching the render loop.
- We send OpenTrack's **"UDP over network"** packet: six little-endian doubles
  `{x, y, z, yaw, pitch, roll}` to `127.0.0.1:4242`. Rotation only; translation 0.
- The quaternion is converted to Tait-Bryan degrees; `*_scale`, `invert_*` and
  `deadzone_deg` allow coarse tuning (OpenTrack's own curves do the fine work).

### Elite Dangerous (verified)

- Elite Dangerous has **native TrackIR/freetrack head-look** — confirmed by
  OpenTrack's own wiki, which calls it out by name under the freetrack protocol.
- On Linux/Proton it runs **Gold** (ProtonDB) and head tracking works, with one
  caveat documented on the OpenTrack wiki: **install OpenTrack to `/opt/opentrack`
  or `~/.local`, not `/usr`**, or Steam's pressure-vessel runtime shadows it and
  Proton games won't see the tracker.
- In-game: bind the *Head look* axes (or enable TrackIR) and calibrate sensitivity
  in OpenTrack. xr-workspace only feeds raw orientation; OpenTrack handles the
  game-facing protocol.

### Per-game enable & configure

Head tracking is **off by default** and tuned per game via **named profiles** in
`config.json` (`profiles: { "elite-dangerous": { … }, "dcs": { … } }`), each
overriding the global `head_tracking` defaults. Activation:

- **Startup**: `active_profile` in config, or `--profile NAME` / `--head-tracking`.
- **Live**: control API — `{"cmd":"profile","name":"elite-dangerous"}`,
  `{"cmd":"head_tracking","enabled":true}`, `{"cmd":"head_tracking","set":{…}}`.
- **Per-launch helper**: `xr-game <profile> <command…>` selects the profile,
  enables tracking for the game's lifetime, and disables it on exit. Works as a
  Steam launch-option wrapper. `xrctl on|off|profile NAME|headtrack prop val`
  expose the same controls from the shell or the smplOS settings app.

---

## 13. Open questions

- Headless output creation: Hyprland IPC vs generic `wlr-output-management` —
  pick one as the default orchestrator.
- Capture cadence: capture every output every frame vs. on-damage only (prefer
  damage-driven to save the iGPU).
- Multiple physical glasses outputs / hotplug handling.
- Whether to vendor a tiny JSON parser (dependency-free) or accept a header-only
  lib; current plan: small in-tree parser to stay dependency-light.
