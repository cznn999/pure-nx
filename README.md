# pure-nx

A Nintendo Switch port of **Pure** (Xbox 360, 2008) by static recompilation with
[ReXGlue](https://github.com/rexglue), drawn through Renut's Direct3D-level renderer on deko3d.

**No game files are included.** You need your own legally obtained copy of Pure (Xbox 360). This repository holds
only the port's source code; the recompiled code (`generated/`) and the shader packages (`switch_shaders/`) are made
from your copy of the game on your machine.

## Status

- Full races with bots, 480p internal resolution, FSR-style upscale to the screen.
- ~30 fps in races with a CPU overclock (sys-clk, ~1785 MHz). At stock clocks (1020 MHz) races run at ~17-20 fps: the
  game's CPU work is the limit, not the GPU. I've spent several days trying to minimize the CPU bottleneck and for now I've hit the ceiling.
- Using 4 CPU cores is **recommended** (launched through a Sphaira forwarder, see below).

## Installing (players)

1. Copy `switch/pure-nx/pure.nro` from the release to the SD card.
2. Copy your extracted game (the folder with `default.xex`) to `sdmc:/switch/pure-nx/game_root/`.
3. Optional, for four cores: create a forwarder of `pure.nro` with Sphaira ("Install Forwarder") and start the game
   from it twice. On the first start Pure writes `sdmc:/atmosphere/contents/<forwarder id>/exefs/main.npdm` (an NPDM
   that grants core 3); it never writes one for a retail title's id.

The first start compiles a few shaders (short pauses); they are cached in `sdmc:/switch/pure-nx/`.

## Settings

`sdmc:/switch/pure-nx/pure.toml` is written on the first start; any key there overrides the build's defaults
(`pure_switch_defaults.toml`):

| Key | Default | |
|---|---|---|
| `pure_fsr` | `true` | FSR-style edge-adaptive upscale with sharpening; `false` = bilinear |
| `pure_fsr_sharpness` | `0.3` | 0.0-2.0, read at start |
| `input_xbox_layout` | `true` | face buttons by position (Xbox layout); `false` = by letter |
| `log_to_file` | `true` | logs in `sdmc:/switch/pure-nx/logs/` |

## Building (Windows)

Needs devkitPro (devkitA64, libnx, switch-zstd), Visual Studio's CMake and Ninja, Python 3, and the sa2-nx tree next
to this one (`..\sa2-nx`: plume, the patched uam, XenosRecomp and the Switch toolchain file).

0. `setup.cmd` - once: fetches the SDK's third-party sources, builds `rexglue.exe` and runs step 1. Put your
   extracted game (with `default.xex`) in `game_root\` first.
1. `codegen.cmd` - recompiles `default.xex` (path in `pure_manifest.toml`) into `generated/default`.
2. `switch_shaders/` - made on Linux from the game's shaders with `tools/switch_shaders/make_switch_shaders.sh`.
3. `build_switch.cmd` (or `build_switch_pgo.cmd`) - builds `out\nx\pure.nro`.
4. `package_switch.cmd [drive]` - the SD card folder.

## License

GPL-3.0 (`LICENSE`), as the sa2-nx / banjo-nx code it is built on. Third-party parts keep their licenses
(`tools/rexglue-sdk-pure/THIRD_PARTY_NOTICES.md`). Pure is a trademark of its owners; this project is not affiliated
with or endorsed by them.
