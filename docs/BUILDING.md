# Building tpw-rtx

## What you need

- **Visual Studio 2022** (Community is fine) with the **Desktop development with C++** workload. That includes the Windows SDK
  and `dxc`.
- **NVIDIA DLSS SDK** v310.9.16 (headers + `nvsdk_ngx_s.lib`). It is not in this repository, because NVIDIA's licence doesn't allow
  redistributing it. Clone it next to the sources:

  ```
  git clone --depth 1 --branch v310.9.16 https://github.com/NVIDIA/DLSS third_party\DLSS
  ```

  Or set `DLSS_SDK` to an existing copy of the SDK.

## Build

```
build.bat
```

It produces, in `build\`:

| File | What |
|---|---|
| `dinput.dll` | the hook (32-bit), `src/hook` |
| `tpwrtx_viewer.exe` | the renderer (64-bit), `src/viewer` |
| `rt.cso` | the path tracer, compiled from `src/viewer/rt.hlsl` (`cs_6_5`) |
| `tpwrtx_nvngx.dll_forwarder.dll` | the optional DLSS 5 forwarder, `src/dlss5` |
| `tpwrtx.ini`, `dxwrapper.ini`, `install.bat` | copied from `config/` and the repo root |

Then run `build\install.bat "<game folder>"`, or copy the files next to `TP.EXE` yourself.

## Layout

```
src/hook/      dinput.dll proxy: capture, fixes, overlay (tphook.c, overlay.cpp)
src/viewer/    renderer: D3D12, DXR path tracer, denoiser, DLSS-RR, settings panel
src/dlss5/     the unofficial DLSS 5 forwarder (see docs/DLSS5.md)
src/shared/    the shared-memory layout used by both halves
config/        tpwrtx.ini, dxwrapper.ini
```

## Debugging aids

- `tphook.log`: a once-per-second watchdog line (fps, focus), plus DirectInput and overlay events.
- `tpwrtx_viewer.log`: adapter, ray tracing support, DLSS status, errors.
- Create a file named `tpwrtx_shot` in the game folder to have the renderer save `tpwrtx_view.png`.
- `tpwrtx_cmd` takes one command:
  - `orbit <yaw> <pitch> [zoom]` or `game` for the camera;
  - `rt 0|1` for ray tracing;
  - `sun <azimuth> <elevation>` and `exposure <x>` for lighting;
  - `dumptex <id>` to save a texture.
- `tpwrtx.ini` `minimal = 1` keeps only the build-independent fixes, for bisecting problems.
