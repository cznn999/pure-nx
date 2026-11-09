# dishonored-nx (2026-10-31) pass clamp7e3 (constant 913): a 7e3 target's range, as the Xenos clamps it.
import os, re, sys
sys.path.insert(0, sys.argv[3])
import deko3d_glsl_opt as o
CLAMP7E3_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_913\n#define SPIRV_CROSS_CONSTANT_ID_913 0u\n#endif\n'
                 'const uint g_banjo7e3 = SPIRV_CROSS_CONSTANT_ID_913;\n')
TARGET0_RE = re.compile(r'^(\s*)out_var_SV_Target0 = [^;]*;$', re.M)
def pass_clamp7e3(text):
    if 'SPIRV_CROSS_CONSTANT_ID_913' in text or 'out_var_SV_Target0' not in text:
        return text
    span = o.main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    def add(m):
        ind = m.group(1)
        return (m.group(0) + f'\n{ind}if ((g_banjo7e3 & 1u) != 0u)\n{ind}{{\n'
                f'{ind}    vec4 _c7e3 = mix(out_var_SV_Target0, vec4(0.0), isnan(out_var_SV_Target0));\n'
                f'{ind}    out_var_SV_Target0 = vec4(clamp(_c7e3.xyz, vec3(0.0), vec3(31.875)), clamp(_c7e3.w, 0.0, 1.0));\n'
                f'{ind}}}')
    body, n = TARGET0_RE.subn(add, body)
    if not n:
        return text
    return o.insert_globals(text[:a] + body + text[b:], CLAMP7E3_DECL)
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True); n = c = 0
for f in os.listdir(src):
    t = open(os.path.join(src, f), encoding='utf-8').read()
    if f.startswith('ps_'):
        r = pass_clamp7e3(t); c += r != t; t = r
    open(os.path.join(dst, f), 'w', encoding='utf-8', newline='\n').write(t); n += 1
print(n, 'files', c, 'clamped')
