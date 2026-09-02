#!/usr/bin/env python3
"""Dump a canonical, diffable netlist from a KiCad .kicad_pcb.

The board files are gitignored (5 MB), so this produces the small derived
artifact that firmware constants are actually justified against. Re-run it on a
new board revision and diff the output to see exactly what changed.

    tools/kicad_netlist.py hardware/synthseqr_v3.kicad_pcb > hardware/netlist.txt
"""

import hashlib
import sys
from datetime import datetime, timezone


def parse(text):
    """Minimal s-expression reader. Returns nested lists; atoms are str."""
    tokens, i, n = [], 0, len(text)
    stack, cur = [], []
    while i < n:
        c = text[i]
        if c == '(':
            stack.append(cur)
            cur = []
            i += 1
        elif c == ')':
            done = cur
            cur = stack.pop()
            cur.append(done)
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while text[j] != '"':
                if text[j] == '\\':
                    buf.append(text[j + 1])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            cur.append('"' + ''.join(buf))
            i = j + 1
        elif c.isspace():
            i += 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '()"':
                j += 1
            cur.append(text[i:j])
            i = j
    return cur


def head(node):
    return node[0] if node and isinstance(node[0], str) else None


def find(node, key):
    return [c for c in node if isinstance(c, list) and head(c) == key]


def first(node, key):
    got = find(node, key)
    return got[0] if got else None


def unquote(s):
    return s[1:] if isinstance(s, str) and s.startswith('"') else s


def prop(fp, name):
    for p in find(fp, 'property'):
        if len(p) >= 3 and unquote(p[1]) == name:
            return unquote(p[2])
    return None


def main():
    path = sys.argv[1]
    raw = open(path, 'rb').read()
    tree = parse(raw.decode('utf-8'))[0]

    comps = []          # (ref, value, footprint, x, y)
    nets = {}           # net name -> set of "REF.PAD"

    for fp in find(tree, 'footprint'):
        ref = prop(fp, 'Reference') or '?'
        val = prop(fp, 'Value') or ''
        lib = unquote(fp[1]) if len(fp) > 1 and isinstance(fp[1], str) else ''
        at = first(fp, 'at')
        x, y = (float(at[1]), float(at[2])) if at else (0.0, 0.0)
        comps.append((ref, val, lib, x, y))

        for pad in find(fp, 'pad'):
            net = first(pad, 'net')
            if not net:
                continue
            # KiCad 10 drops the net number: (net "NAME")
            name = unquote(net[-1])
            if name.startswith('unconnected-'):
                continue
            nets.setdefault(name, set()).add(f"{ref}.{unquote(pad[1])}")

    def refkey(r):
        pre = ''.join(ch for ch in r if not ch.isdigit())
        num = ''.join(ch for ch in r if ch.isdigit())
        return (pre, int(num) if num else 0)

    digest = hashlib.sha256(raw).hexdigest()[:16]
    print(f"# netlist of {path}")
    print(f"# sha256:{digest}  generated {datetime.now(timezone.utc):%Y-%m-%dT%H:%M:%SZ}")
    print(f"# {len(comps)} components, {len(nets)} nets")

    print("\n## components\n")
    by_prefix = {}
    for ref, val, lib, x, y in comps:
        by_prefix.setdefault(refkey(ref)[0], []).append((ref, val, lib, x, y))
    for pre in sorted(by_prefix):
        rows = sorted(by_prefix[pre], key=lambda r: refkey(r[0]))
        print(f"{pre}: {len(rows)}")
        for ref, val, lib, x, y in rows:
            print(f"  {ref:<8} {val:<24} {x:9.2f} {y:9.2f}  {lib}")

    print("\n## nets\n")
    for name in sorted(nets):
        pads = sorted(nets[name], key=lambda p: refkey(p.split('.')[0]))
        print(f"{name}  ({len(pads)})")
        print(f"    {' '.join(pads)}")


if __name__ == '__main__':
    main()
