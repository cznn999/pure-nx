#!/usr/bin/env bash
# pure-nx (2026-11-06): switch_shaders/ from renut_shaders/ (the game's shader containers), on Linux, as dishonored-nx's
# package was made:
#   1. XenosRecomp (sa2-nx's app/thirdparty/reblue-XenosRecomp with patches/xenosrecomp-dishonored.patch of dishonored-nx
#      and patches/xenosrecomp-pure.patch, RENUT_RECOMP, no DXIL) with BANJO_DEKO3D_SPIRV_DIR: shader_cache.cpp and one
#      SPIR-V a shader
#   2. sa2-nx's tools/deko3d_shaders.py: SPIRV-Cross, tools/deko3d_glsl.py, uam (a check that each compiles)
#   3. tools/deko3d_glsl_opt.py with every pass but scale1 and fragscale (any render resolution), then clamp7e3.py
#   4. tools/deko3d_shader_package.py: deko3d_glsl.bin and its .incbin source
#   5. shader_cache_switch.cpp.in with an empty SPIR-V blob (the deko3d back end reads the GLSL package)
# host_shaders/*.deko3d.h are Renut's own shaders: dishonored-nx's, unchanged (the same src/renut_engine/gpu/shaders).
#
#   make_switch_shaders.sh <sa2-nx> <xenosrecomp exe> <spirv-cross> <uam> <zstd> <renut_shaders> <out>
# XenosRecomp needs LD_LIBRARY_PATH with libdxcompiler.so (its thirdparty/dxc-bin/lib/x64).
set -euo pipefail
SA2NX=$1 XR=$2 SPIRV_CROSS=$3 UAM=$4 ZSTD=$5 IN=$6 OUT=$7
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T="$SA2NX/tools"
W="$OUT/work"
rm -rf "$W"; mkdir -p "$W/spirv" "$W/gen" "$W/pkg" "$OUT/host_shaders"

BANJO_DEKO3D_SPIRV_DIR="$W/spirv" "$XR" "$IN" "$W/gen/shader_cache.cpp" \
    "$SA2NX/app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h"

python3 - "$T" "$SPIRV_CROSS" "$UAM" "$W" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import deko3d_shaders as m
m.SPIRV_CROSS, m.UAM = sys.argv[2], sys.argv[3]
sys.argv = ['deko3d_shaders.py', '--spirv', sys.argv[4] + '/spirv', '--out', sys.argv[4] + '/d3']
m.main()
EOF

PASSES=$(python3 -c "import sys;sys.path.insert(0,'$T');import deko3d_glsl_opt as o;print(','.join(n for n,_ in o.PASSES if n not in('scale1','fragscale')))")
python3 "$T/deko3d_glsl_opt.py" --in "$W/d3/glsl" --out "$W/d3/glsl_scaled" --passes "$PASSES"
python3 "$HERE/clamp7e3.py" "$W/d3/glsl_scaled" "$W/d3/glsl_final" "$T"

(cd "$W/pkg" && python3 - "$T" "$ZSTD" "$W" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import deko3d_shader_package as m
m.ZSTD = sys.argv[2]
sys.argv = ['deko3d_shader_package.py', '--glsl', sys.argv[3] + '/d3/glsl_final', '--out', sys.argv[3] + '/pkg']
m.main()
EOF
)
cp "$W/pkg/deko3d_glsl.bin" "$OUT/"
sed -e "s#\.incbin \\\\\"[^\\\\]*deko3d_glsl.bin\\\\\"#.incbin \\\\\"@PURE_SWITCH_SHADER_DIR@/deko3d_glsl.bin\\\\\"#" \
    "$W/pkg/deko3d_glsl_switch.cpp" > "$OUT/deko3d_glsl_switch.cpp.in"
grep -q "@PURE_SWITCH_SHADER_DIR@/deko3d_glsl.bin" "$OUT/deko3d_glsl_switch.cpp.in"

python3 - "$W/gen/shader_cache.cpp" "$OUT" <<'EOF'
import hashlib, os, re, sys
text = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'const uint8_t g_compressedSpirvCache\[\]\s*=\s*\{(.*?)\};', text, re.S)
blob = b''
open(os.path.join(sys.argv[2], 'spirv_cache.zst'), 'wb').write(blob)
asm = ('// pure-nx: the compressed SPIR-V, embedded by the assembler (sha-256 ' + hashlib.sha256(blob).hexdigest()[:16] +
       ', 0 bytes: the deko3d back end reads deko3d_glsl.bin; dishonored-nx\'s form).\n'
       '__asm__(".section .rodata.banjo_renut_spirv,\\"a\\"\\n"\n'
       '        ".global g_compressedSpirvCache\\n"\n'
       '        ".type g_compressedSpirvCache, %object\\n"\n'
       '        ".balign 16\\n"\n'
       '        "g_compressedSpirvCache:\\n"\n'
       '        ".incbin \\"@PURE_SWITCH_SHADER_DIR@/spirv_cache.zst\\"\\n"\n'
       '        ".size g_compressedSpirvCache, .-g_compressedSpirvCache\\n"\n'
       '        ".previous\\n");')
text = text[:m.start()] + asm + text[m.end():]
text = re.sub(r'(const size_t g_spirvCacheCompressedSize\s*=\s*)\d+', r'\g<1>0', text)
text = re.sub(r'(const size_t g_spirvCacheDecompressedSize\s*=\s*)\d+', r'\g<1>0', text)
open(os.path.join(sys.argv[2], 'shader_cache_switch.cpp.in'), 'w', encoding='utf-8').write(text)
EOF
echo "switch_shaders: $(ls "$W/d3/glsl_final" | wc -l) shaders -> $OUT"
