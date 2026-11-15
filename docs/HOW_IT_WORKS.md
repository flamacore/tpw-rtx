# How tpw-rtx works

## The pieces

```
TP.EXE (32-bit, 1999 engine, DirectDraw + Direct3D 6)
 ├─ ddraw.dll / dxwrapper.dll     DirectDraw → Direct3D 9, windowed (third party)
 └─ dinput.dll  ← tpw-rtx hook (src/hook)
      • captures every triangle + texture the game draws, plus the game's camera
      • fixes: timers, video memory, alt-tab mouse, 4:3 window
      • shared memory "Local\TPWRTX_SHARED_V1" (src/shared/tpwrtx_shared.h)
      • shows the renderer's image on the game window (DirectComposition)
                     │ frames, textures              ▲ finished image (shared D3D12 texture + fence)
                     ▼                               │
tpwrtx_viewer.exe (64-bit, Direct3D 12) ← tpw-rtx renderer (src/viewer)
      • rebuilds the scene in world space, keeps off-screen geometry
      • path tracing with DXR inline ray queries (rt.hlsl)
      • DLSS Ray Reconstruction (NGX), or its own a-trous denoiser
      • optional DLSS 5 pass (src/dlss5, unofficial)
      • settings panel (Win32), tpwrtx_settings.json
```

The game is never modified on disk. Everything happens in memory, through a proxy `dinput.dll` that Windows loads from the game
folder. TP.EXE imports only `DirectInputCreateA` from it.

## Getting a 3D scene out of a 1999 game

The engine transforms everything on the CPU and hands Direct3D screen-space vertices: `XYZRHW | DIFFUSE | SPECULAR | TEX1`,
indexed triangle lists, no vertex buffers and no transforms. That throws away the 3D information, so it has to be recovered:

- `rhw` is 1 / view depth, so `z_view = k / rhw`.
- The projection was solved from captured frames: at 2048×1536, `x_view = (sx − 1023.8) · z / 1535.3` (horizontal FOV 67.4°). The
  median re-projection error over 13,700 matched vertices was 0.009 px.
- The camera lives in game memory: view matrix at `0x790900`, camera-to-world at `0x790990`. Values read at `BeginScene` match that
  frame exactly.

With those, every vertex goes back to world space. The engine culls what is off-screen, but shadows and bounce light need it, so the
renderer keeps triangles from earlier frames that are now entirely off-screen. Anything that is partly visible again but wasn't
redrawn has moved, and is dropped.

Draw states decide what goes into the ray tracer:

- Opaque geometry is traced.
- Foliage, fences and visitor sprites are alpha-tested *and* blended, and are traced as cut-outs.
- Truly see-through things (water, smoke, glass, balloons) are rasterised on top, as are additive flames. Soft texture alpha or faded
  vertex alpha gives them away.
- Flame and glow sprites become point lights.

## Lighting

`rt.hlsl` is a compute shader using DXR 1.1 ray queries. Per pixel it traces:

- a primary ray;
- a sun shadow ray, with a jittered cone for soft shadows;
- one torch light, picked at random;
- one cosine-weighted bounce, lit by the sun and torches at the bounce point plus sky.

Surface albedo comes from the game textures. The vertex colour's hue and part of its baked shading are kept, adjustable in the
panel. For pseudo-PBR, normals are bumped from the texture luminance along the triangle's dP/du and dP/dv, roughness is guessed
from texture brightness, and the sun gets a GGX highlight.

Two denoising paths:

- **DLSS Ray Reconstruction:** the ray tracer writes the raw per-frame sample plus guides: diffuse and specular albedo,
  normal + roughness, depth, motion vectors, and a 64-phase Halton jitter. NGX returns a clean, anti-aliased HDR image, optionally
  upscaled from Quality, Balanced or Performance resolution.
- **Own denoiser:** demodulated irradiance (lighting divided by albedo), temporal accumulation reprojected with exact world positions,
  then an edge-aware à-trous wavelet filter. Textures stay sharp because only the lighting is blurred.

The composite applies ACES tone mapping, then contrast and saturation. The optional DLSS 5 pass runs here, before the raster passes
(water, smoke, UI) are drawn on top.

## Presenting it

