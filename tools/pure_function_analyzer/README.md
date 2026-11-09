# Pure-NX PPC function analyzer

`analyze.py` reads ReXGlue unresolved-branch diagnostics, joins them to the
already collected `veneer-classification.csv` and import audit, and can inspect
an optional flat, decoded XEX guest-image snapshot. It does not invoke ReXGlue,
modify the main manifest/overrides, or build the project.

## Safe first run (dry-run)

From the project folder:

```powershell
python tools\pure_function_analyzer\analyze.py `
  --log out\logs\pure-veneer-batch2.log `
  --classification-csv out\probe\veneer-classification.csv `
  --analysis-report out\logs\pure-remaining-537-analysis.txt
```

Without `--write`, output is printed only. To write a report and an isolated
temporary overrides file, supply both explicit output paths and `--write`:

```powershell
python tools\pure_function_analyzer\analyze.py `
  --log out\logs\pure-veneer-batch2.log `
  --classification-csv out\probe\veneer-classification.csv `
  --analysis-report out\logs\pure-remaining-537-analysis.txt `
  --write `
  --report out\probe\pure-function-analysis.txt `
  --overrides out\probe\pure-function-overrides.toml
```

The override writer only includes prior-audit group A rows, and only when the
row reports executable code with no PDATA/import overlap. Import veneers and
unknown CTR destinations remain separate categories. Cached CSV classifications
are labeled as prior evidence; they are not presented as byte-level rechecks.

## Load XEX through ReXGlue

`exporter/` is a small host utility that calls the SDK `Runtime::Setup` and
`Runtime::LoadXexImage` APIs, then reads the mapped `XexModule` memory and its
PE section, exception-directory, and import metadata. It does not implement
XEX decryption, compression, or LZX. The flat image preserves guest VAs from
`image_base`; unmapped gaps are zero-filled and remain explicitly unmapped in
the JSON sidecar. Import names are not exposed by `XexModule`; library and
ordinal are retained. `XexModule` exposes no relocation enumeration API, so
the sidecar records that limitation.

Build the helper in an isolated output directory, linking the already-built
host SDK runtime (this does not build the game or SDK):

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' `
  -S tools/pure_function_analyzer/exporter `
  -B out/probe/pure-function-analyzer/exporter-build-vs `
  -G Ninja `
  -D CMAKE_CXX_COMPILER='C:/Program Files/LLVM/bin/clang++.exe' `
  -D CMAKE_RC_COMPILER='C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/rc.exe' `
  -D CMAKE_MAKE_PROGRAM='C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe' `
  -D CMAKE_BUILD_TYPE=RelWithDebInfo `
  -D REXGLUE_SDK_ROOT=../sa2-nx/sdk `
  -D REXGLUE_SDK_LIB_DIR=../sa2-nx/sdk/out/win-amd64
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' `
  --build out/probe/pure-function-analyzer/exporter-build-vs --target pure_xex_exporter
out/probe/pure-function-analyzer/exporter-build-vs/pure_xex_exporter.exe `
  <game folder>/default.xex out/probe/pure-function-analyzer
```

The exporter writes `out/probe/pure-function-analyzer/image.bin` and
`image-map.json`. The analyzer validates the image length against the map,
indexes literal BE32 pointers and `lis` plus `addi/ori` address constructions,
checks exact PPC veneer bytes and signed CTR construction, compares targets to
PDATA/known function extents/imports, and reports overlap and branch-source
evidence. It seeds the bounded pointer-fixpoint with PDATA functions, the XEX
entry point, and explicitly supplied accepted probe overrides. Each inferred
candidate keeps its source address, reference type, prolog/terminator evidence,
and overlap result. Prolog plus a scanned terminator is heuristic evidence;
pointer-fixpoint candidates are never written to overrides automatically.

Example full analysis (no codegen):

```powershell
python tools/pure_function_analyzer/analyze.py `
  --log out/logs/pure-veneer-batch2.log `
  --classification-csv out/probe/veneer-classification.csv `
  --analysis-report out/logs/pure-remaining-537-analysis.txt `
  --accepted-overrides out/probe/pure-veneer-batch-overrides.toml `
  --accepted-overrides out/probe/pure-veneer-batch2-overrides.toml `
  --image out/probe/pure-function-analyzer/image.bin `
  --map out/probe/pure-function-analyzer/image-map.json `
  --write `
  --report out/logs/pure-function-analysis-full.txt `
  --overrides out/probe/pure-function-analyzer/pure-function-overrides-full.toml
```
