# Pure (425607D9) - Direct3D census for Renut (deko3d)

Renderer direction (user decision, 2026-11-06): **Renut's Direct3D-level renderer on deko3d, as dishonored-nx**. No
NVK in the NRO (`REXGLUE_SWITCH_NO_NVK=ON`, the `out\nvk-stub` trick of dishonored-nx's `build_switch.cmd`). The Xenos
plugin stays for the PC reference build and for the runtime census below.

## The big difference from Dishonored

Dishonored and SA2 use Direct3D **2.0.21076** (LTCG), and Renut's hooks (`renut_engine/gpu/d3d/hooks/*.cpp`) assume
that build's device layout (VS at device+13072, PS at device+13068, ...). Pure's XEX imports XAM/kernel
**2.0.6995.0** (2008). Its Direct3D is about four years older: function addresses, the device layout and possibly some
call shapes differ. Nothing in Renut's hooks can be reused at SA2/Dishonored addresses or offsets without checking it
against Pure.

## Static PM4 scan (2026-11-06, `tools/census/pm4_scan.py` over `generated/default`)

97,965 generated functions parsed. Type-3 PM4 headers built as constants (`lis`+`ori`/`addi`) appear in 112
functions; full list in `tools/census/pm4_scan_pure.txt`. They cluster in:

- **0x829CE000-0x82A18000**: the Direct3D library (ring, draws, resolves, fences, tiling).
- **0x82C85400-0x82C88000**: `SET_CONSTANT` / `SET_STATE` / `LOAD_CONSTANT_CONTEXT` builders (probably the D3D
  state/constant upload helpers or XGraphics; to confirm).
- Isolated hits at 0x82A5A208, 0x82B5xxxx, 0x82CBD158, 0x82E5E390, 0x82E6F188, 0x82ECA9E0 have impossible packet
  counts (15522, 16158, 16384): data constants, not PM4.

### Candidates (by the packets they build; to confirm with the runtime census)

| Address | Packets | Guess |
|---|---|---|
| 829DDB40, 829EA750 | ME_INIT(18) | device / ring initialisation |
| 829CF738, 829E8D40 | INDIRECT_BUFFER(2) (+ WAIT_REG_MEM) | KickOff / ring segment submit |
| 82A066C0, 82A08C80 | WAIT_REG_MEM + DRAW_INDX(4 dwords) | indexed draw (DMA index buffer) family |
| 82A06510, 82A08AD0, 82A082C8 | DRAW_INDX_2 + WAIT_REG_MEM | auto-index / immediate draw family |
| 829DC4C8, 829DC510, 82A0B8A0, 82A0BCE0 | DRAW_INDX_2(1) | small internal draws (clear / resolve quad?) |
| 829E7048, 829E7200, 829DC0A0, 829CEFB0, 82A12C20, 82A12D58, 82A13108, 82A15640, 829F3640 | INVALIDATE_STATE + IM_LOAD_IMMEDIATE | internal shader loads (clear / resolve / present paths) |
| 829F5C68, 829F8C10 | IM_LOAD(2) | draw-time shader load from shader objects (FlushState shader part?) |
| 829F89A0 | LOAD_ALU_CONSTANT(3) | shader constant upload |
| 829EF248, 82A09978 | EVENT_WRITE_CFL + WAIT_UNTIL_READ + MEM_WRITE | Resolve (copy) path |
| 82A04F88, 82A050C0 | COND_WRITE(6) | fence / BlockOnFence family |
| 82A06258, 82A063D0, 82A068E8, 82A08EA8, 82A08FE8, 82A0E2C0, 82A0E370, 82A17018 | EVENT_WRITE_SHD(3) | fence writes (InsertFence / Swap) |
| 829EE790, 829EE890 | VIZ_QUERY | occlusion queries (Begin/End) |
| 829EB378 ... 829ED0E0 (6 functions) | SET_BIN_MASK x2 | predicated tiling (BeginTiling / tile loop) |

Note: the 21076 builds draw with `DRAW_INDX` 2-dword packets; none of Pure's functions builds one as a constant, so
the auto-index draws either use `DRAW_INDX_2` or compose the header at run time. Treat the table as candidates only.

## Still required before Renut

1. **Runtime census** (dishonored-nx's `src/d3d_census.cpp` method): hook every function of 0x829CE000-0x82A18000 and
   0x82C85400-0x82C88000, count calls per 5 s and log the first calls' r3..r8, on the PC build with the Xenos plugin.
   Needs the PC build to link (fixed in CMakeLists.txt: `WIN32` executable, 2026-11-06) and to reach gameplay.
2. Name Draw*, SetVertexShader/SetPixelShader (or the draw-time shader load), SetTexture, SetRenderTarget,
   Resolve, Swap/present, KickOff, the device pointer, and the device offsets of the bound VS/PS.
3. Compare each hooked function's contract with the 21076 one Renut expects; adapt the hooks in a Pure-owned copy
   of `renut_engine` (not in sa2-nx).
4. Shader containers for Renut's translator: dump at creation time (`dishonored_dump_shaders` method) and/or extract
   from the game files (`global_shader_combos`, `.ark`). Then the Switch shader package (`switch_shaders/`), which can
   be built on Linux (XenosRecomp + DXC + SPIRV-Cross + uam), as for dishonored-nx.

## Matching Renut's hooked functions to Pure (2026-11-06, static)

`tools/census/match.py` compares each function Renut hooks or calls in dishonored-nx (86 addresses read from
dishonored-nx's `renut_engine/gpu/d3d` and `src/*.cpp`; 60 are Dishonored addresses, the rest SA2 / N&B addresses
quoted in comments) with every generated Pure function of 0x829C0000-0x82A20000 and 0x82C85000-0x82C88000: opcode
sequence similarity, normalised instruction similarity, and shared device offsets. Result: `tools/census/match.txt`
(top 3 candidates each). Strong, unique matches (score >= 0.7) include:

| Dishonored | Pure | Function |
|---|---|---|
| 82A49988 | 829EC5D8 | D3DDevice_DrawIndexedVertices |
| 82A49570 | 829EC0E8 | D3DDevice_DrawVertices |
| 82A48B28 | 829EB378 | D3DDevice_BeginVertices |
| 82A49030 | 829EB9A8 | D3DDevice_BeginIndexedVertices |
| 82A48FE8 | 829EB948 | D3DDevice_DrawVerticesUP |
| 82A4DB18 | 829EA048 | CDevice::KickOff |
| 82A4D6D8 | 829E9E60 | CDevice::KickOffSegment |
| 82A4C528 | 829EA378 | CDevice::BeginRingBig |
| 82A4E8C8 | 829F4540 | SynchronizeToPresentationInterval |
| 82A50970 | 829F7CC8 | SetStreamSource |
| 82A517E0 | 829F8C10 | draw-time shader load (FlushState) |
| 82A523B0 | 829CEEA8 | CBlocker::Check |
| 82A48688 | 829DD270 | (exact) |
| 82A48950 | 829DD6C8 | CreateVertexDeclaration |
| 82A4B0C0 | 829D6F00 | (texture/surface layout, 46/49 offsets equal) |
| 82A63610 | 82A13528 | VdRetrainEDRAM |
| 82A645A8 | 82A0A508 | initial state packets |

Weak or ambiguous, to check by hand: BlockOnFence (82A4DC18 -> 829E9530 0.56), D3DDevice_Swap (82A4F610 ->
829F4B68 0.61), AllocateInlineData (82A4CB20 -> 829E8BF0 0.43), the ring segment allocator (82A4CCC0 -> 829E9AF0),
the occlusion query path (82A549E0 -> 82A086C0), the device creation path (82A5F9A0, 82A5EE88, 82A5F168), and the
UE3-specific Dishonored functions (8273E7A8, 827428B0, 82742AB0, 82745180, 82745360), which have no Pure equivalent.

## Device layout: Pure is N&B's, not SA2's / Dishonored's

`tools/census/offmap.py` aligns the instructions of the matched pairs and pairs their device offsets
(`tools/census/offmap.txt`). The public device (D3DTAGCOLLECTION, constants, up to 0x2A48) is identical. The private
part is shifted in blocks: Dishonored 0x2B08-0x2B3F -> Pure -128 (0x2A88-0x2ABF), the vertex declaration -420/-432,
the bound objects 0x30D2-0x34F8 -> -376 (bound VS 13072 -> 12696, PS 13068 -> 12692), and the fields from ~0x3520 on
-368 ... -404. That is exactly the inverse of the shifts sa2-nx documents between N&B's Direct3D (2.0.7645) and
SA2's (2.0.21076) in `renut_engine/gpu/d3d/d3d.h`. Examples: index buffer Pure 0x309C = N&B 0x309C; creation thread
id Pure 10888 = N&B 10888. So the original reNut (masterspike52/reNut, N&B) offsets are the right starting point for
Pure; each one still has to be checked against Pure's own code before use.

## Shader containers for Renut (2026-11-06)

`global_shader_combos/global_shader_combos.fxc` holds 200 whole Xbox 360 shader containers (magic 0x102A11xx,
virtual header + physical block, validated as reNut's tools/dump_shaders.py does): 100 vertex, 100 pixel. Extracted to
`renut_shaders/` as `{vs,ps}_<xxh3_64 of the container>.bin` (reNut's naming, which Renut's shader cache keys on;
`renut_shaders.zip`). The default.xex image has none. The `.ark` archives (levels, Resident, Frontend) are compressed
with Black Rock's own LZ scheme (no container magic in the raw files): any material shaders inside them still have to
come from there or from a creation-time dump on the console.

## Renut on Pure (2026-11-06)

`src/renut_engine` is dishonored-nx's copy, retargeted. What changed from Dishonored's:

- **Device layout** (`gpu/d3d/d3d.h`, `d3d_native.h`, `hooks/device.cpp`, `backend/queued.cpp`,
  `sa2_setter_marks.h`, `sa2_renut_pacing.cpp`, `render/draw.cpp`): N&B's first-release offsets
  (the bracketed values sa2-nx kept), read from Pure's own code. Direct3D_CreateDevice 0x829CFB38
  allocates 0x5E00 and stores the behaviour flags at 23932 (N&B 23980, the only offset that is
  not N&B's). The bound-resource test 0x829E6F20 reads declaration 0x2E2C, index buffer 0x3094,
  targets 0x3098, depth 0x30A8, streams 0x30AC, textures 0x3100, PS 0x3194, VS 0x3198.
  IsFencePending 0x829EA250 reads the fence tag 0x2A9C and the completion word 0x2A90 through
  VdGlobalDevice (slot 0x83850B84).
- **Hooks**, each at a function of Pure's generated code (Dishonored's address in the source comments):

  | Pure | Function |
  |---|---|
  | 82A04440 | D3D::InitializeHardwareDevice (replaced: the device without a GPU) |
  | 829F4540 | SynchronizeToPresentationInterval |
  | 829EA048 / 829E9E60 | KickOff / KickOffSegment |
  | 829EA378 / 829E8970 / 829E8BF0 / 829E9AF0 | BeginRingBig / RingBufferDeviceAllocate / AllocateInlineData / segment allocator |
  | 829F4B68 | D3DDevice_Swap |
  | 829E9530 / 829EA250 / 829CEEA8 | BlockOnFence / IsFencePending / CBlocker::Check |
  | 829D0388 | window offset and scissor |
  | 82A086C0 | occlusion query ZPASS event (device, query, index): a free-running count |
  | 829EC0E8 / 829EC5D8 / 829EB948 / 829EB378 / 829EB9A8 | DrawVertices / DrawIndexedVertices (5 args) / DrawVerticesUP / BeginVertices / BeginIndexedVertices |
  | 829F3E58 / 82A017A8 | ClearF / Resolve (N&B's arguments: stencil at r1+92) |
  | 829DD270 / 829DCF98 / 829DD6C8 / 829F7CC8 | XGRegisterVertexShader / XGRegisterPixelShader / vertex declaration in place / D3DVertexShader_Bind |
  | 829E6100 | D3DResource_Destroy |

  Called from the creation hook: 829CEFB0, 829DC510, 82A0A508 (initial packets), 82A03DB0
  (InitializePresentationParameters), 82A03AA8 (InitializeApiState), 82A0E098, 829D4160.
  Left out (the GPU's): 82A04308 (VdInitializeEngines and co.), 829EA750 (ring buffer),
  82A08028 (interrupt DPC), 82A13528 (VdRetrainEDRAM), 82A13830.
- Dropped: Dishonored's UE3 hooks (occlusion batch harvest 827428B0) and the LTCG argument
  workarounds (Pure's Direct3D takes N&B's arguments).
- **Shaders**: `switch_shaders/` holds the 200 containers of `global_shader_combos.fxc`
  (`renut_shaders.zip`), made by `tools/switch_shaders/make_switch_shaders.sh`. The rest are
  created by the game at run time: `pure_dump_shaders = true` writes them to `shaders/` beside
  the NRO for the next package. One vertex shader (C6F5CAC68534B40F) exports FOG0:
  `patches/xenosrecomp-pure.patch` sends it to the scratch output.