The renderer can't own a window over the game: a second topmost window fights the game for focus and z-order, and the game pauses
or misbehaves when it loses focus. Instead it renders into a **shared D3D12 texture** and signals a **shared fence**. Both handles
are duplicated into the game process, and the hook opens them with Direct3D 11 and shows them through a **DirectComposition**
visual on the game's own window. If the renderer stops delivering, the overlay hides itself after 120 frames and the original game
shows through.

The game's own mouse pointer is invisible under the overlay, so the hook reads its position (`0xfaa540`/`0xfaa544`, in 2048×1536
units) and adds a gold arrow to the stream.

## Fixes for old bugs (useful even without ray tracing)

| Problem | Cause | Fix |
|---|---|---|
| The park slows to a crawl after loading | `timeGetTime()` is about 10⁹ ms after 11+ days of Windows uptime. The game keeps time in 32-bit floats, which only resolve 128 ms steps at that size. | `timeGetTime`, `GetTickCount` and `QueryPerformanceCounter` are re-based for TP.EXE so they start at 10 s. |
| Software rendering under dxwrapper | `GetAvailableVidMem` with empty caps fails, so the game decides it's out of VRAM. | Report 256 MB. |
| Mouse frozen after alt-tab | The mouse is read through DirectInput **event notification**: the game sleeps until DirectInput signals input. After Windows takes the device away, no event ever fires again. | Remember the devices the game acquired, and re-acquire them whenever the game has focus. |
| Stretched 16:9 window | The game sizes its window to the full screen. | Keep the client area at 4:3; DPI-aware. |

## Settings panel without pausing the game

The panel is an ordinary Win32 window that **never activates** (`WS_EX_NOACTIVATE`), so the game keeps focus and keeps running at
full speed. While it is open, the renderer sets a heartbeat in shared memory. The hook then releases the game's DirectInput devices
so the system cursor can reach the panel, and takes them back when the panel closes. If the renderer dies, the heartbeat stops and
the hook takes them back after 2 seconds.

## Camera and widescreen

The hook patches a few instructions in TP.EXE's memory at start-up (the file on disk is never changed), then follows the settings
panel every frame:

- **Zoom:** the camera distance is clamped to 70..130 by two compares and two immediates in the camera update. They now read the
  panel's limits. The game derives the pitch from the zoom (45° at 70, 65° at 130), so beyond the old range the camera keeps
  tilting, capped at about 84°.
- **FOV:** the camera init passes a fixed 90 to the projection (`FUN_00578be0`) and frustum (`FUN_0056b790`) builders. The init now
  reads the panel's value, and the hook calls both builders again when it changes. Mouse picking keeps its own copy of the
  frustum (a half-diagonal angle and the 4:3 aspect), which is recomputed to match.
- **Draw area:** the engine only draws objects and terrain inside a square around the camera, 460 units across, which was just
  enough for zoom 130 at 4:3. That size now grows with the zoom, the FOV and the screen width.
- **WASD / Q/E:** the camera code asks `FUN_0040ca20(group, action)` whether a bound key is down. Those calls go through a wrapper
  that also checks W/A/S/D/Q/E, but only while the game's keyboard group is enabled (it is disabled while you type a name).
- **Widescreen:** the resolution switch (mode 7 = 2048×1536) is patched to the configured size, the aspect (`0x8bcbcc`, 0.75)
  follows it, and the window is kept at that shape instead of 4:3. The renderer reads the field of view and aspect from the game
  every frame, so the reconstruction follows both.

## Game addresses (one TP.EXE build)

| Address | Meaning |
|---|---|
| `0x790900` | view matrix (world → view, row vectors) |
| `0x790990` | camera → world (row 3 = eye position) |
| `0x77c488` | game pointer system active |
| `0xfaa540`, `0xfaa544` | pointer x, y (shorts, 2048×1536 space) |
| `0x7a1a14` | state flags (bit 3 = app active) |
| `0x8bd518` | main window handle |
| `0x7909ec` | camera distance (zoom); clamp at `0x42c66b`..`0x42c6a1` |
| `0x87af58` | current FOV (degrees; 90 in a park, 100 in the menus) |
| `0x8bcbcc` | aspect (height / width) |
| `0x56eaae` | draw-area size (460) |
| `0x449ec0` | resolution switch |
| `0x787c14`, `0x78721c` | camera and shortcut key groups |

These were found with Ghidra in TP.EXE SHA-256 `d0399acf…27ac9`. The hook checks the build (PE timestamp and image size) and uses
none of them on any other build.
