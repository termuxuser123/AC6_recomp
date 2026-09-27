# AC6Recomp

> [!CAUTION]
> This project is still work in progress. It can boot and run in-game, but bugs, crashes, and missing functionality should be expected.

A native PC port of **Ace Combat 6: Fires of Liberation** built on top of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The Xbox 360 PowerPC game code is statically recompiled to x86-64, while visible rendering currently remains authoritative in the vendored RexGlue/Xenia graphics backend.

> [!NOTE]
> **This `linux` branch builds and runs natively on Linux** (Vulkan). See [Building on Linux](#building-on-linux).
> The Linux port itself is BMagnu's work from [sal063/AC6_recomp#45](https://github.com/sal063/AC6_recomp/pull/45); this branch adds fixes on top: controller input freezing in gameplay, a `/dev/shm` leak that caused "Bus error" crashes, a CPU busy-wait while waiting for the GPU (helps laptop iGPUs), and a link error on some toolchains. It also carries two fixes for black screens when running the Windows build under Wine.

---

## Disclaimer

This project is an unofficial, non-commercial fan effort. It is **not affiliated with, endorsed by, or associated with** Bandai Namco Entertainment, Project Aces, or Microsoft. *Ace Combat 6: Fires of Liberation* and all related assets, characters, and trademarks are the property of their respective owners.

**This repository contains source code only. It does not contain, and will never contain, any game data.** To use this software you must supply your own legally obtained copy of the game. No game files, disc images, DLC packages, title updates, or console keys are distributed here, and none may be committed to this repository.

The software is provided as-is, without warranty of any kind. It is experimental: expect bugs, crashes, and visual or audio differences from the original console release. Do not redistribute builds bundled with game data.

---

## Features

Everything listed here is on by default unless noted, and each fix can be turned off individually.
See [Important console variables](#important-console-variables).

- **60 FPS.** The game can now run up to 60 FPS, with the flight model corrected.
- **High quality terrain.** Terrain is drawn at the full resolution shipped on the disc, twice what the console rendered. Cracks and seams that appear over mountains are also removed.
- **Resolution scaling.** Render internally at 2× (1440p) or 3× (2160p) and beyond for a much sharper image.
- **Ultrawide support.** Optional hor+ widescreen in missions and cutscenes.
- **AC7-like keyboard and mouse controls.** Mouse steering, remappable bindings, live-reloaded from a config file. Controllers work out of the box, including a virtual pad when none is connected.
- **Japanese language support.**
- **Texture replacement modding** — see [Modding docs](#modding-docs).

### Fixes

*Most of the known issues with the recomp have been fixed. Please report if you have found more issues.*

*Performance is a known issue with the recomp! Due to native renderer not yet implemented, a powerful GPU is recommended to be able to run the game at 60 FPS at high resolution!*

---

## Quick Start Guide

### Requirements

| | |
|---|---|
| **OS** | Windows 10 or 11, 64-bit, or Linux (x86-64, see [Building on Linux](#building-on-linux)) |
| **CPU** | **Must support AVX2** — Intel 4th-gen Core (2013) or newer, AMD Zen (2017) or newer. |
| **GPU** | Direct3D 12 capable |
| **Game data** | Your own legally obtained copy of Ace Combat 6 (US region only! Europe and Japan is not supported) |

*Note that the system requirement does not guarantees that the recomp will perform well on your system!*

### Steps

1. **Get a build.** Either download from release, or build from source (see [How to build](#how-to-build)).

2. **Provide the game data.** Put *one* of the following next to `ac6recomp.exe`:
   - the game's `.iso` disc image **or**
   - an `assets/` folder containing the extracted game files.

4. **(Optional) Add DLC.** Extracted DLC packages can be placed next to the executable, inside a folder named `dlc/`.

5. **Run `ac6recomp.exe`.** On first run it creates `ac6recomp.toml` beside the executable, which contains the game settings.

6. **In-game keys:**
   - `F3` - toggle FPS overlay
   - `F4` - settings overlay (browse and change most options live)
   - `F11` - toggle fullscreen

---

## Important console variables

Settings live in **`ac6recomp.toml`** next to the executable. Most can also be changed live in the `F4` overlay. Some only take effect after a restart.

> [!IMPORTANT]
> In `.toml`, **use forward slashes in paths**. A single backslash is an escape character and one bad line silently voids the entire file, sending every setting back to its default.

| cvar | default | what it does |
|---|---|---|
| `user_language` | `1` | Sets the language. `1` for English, `2` for Japanese. |
| `fullscreen` | `false` | Start in fullscreen (`F11` toggles at runtime) |
| `draw_resolution_scale_x` | `1` | Internal render scale on the x axis. `1` for 1280, `2` for 2560, `3` for 3840, etc. |
| `draw_resolution_scale_y` | `1` | Internal render scale on the y axis. `1` for 720, `2` for 1440, `3` for 2160, etc. |
| `ac6_unlock_fps` | `true` | Allow framerates above the stock 30 fps |
| `ac6_dynamic_vblank` | `true` | Keep menus and cutscenes at their native 60 Hz pacing while gameplay runs at your target |
| `ac6_performance_mode` | `true` | Reduces logging and diagnostic overhead |
| `ac6_widescreen` | `false` | Ultrawide support (hor+) in missions. Menus and hangar stay 16:9 |
| `ac6_widescreen_cinematics` | `true` | With ultrawide on, also widen in-engine cinematics. They are staged for 16:9, so this can expose set edges |
| `ac6_terrain_hd` | `true` | Draw terrain at the full shipped resolution (2× what the console drew), which also removes terrain cracks |
| `ac6_fullres_effects` | `false` | Draw clouds, smokes, trails, and afterburner effect at native resolution (2× what the console drew), slightly affects performance |
| `ac6_cursor_hide_seconds` | `3.0` | Hide the mouse cursor after this many idle seconds. `0` = never hide |
| `ac6_kbm_enabled` | `false` | **Enable keyboard and mouse controls.** Off by default — controllers work out of the box |
| `ac6_kbm_config` | `ac6_input.toml` | Path to the key bindings file. Edits are picked up live |
| `ac6_texture_swaps_enabled` | `false` | Enable texture replacement mods (see [Modding docs](#modding-docs)) |

For 1440p, set both `draw_resolution_scale_x` and `draw_resolution_scale_y` to `2`. For 2160p or 4K, set them to `3`.

For 1440p Ultrawide, it is recommended to set `draw_resolution_scale_x` to `3` and `draw_resolution_scale_y` to `2`.

### Fixes

These are on by default and exist so a problem can be isolated. Turning one off restores the original behavior, bugs included.

| cvar | default | what it fixes |
|---|---|---|
| `ac6_effect_mode_fix` | `false` | **IMPORTANT: If your clouds and afterburner effects are flickering, set this to `true`!** |
| `ac6_fix_scaling` | `true` | Upscaling mosaic on deferred passes (only active above 1× scale) |
| `ac6_fix_deswizzle` | `true` | Mosaic and streak artifacts from the game's manual texture de-swizzle |
| `ac6_fix_dof` | `true` | Cutscene depth-of-field striping and ghosting above 1× scale |
| `ac6_fix_trails` | `true` | Invisible missile and jet trails |
| `ac6_fix_water_line` | `true` |  Lines across open water seen at a grazing angle |
| `ac6_flare_drop_quad2` | `true` | Faint rectangle around the sun |
| `ac6_cutscene_resync` | `true` | Keeps cutscene video locked to its audio after a render hitch |

---

## Default keybinds

Controllers work with no configuration. **Keyboard and mouse are off by default**. Set `ac6_kbm_enabled = true` to enable them.

Bindings live in `ac6_input.toml`, created next to the executable on first run after `ac6_kbm_enabled` is enabled. By default, the keybinds are similar to Ace Combat 7.

### Flight

| action | keys | | action | keys |
|---|---|---|---|---|
| Pitch down / up | `1` / `3` | | Fire machine gun | `Mouse1`, `Left Ctrl` |
| Roll left / right | `A` / `D` | | Fire missile | `Mouse2`, `Space` |
| Yaw left / right | `Q` / `E` | | Change weapon | `Mouse wheel`, `C` |
| Accelerate | `W` | | Switch targets | `Tab` |
| Decelerate | `S` | | Change view | `V` |
| High-G turn | `2` | | Radar / map | `R` |
| Autopilot | `Z`, `X` | | Landing gear | `G` |
| Pause | `Escape` | | | |

**Mouse** steers the aircraft. Hold `Left Alt` (camera control) to look around with the mouse instead of steering.

**Camera:** `Numpad 8` / `2` / `4` / `6` — up / down / left / right.

**Wingman commands:** arrow keys.

### Menus

| action | keys |
|---|---|
| Navigate | `W` `A` `S` `D` or arrow keys |
| Confirm | `Space`, `Mouse1` |
| Cancel | `Escape`, `Backspace`, `Mouse2` |
| Start | `Enter` |
| Back | `Tab` |

---

## How to build

### Option A: Automatic setup (recommended)

The automated build script handles environment setup, ISO extraction, and the multi-step CMake build:

1. Place your legally obtained **Ace Combat 6 ISO** in the repository root directory.
2. Run `setup_and_build.bat`. The script locates Visual Studio, extracts the assets, runs codegen, and builds the runtime using the MSVC backend.
3. Copy the compiled executable and default config to the root directory to run:
   ```powershell
   Copy-Item out/build/win-amd64-relwithdebinfo/ac6recomp.exe . -Force
   Copy-Item out/build/win-amd64-relwithdebinfo/ac6recomp.toml . -Force
   ```
4. Run `ac6recomp.exe` directly from the root directory.
5. Optionally create a shortcut to the executable.

### Option B: Manual build

```bash
cmake --preset win-amd64-relwithdebinfo
cmake --build --preset win-amd64-relwithdebinfo --target ac6recomp_codegen
cmake --preset win-amd64-relwithdebinfo
cmake --build --preset win-amd64-relwithdebinfo
```

The executable is placed at `out/build/win-amd64-relwithdebinfo/ac6recomp.exe`.

On Windows, use the preset commands above rather than plain `cmake -L` in the repo root. If you previously configured from an `x86` Visual Studio prompt or with the wrong compiler on `PATH`, delete `out/build/win-amd64-relwithdebinfo` and re-run the preset from a normal 64-bit PowerShell/CMD window or an x64 Native Tools prompt.

### Building on Linux

The Linux build uses the Vulkan backend. Tested on Debian with an AMD Radeon 660M (Mesa RADV).

**Not available on Linux yet:** keyboard and mouse controls (use a controller), texture replacement mods, and networking.

**1. Install the dependencies** (Debian/Ubuntu package names):

```bash
sudo apt install clang cmake ninja-build pkg-config git python3 \
  libgtk-3-dev libx11-xcb-dev \
  libpulse-dev libpipewire-0.3-dev libasound2-dev \
  libudev-dev libdbus-1-dev \
  mesa-vulkan-drivers libvulkan1
```

- **clang 19 or newer** is required (`clang --version`). Older versions fail on C++23 features the SDK uses. If your default `clang` is older, install `clang-19` and add `-DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19` to the `cmake --preset` commands below.
- The audio (`libpulse`, `libpipewire`, `libasound2`) and `libudev` packages matter: the bundled SDL only enables sound and controller hotplug for the ones it finds at build time. Without them the build succeeds but you get no sound or no controller.

**2. Get the code and the game executable:**

```bash
git clone -b linux https://github.com/termuxuser123/AC6_recomp.git
cd AC6_recomp
```

The build needs the game's `default.xex` extracted to `assets/default.xex` - it cannot read it from inside the `.iso`. Extract the disc with [xdvdfs](https://github.com/antangelo/xdvdfs/releases):

```bash
./xdvdfs unpack "/path/to/Ace Combat 6.iso" assets
ls assets/default.xex
```

**3. Build.** The first configure only sets up the SDK and the recompiler, codegen then writes the recompiled game code into `generated/`, and the second configure picks it up:

```bash
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo --target ac6recomp_codegen
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo
```

Codegen prints many `Unable to decode instruction` warnings - those are data tables inside the game's code section and are expected. After a normal code update (`git pull`) only the last command is needed; re-run all four only when the codegen config changes.

**4. Run.** The game looks for its files next to the executable:

```bash
cd out/build/linux-amd64-relwithdebinfo
ln -s ../../../assets assets
./ac6recomp
```

If it exits immediately with code 1, it did not find `assets/default.xex` (or a `.iso`) next to the executable.

**Performance tips for weaker GPUs / laptop iGPUs:**

```bash
./ac6recomp --no-ac6_unlock_fps --no-ac6_terrain_hd --render_target_path_vulkan=fbo --no-vsync
```

- `--no-ac6_unlock_fps` locks to the original 30 FPS. If the GPU cannot sustain 60, this gives much steadier frame pacing than an unlocked 30-45.
- `--no-ac6_terrain_hd` draws terrain at console detail instead of 2x.
- `--render_target_path_vulkan=fbo` uses the faster render target path.
- `--no-vsync` avoids frames just over the refresh interval being held an extra refresh.
- On laptops, plug in and use the performance power profile (`powerprofilesctl set performance`).

Press `F3` to see host (displayed) and guest (game) FPS. To make settings permanent, set them in `ac6recomp.toml` next to the executable (for example `ac6_unlock_fps = false`) instead of passing flags.


---

## Repository policy

This repository contains source code only.

Do **not** commit or redistribute:

- retail game data
- `default.xex`
- disc images, packages, title updates, or firmware files
- console keys or any other proprietary Microsoft / publisher material

Users must supply their own legally obtained game files locally.

## Modding docs

- [Texture Swap Modding Guide](docs/TEXTURE_SWAP_MODDING_GUIDE.txt)
- [Texture Swap Reference](docs/TEXTURE_SWAPS.txt)


## Project layout

```text
AC6_recomp/
|- src/
|  |- ac6_backend_fixes/       AC6-specific backend diagnostics and fix routing
|  |- ac6_native_graphics.*    AC6 frame-boundary analysis and overlay status
|  |- ac6_native_renderer/     Experimental replay renderer and research tooling
|  |- d3d_hooks.*              Guest D3D capture and shadow-state hooks
|  `- render_hooks.*           Timing and frame pacing hooks
|- thirdparty/rexglue-sdk/     Vendored RexGlue SDK
|- generated/                  Generated recomp sources
|- assets/                     Local game files, not kept in repo
`- docs/RENDERER_ARCHITECTURE.txt
```

## License

See [LICENSE](LICENSE).
