#!/usr/bin/env python3
"""pure-nx (2026-11-08): non_argument_as_local for Pure (renut-nx docs/codegen-enhancements.md, "Non-argument
registers as locals": r0, r2, r11, r12, f0 and v32-v63 as C++ locals; renut-nx measured -4.5 % instructions and
-11.4 % stores in the game code). tools/non_argument_audit.py (renut-nx, from peggle2-nx) finds every function a value
crosses a call into (ENTRY) or out of (RET) in those registers, on code generated WITHOUT the option; those functions
keep the registers in ctx (non_argument_as_local_keep) and their callers hand them over and copy them back.

  --needs-codegen  exit 1 if generated/default was made with the option (then codegen.cmd must run without it first)
  --disable        option off and the keep list out of overrides.toml
  --write          the audit on generated/default (made without the option), then the keep list and the option on
  --verify         the audit on code generated with the option (--with-option): prints its counts
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'generated', 'default')
TOML = os.path.join(ROOT, 'overrides.toml')
AUDIT = os.path.join(ROOT, 'tools', 'non_argument_audit.py')
BEGIN = '# --- non-argument keep list: tools/nonarg_keep_pure.py (do not edit by hand) ---'
END = '# --- end of the non-argument keep list ---'
LOCAL = re.compile(r'^\tPPCRegister r11\{\};$', re.M)


def read():
    return open(TOML, encoding='utf-8').read()


def write(text):
    open(TOML, 'w', encoding='utf-8', newline='\n').write(text)


def strip(text):
    if BEGIN in text:
        a = text.index(BEGIN)
        b = text.index(END) + len(END)
        text = text[:a].rstrip('\n') + '\n' + text[b:].lstrip('\n')
    return re.sub(r'^non_argument_as_local = (true|false)\n', '', text, flags=re.M)


def insert_top(text, block):
    i = text.index('\n[functions]')
    return text[:i].rstrip('\n') + '\n\n' + block + '\n' + text[i:]


def tree_has_option():
    import glob
    for path in glob.glob(os.path.join(GEN, '*.cpp'))[:40]:
        if LOCAL.search(open(path, encoding='utf-8', errors='replace').read()):
            return True
    return False


def audit(with_option):
    args = [sys.executable, AUDIT, GEN] + (['--with-option'] if with_option else [])
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    if r.returncode != 0:
        sys.exit('nonarg_keep_pure: the audit failed')
    return r.stdout.splitlines()


def main():
    if '--needs-codegen' in sys.argv:
        sys.exit(1 if tree_has_option() else 0)
    if '--disable' in sys.argv:
        write(insert_top(strip(read()), 'non_argument_as_local = false'))
        return
    if '--verify' in sys.argv:
        lines = audit(True)
        for ln in lines:
            if not ln.startswith('#'):
                print(ln[:160])
        return
    if '--write' in sys.argv:
        if tree_has_option():
            sys.exit('nonarg_keep_pure: generated/default was made with the option: run --disable and codegen.cmd')
        keep, unsure = set(), []
        for ln in audit(False):
            p = ln.split()
            if len(p) < 2 or p[0] not in ('ENTRY', 'RET'):
                continue
            names = [p[1]]
            if p[0] == 'RET' and '<=' in ln:  # the callees the value may come from: they keep, the caller copies back
                names += re.split(r'[ ,]+', ln.split('<=')[-1].strip())
            for nm in names:
                m = re.fullmatch(r'(?:__imp__)?sub_([0-9A-F]{8})', nm)
                if m:
                    keep.add(int(m.group(1), 16))
                elif nm == 'INDIRECT':
                    unsure.append(ln[:140])
        rows = sorted(keep)
        body = ',\n'.join('    ' + ', '.join(f'0x{a:08X}' for a in rows[i:i + 8]) for i in range(0, len(rows), 8))
        block = (f'{BEGIN}\n# {len(rows)} functions (ENTRY / RET of tools/non_argument_audit.py)\n'
                 f'non_argument_as_local = true\nnon_argument_as_local_keep = [\n{body}\n]\n{END}')
        write(insert_top(strip(read()), block))
        print(f'nonarg_keep_pure: {len(rows)} functions kept in ctx; non_argument_as_local on')
        for u in unsure:
            print('nonarg_keep_pure: WARNING a value returned through an indirect call:', u)
        return
    sys.exit(__doc__)


if __name__ == '__main__':
    main()
