#!/usr/bin/env python3
"""pure-nx (2026-11-08): the share_registers marks for non_volatile_as_local ("the diet"), written into overrides.toml.

With non_volatile_as_local the generated functions keep r14-r31, f14-f31, v14-v31 / v64-v127 in C++ locals instead of
ctx, and their __savegprlr / __restgprlr calls go away (nfsmw-nx: 23,857 calls, about 14 % of the main thread;
renut-nx and Mass Effect measured -4.5 % instructions, -11 % stores, -10 % main thread). The one thing that breaks:
a function the generator split in pieces, whose later piece reads a register the first piece set. Those pieces must
be marked share_registers (they keep ctx). tools/read_before_write.py (renut-nx's, flow-sensitive) finds them in code
generated WITH the option and WITHOUT marks; this script runs it and writes its list into overrides.toml:
  - a function already in [functions] (a stub line "0x..." = { }) gets share_registers = true inside its braces;
  - the others go into the block between the two "diet marks" comment lines at the end of [functions] (rewritten on
    every run; the rest of the file is not touched).

Order (build_switch_diet.cmd runs all of it):
  1. overrides.toml has non_volatile_as_local = true; codegen.cmd (no marks yet: run with --clear first if needed)
  2. python tools\\diet_marks_pure.py            -> the marks
  3. codegen.cmd again, python tools\\diet_marks_pure.py --check (must report 0 new)
  4. python tools\\post_codegen_pure.py, then the build.

  python tools/diet_marks_pure.py [--gen generated/default] [--clear] [--check] [--noreturn=ADDR,...]

A function whose guest code starts like a normal entry (mflr r12, stwu r1, or a std/stfd save of r14-r31 / f14-f31
below r1) is never marked, only reported: a split piece does not start with a prologue, and marking a normal function
share_registers corrupts its callers (read_before_write.py's docstring; the first diet build crashed in sub_82C7D6D0
that way, 2026-11-08: 30 such false marks, reads after the D3DX compiler's error call, which longjmps).
"""
import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BEGIN = '# --- diet marks: tools/diet_marks_pure.py (share_registers for non_volatile_as_local); do not edit ---'
END = '# --- end of the diet marks ---'
MARK = 'share_registers = true'
KEY = re.compile(r'^"0x([0-9A-Fa-f]{8})"\s*=\s*\{(.*?)\}(.*)$')


ENTRY = re.compile(r'^\t// (?:mflr r12|stwu r1,|std r(?:1[4-9]|2[0-9]|3[01]),-\d+\(r1\)|'
                   r'stfd f(?:1[4-9]|2[0-9]|3[01]),-\d+\(r1\))')
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{')


def normal_entries(gen, addrs):
    """The addresses in addrs whose generated body starts with a prologue instruction (see the docstring)."""
    found, want = set(), set(addrs)
    for path in glob.glob(os.path.join(gen, '*.cpp')):
        cur = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for ln in f:
                m = DEF.match(ln)
                if m:
                    cur = m.group(1) if m.group(1) in want else None
                    continue
                if cur and ln.startswith('\t// '):
                    if ENTRY.match(ln):
                        found.add(cur)
                    cur = None
    return found


def run_audit(gen, noreturn):
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'read_before_write.py'), gen, '--toml']
    if noreturn:
        cmd.append('--noreturn=' + noreturn)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        sys.exit('read_before_write.py failed')
    sys.stderr.write(''.join(l + '\n' for l in r.stderr.splitlines() if l.startswith(('TOTAL', '# noreturn'))))
    marks = {}
    for line in r.stdout.splitlines():
        m = re.match(r'^"0x([0-9A-F]{8})"\s*=\s*\{ share_registers = true \}\s*#\s*(.*)$', line)
        if m:
            marks[m.group(1).upper()] = m.group(2)
    normal = normal_entries(gen, marks)
    for addr in sorted(normal):
        sys.stderr.write('not marked (normal entry, a false positive of the audit): 0x%s  %s\n' % (addr, marks.pop(addr)))
    if normal:
        sys.stderr.write('%d normal function(s) left unmarked: look at them with read_before_write.py --why=sub_X\n'
                         % len(normal))
    return marks


def strip_marks(lines):
    """The file without the managed block and without the marks this script added inside existing braces."""
    out, skip = [], False
    for ln in lines:
        if ln.rstrip('\n') == BEGIN:
            skip = True
            continue
        if ln.rstrip('\n') == END:
            skip = False
            continue
        if skip:
            continue
        m = KEY.match(ln.rstrip('\n'))
        if m and 'diet mark' in m.group(3):
            inner = re.sub(r',?\s*share_registers = true\s*', ' ', m.group(2)).strip()
            rest = re.sub(r'\s*\(diet mark\)', '', m.group(3))
            ln = '"0x%s" = { %s}%s\n' % (m.group(1), inner + ' ' if inner else '', rest)
        out.append(ln)
    while out and out[-1].strip() == '':
        out.pop()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'generated', 'default'))
    ap.add_argument('--overrides', default=os.path.join(ROOT, 'overrides.toml'))
    ap.add_argument('--clear', action='store_true', help='remove the marks (before a codegen to audit afresh)')
    ap.add_argument('--check', action='store_true', help='only report functions that would need a new mark')
    ap.add_argument('--noreturn', default='', help='guest functions that never return (longjmp ...), comma-separated')
    a = ap.parse_args()

    text = open(a.overrides, encoding='utf-8').read()
    if 'non_volatile_as_local = true' not in text and not a.clear:
        sys.exit('overrides.toml has no "non_volatile_as_local = true": nothing to mark')
    lines = strip_marks(open(a.overrides, encoding='utf-8').readlines())
    existing = {m.group(1).upper(): i for i, ln in enumerate(lines) for m in [KEY.match(ln.rstrip('\n'))] if m}
    if a.clear:
        open(a.overrides, 'w', encoding='utf-8').write(''.join(lines) + '\n')
        print('diet marks removed from', a.overrides)
        return

    marks = run_audit(a.gen, a.noreturn)
    if a.check:
        print('%d function(s) read a non-volatile register before writing it (0 expected after the marks)' % len(marks))
        for k, v in sorted(marks.items()):
            print('  0x%s  %s' % (k, v))
        sys.exit(1 if marks else 0)

    added_inline, block = 0, []
    for addr, regs in sorted(marks.items()):
        if addr in existing:
            i = existing[addr]
            m = KEY.match(lines[i].rstrip('\n'))
            inner = m.group(2).strip()
            if MARK not in inner:
                inner = (inner + ', ' if inner else '') + MARK
            lines[i] = '"0x%s" = { %s }%s (diet mark)\n' % (m.group(1), inner, m.group(3) if m.group(3) else '  #')
            added_inline += 1
        else:
            block.append('"0x%s" = { %s }  # %s\n' % (addr, MARK, regs))
    out = lines + ['\n', BEGIN + '\n'] + block + [END + '\n']
    open(a.overrides, 'w', encoding='utf-8').write(''.join(out))
    print('%d share_registers mark(s): %d in existing entries, %d in the diet block of %s' %
          (len(marks), added_inline, len(block), a.overrides))
    print('next: codegen.cmd, then python tools\\diet_marks_pure.py --check (0 expected)')


if __name__ == '__main__':
    main()
