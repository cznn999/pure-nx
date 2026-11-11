# Pure-NX port notes

These notes distinguish observations from the Pure files and project archive from decisions that still need runtime verification. Do not copy game-specific offsets or tuning from another port without proving that the Pure XDK layout matches.

## Project identity and inputs

- Game: **Pure** (Xbox 360).
- Title ID: `425607D9` (from the existing Pure project analysis; re-check against the installed package metadata before packaging).
- Host project name: `pure`.
- ReXGlue manifest: `pure_manifest.toml`.
- Game data root in the manifest: `<game folder>`.
- Main executable: `default.xex`.
- The game data and all generated game code remain outside version control and outside distributable source archives.

## Code generation status

The existing Windows workspace completed ReXGlue code generation before this source snapshot was made:

- 98,201 functions processed.
- 180 `pure_recomp.*.cpp` translation units generated.
- Last recorded codegen summary: 367 written, 0 unchanged, 0 deleted; approximately 550.6 seconds.
- The generated output directory `generated/default` was intentionally excluded from the uploaded source archive. Do not interpret its absence from that archive as a failed or missing codegen run. Keep the user's local generated output intact.

The earlier baseline had 2,092 unresolved unconditional branch edges. The user has already applied the isolated veneer-discovery work in `tools/pure-veneer-discovery-1.0.4.patch` to the SDK worktree and subsequently obtained a successful codegen run. The successful run is established; the available log did not show the patch's `Tail veneer` diagnostic, so do not claim that every edge was resolved specifically by that detection path.

## Renderer direction

### Staged reference path

1. Keep the ReXGlue/Xenos GPU plugin as the **PC reference path** while getting the game through startup and into gameplay. This isolates CPU, kernel, filesystem, input, audio and import problems before replacing graphics.
2. After a working PC reference exists, integrate reNut at the game's Direct3D API boundary.
3. Use Plume's Deko3D backend for the Switch renderer. The final Switch path must not link the NVK driver.

This is an API-level renderer path: Pure's Direct3D calls are hooked and replayed through reNut/Plume. It is not a new Deko3D backend inside `rexgpu-xenos`, and it does not require building a PM4/EDRAM renderer as an intermediate step.

### Reusable source and non-reusable source

SA2-NX is a read-only reference for the working reNut/Plume/Deko3D integration. Reuse only the general renderer, shader-tooling and platform-build pieces after checking their dependencies. Do **not** copy SA2-specific D3D hook addresses, device-field offsets, timing/pacing hooks, screen hooks, game cvars, or game shaders into Pure.

The uploaded source snapshot contains the SA2-NX worktree under `tools/rexglue-sdk-pure`. Its `.git` file points to an external worktree path on the user's Windows machine; it is not an independent Git repository in this archive. Treat it as read-only reference material. Changes for Pure belong in Pure-owned files, not in that nested source tree.

## Pure-specific compatibility census

Observed from the supplied Pure executable and earlier analysis:

- The XEX imports refer to XAM/kernel library version `2.0.6995.0`. This is an import-library version observation, **not yet a definitive identification of the full XDK build**.
- `index.xml` lists 440 entries; the game uses multiple `.ark` containers. These game assets are not included in the project source archive.
- A `global_shader_combos` shader data file was found in the extracted game tree in an earlier local inspection. This does not establish that it is the only shader source.

Still required before enabling reNut:

- Identify the Pure D3D device layout and the offsets used by the hooked functions.
- Match retail-XEX D3D functions against a known XDK build and verify signatures/call layouts; do not reuse SA2 addresses.
- Census every shader-bearing input: `default.xex`, all relevant `.ark` files, standalone shader-combo files and any update/DLC content supplied for this title.
- Determine whether shader containers are standard Xenos containers accepted by reNut's XenosRecomp extractor or need a Pure-specific extractor/adapter.
- Map Pure's vertex/pixel shader object fields and draw/flip calls to reNut's expected hooks.

The source archive does not include the `.ark` game data, so the complete shader census cannot be completed from that archive alone. Do not upload or commit game assets; run census tools locally against `<game folder>` and keep their output in the build/work directory.

## Deko3D / no-NVK integration notes

The SA2 reference has two independent no-NVK switches:

- ReXGlue SDK: `REXGLUE_SWITCH_NO_NVK`.
- Plume: `PLUME_SWITCH_NO_NVK`.

Both need to be configured for a Switch build that uses Plume/Deko3D without the Mesa NVK archive. In the vendored Plume CMake observed in the reference tree, the existence check for `PLUME_SWITCH_NVK_LIBRARY` occurs before the `PLUME_SWITCH_NO_NVK` branch. When copying Plume into the Pure-owned tree, review and correct that guard so an absent NVK archive is accepted when `PLUME_SWITCH_NO_NVK=ON`. Do not modify the SA2 reference copy.

The SDK's Switch CMake currently uses Vulkan API support alongside its optional NVK driver linkage. `REXGLUE_SWITCH_NO_NVK` disables the driver linkage; it is not the same as disabling all Vulkan API code. Do not turn off `REXGLUE_USE_VULKAN` without first verifying the SDK's backend-selection constraints and the exact Switch startup path.

## Current project bootstrap

The root project previously included `generated/rexglue.cmake` without setting `REXSDK_DIR`, although the isolated SDK source tree is already located at `tools/rexglue-sdk-pure/sdk`. The first Pure-owned change sets that local path as the default while preserving an explicit `-DREXSDK_DIR=...` override.

The current root target still selects `GPU_PLUGINS xenos`. That is intentional for the PC reference phase. Do not remove it until the Renut PC path has been integrated and verified; do not use the Xenos plugin as the final Deko3D Switch renderer.

## Runtime and packaging constraints

- Do not run two builds at once.
- Do not build while Ryujinx is running.
- Before launching Ryujinx, verify no other instance is running and at least 8 GB of free commit memory is available.
- Stop only the PID of a Ryujinx process started for this port.
- No PGO/LTO until explicitly authorized and the prescribed console profiling sequence is complete.
- No Git push. Keep game data and generated game code out of commits and packages.
