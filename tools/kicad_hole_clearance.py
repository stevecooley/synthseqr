#!/usr/bin/env python3
"""Report the copper clearance around every drilled hole in a KiCad .kicad_pcb.

Written to answer one question: does a pour reach the unplated mounting holes of
the slide pots, where a metal frame lug can touch it? Two things make that easy
to miss. A board carried forward from KiCad 5 often has no hole-clearance
constraint set, so DRC never checks copper against an NPTH hole. And a mounting
hole drawn as a graphic circle rather than as a pad is invisible to DRC
entirely — this reports those too, from Edge.Cuts circles.

The board files are gitignored, so run this against your local copy:

    tools/kicad_hole_clearance.py hardware/synthseqr_v3.kicad_pcb RV
    tools/kicad_hole_clearance.py hardware/synthseqr_v3.kicad_pcb        # whole board
    tools/kicad_hole_clearance.py --selftest

Negative clearance means copper reaches into the hole. Pad copper is
approximated by its bounding circle and arcs by their start-mid-end polyline, so
a flagged hole is worth looking at in pcbnew, not worth panicking over.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kicad_netlist import parse, find, first, unquote      # noqa: E402

FLAG_MM = 0.25          # anything tighter than this gets called out


# ---------------------------------------------------------------- geometry
def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    L = vx * vx + vy * vy
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / L))
    return math.hypot(ax + t * vx - px, ay + t * vy - py)


def in_poly(px, py, pts):
    inside = False
    n = len(pts)
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        if (ay > py) != (by > py):
            x = ax + (py - ay) * (bx - ax) / (by - ay)
            if x > px:
                inside = not inside
    return inside


def poly_dist(px, py, pts):
    """Signed: negative when the point is inside the polygon."""
    d = min(seg_dist(px, py, pts[i][0], pts[i][1],
                     pts[(i + 1) % len(pts)][0], pts[(i + 1) % len(pts)][1])
            for i in range(len(pts)))
    return -d if in_poly(px, py, pts) else d


def fp_rotate(x, y, deg):
    """Footprint-local to board offset. KiCad rotates counter-clockwise on a
    screen whose Y runs down, which on the stored coordinates is clockwise."""
    a = math.radians(deg)
    return x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a)


# ---------------------------------------------------------------- extraction
def xy_list(node):
    pts = first(node, 'pts')
    return [(float(p[1]), float(p[2])) for p in find(pts, 'xy')] if pts else []


def at_of(node):
    a = first(node, 'at')
    if not a:
        return 0.0, 0.0, 0.0
    return float(a[1]), float(a[2]), (float(a[3]) if len(a) > 3 else 0.0)


def layers_of(node, key='layers'):
    got = first(node, key)
    return [unquote(v) for v in got[1:]] if got else []


def cu_layers(names):
    """Copper layers only, with the shorthands expanded. On a 4-layer board
    '*.Cu' also covers the inner layers; only the outer two are enumerated,
    which is what matters for copper a mounting lug can touch."""
    out = []
    for n in names:
        if n in ('*.Cu', 'F&B.Cu'):
            out += ['F.Cu', 'B.Cu']
        elif n.endswith('.Cu'):
            out.append(n)
    return out


def copper_features(tree):
    """[(kind, layer, net, shape, owner)]; shape is ('poly', pts), ('poly-open',
    pts, halfwidth) or ('circ', x, y, r). owner identifies a pad's own copper so
    a plated hole is not reported as being bridged by its own annular ring."""
    numbered = {}
    for n in find(tree, 'net'):
        if len(n) >= 3:
            numbered[n[1]] = unquote(n[2])

    def netof(node):
        n = first(node, 'net')
        return numbered.get(n[1], '?') if n and len(n) > 1 else ''

    feats = []
    unfilled = 0
    for z in find(tree, 'zone'):
        nn = first(z, 'net_name')
        net = unquote(nn[1]) if nn and len(nn) > 1 else netof(z)
        fills = find(z, 'filled_polygon')
        if fills:
            for f in fills:
                lay = first(f, 'layer')
                feats.append(('zone', unquote(lay[1]) if lay else '?', net,
                              ('poly', xy_list(f)), None))
        else:
            unfilled += 1
            for lay in layers_of(z, 'layers') or layers_of(z, 'layer'):
                pts = xy_list(first(z, 'polygon') or [])
                if pts:
                    feats.append(('zone-outline', lay, net, ('poly', pts), None))

    for s in find(tree, 'segment'):
        a, b = first(s, 'start'), first(s, 'end')
        w = float(first(s, 'width')[1])
        lay = unquote(first(s, 'layer')[1])
        feats.append(('track', lay, netof(s),
                      ('poly-open', [(float(a[1]), float(a[2])),
                                     (float(b[1]), float(b[2]))], w / 2), None))

    for s in find(tree, 'arc'):
        a, m, b = first(s, 'start'), first(s, 'mid'), first(s, 'end')
        if not (a and m and b):
            continue
        w = float(first(s, 'width')[1])
        lay = unquote(first(s, 'layer')[1])
        feats.append(('arc~', lay, netof(s),
                      ('poly-open', [(float(a[1]), float(a[2])),
                                     (float(m[1]), float(m[2])),
                                     (float(b[1]), float(b[2]))], w / 2), None))

    for v in find(tree, 'via'):
        x, y, _ = at_of(v)
        r = float(first(v, 'size')[1]) / 2
        for lay in cu_layers(layers_of(v)):
            feats.append(('via', lay, netof(v), ('circ', x, y, r), None))

    for fp in find(tree, 'footprint'):
        fx, fy, frot = at_of(fp)
        ref = fp_ref(fp)[0]
        for pad in find(fp, 'pad'):
            kind = unquote(pad[2]) if len(pad) > 2 else ''
            if kind == 'np_thru_hole':
                continue                       # a hole, not copper
            px, py, _ = at_of(pad)
            dx, dy = fp_rotate(px, py, frot)
            size = first(pad, 'size')
            sx, sy = (float(size[1]), float(size[2])) if size else (0.0, 0.0)
            for lay in cu_layers(layers_of(pad)):
                feats.append(('pad~', lay, netof(pad),
                              ('circ', fx + dx, fy + dy, math.hypot(sx, sy) / 2),
                              (ref, id(pad))))
    return feats, unfilled


def edge_circle(node):
    """(x, y, r) for a circle drawn on Edge.Cuts, else None."""
    lay = first(node, 'layer')
    if not lay or unquote(lay[1]) != 'Edge.Cuts':
        return None
    ctr, end = first(node, 'center'), first(node, 'end')
    if not ctr or not end:
        return None
    cx, cy = float(ctr[1]), float(ctr[2])
    return cx, cy, math.hypot(float(end[1]) - cx, float(end[2]) - cy)


def fp_ref(fp):
    ref = val = '?'
    for p in find(fp, 'property'):
        if len(p) >= 3 and unquote(p[1]) == 'Reference':
            ref = unquote(p[2])
        elif len(p) >= 3 and unquote(p[1]) == 'Value':
            val = unquote(p[2])
    return ref, val


def holes(tree):
    """[(ref, value, rot, label, x, y, r, plated, owner)] per drilled thing."""
    out = []
    for fp in find(tree, 'footprint'):
        ref, val = fp_ref(fp)
        fx, fy, frot = at_of(fp)

        for pad in find(fp, 'pad'):
            drill = first(pad, 'drill')
            if not drill:
                continue
            nums = [t for t in drill[1:] if isinstance(t, str) and
                    t.replace('.', '', 1).isdigit()]
            if not nums:
                continue
            d = float(nums[0])
            px, py, _ = at_of(pad)
            dx, dy = fp_rotate(px, py, frot)
            kind = unquote(pad[2]) if len(pad) > 2 else ''
            out.append((ref, val, frot, 'pad ' + (unquote(pad[1]) or '""'),
                        fx + dx, fy + dy, d / 2, kind != 'np_thru_hole',
                        (ref, id(pad))))

        # A mounting hole milled into the board instead of drawn as a pad.
        # DRC never sees these as holes, which is exactly why they are here.
        for c in find(fp, 'fp_circle'):
            got = edge_circle(c)
            if not got:
                continue
            cx, cy, r = got
            dx, dy = fp_rotate(cx, cy, frot)
            out.append((ref, val, frot, 'Edge.Cuts circle',
                        fx + dx, fy + dy, r, False, None))

    for c in find(tree, 'gr_circle'):
        got = edge_circle(c)
        if got:
            out.append(('(board)', 'Edge.Cuts circle', 0.0, 'gr_circle',
                        got[0], got[1], got[2], False, None))
    return out


def clearance(hx, hy, hr, shape):
    kind = shape[0]
    if kind == 'poly':
        return poly_dist(hx, hy, shape[1]) - hr
    if kind == 'poly-open':
        pts, half = shape[1], shape[2]
        d = min(seg_dist(hx, hy, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
                for i in range(len(pts) - 1))
        return d - half - hr
    _, x, y, r = shape
    return math.hypot(x - hx, y - hy) - r - hr


# ---------------------------------------------------------------- report
def report(path, prefixes):
    tree = parse(open(path, encoding='utf-8').read())[0]
    feats, unfilled = copper_features(tree)
    if unfilled:
        print(f"! {unfilled} zone(s) have no fill in the file — outlines used "
              f"instead. Fill zones in pcbnew and re-save for real numbers.\n")

    hs = holes(tree)
    if prefixes:
        hs = [h for h in hs if any(h[0].startswith(p) for p in prefixes)]
    if not hs:
        print("no holes matched")
        return 0

    print(f"{len(hs)} hole(s), {len(feats)} copper features, flagging under "
          f"{FLAG_MM}mm\n")
    worst = []
    for ref, val, rot, label, x, y, r, plated, owner in sorted(hs):
        near = sorted(((clearance(x, y, r, sh), kind, lay, net)
                       for kind, lay, net, sh, own in feats
                       if own is None or own != owner))[:4]
        tag = 'plated' if plated else 'NPTH  '
        print(f"{ref:<8} {val:<28} rot {rot:>6.1f}  {label}")
        print(f"    {tag} d{r * 2:.2f}mm at ({x:.2f}, {y:.2f})")
        for c, kind, lay, net in near:
            mark = '  <<<' if c < FLAG_MM else ''
            where = 'COPPER IN HOLE' if c < 0 else f"{c:6.3f}mm"
            print(f"      {where}  {kind:<13} {lay:<8} {net or '(no net)'}{mark}")
        print()
        if near and near[0][0] < FLAG_MM:
            worst.append((near[0][0], ref, label, near[0][3], near[0][2]))

    if worst:
        print("--- tight ---")
        for c, ref, label, net, lay in sorted(worst):
            print(f"  {c:7.3f}mm  {ref:<8} {label:<18} to {net} on {lay}")
    else:
        print(f"--- nothing under {FLAG_MM}mm ---")
    return 1 if worst else 0


# ---------------------------------------------------------------- self-test
SELFTEST = """
(kicad_pcb (version 20240108)
  (net 0 "")
  (net 1 "LED_5V")
  (net 2 "GND")
  (footprint "lib:PS45" (at 100 100 0)
    (property "Reference" "RV1") (property "Value" "10k slide")
    (pad "1" thru_hole circle (at 0 10) (size 1.6 1.6) (drill 0.9)
      (layers "F&B.Cu") (net 2 "GND"))
    (pad "" np_thru_hole circle (at -5 0) (size 3 3) (drill 3) (layers "*.Cu"))
    (pad "" np_thru_hole circle (at 5 0) (size 3 3) (drill 3) (layers "*.Cu"))
    (fp_circle (center 0 -8) (end 0 -6.4) (layer "Edge.Cuts")))
  (gr_circle (center 200 200) (end 201.5 200) (layer "Edge.Cuts"))
  (zone (net 1 "LED_5V") (net_name "LED_5V") (layers "F.Cu")
    (filled_polygon (layer "F.Cu")
      (pts (xy 90 98) (xy 92 98) (xy 92 102) (xy 90 102))))
  (segment (start 108 100) (end 120 100) (width 0.5) (layer "B.Cu") (net 2))
)
"""


def selftest():
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.kicad_pcb', delete=False) as f:
        f.write(SELFTEST)
        path = f.name
    tree = parse(SELFTEST)[0]
    feats, _ = copper_features(tree)
    hs = holes(tree)
    got = {(h[3], round(h[4], 2), round(h[5], 2), round(h[6], 3), h[7])
           for h in hs}
    want = {('pad 1', 100.0, 110.0, 0.45, True),
            ('pad ""', 95.0, 100.0, 1.5, False),
            ('pad ""', 105.0, 100.0, 1.5, False),
            ('Edge.Cuts circle', 100.0, 92.0, 1.6, False),    # milled, in a footprint
            ('gr_circle', 200.0, 200.0, 1.5, False)}          # milled, board level
    assert got == want, f"holes: {got}"

    # left NPTH hole is centred at x=95, the LED_5V fill ends at x=92
    left = [h for h in hs if round(h[4], 2) == 95.0][0]
    zone = [f for f in feats if f[0] == 'zone'][0]
    c = clearance(left[4], left[5], left[6], zone[3])
    assert abs(c - 1.5) < 1e-9, c                       # 3.0 gap - 1.5 radius

    # right NPTH hole at x=105 is 13mm from that fill
    right = [h for h in hs if round(h[4], 2) == 105.0][0]
    assert abs(clearance(right[4], right[5], right[6], zone[3]) - 11.5) < 1e-9

    # widen the fill until it swallows the left hole: clearance goes negative
    over = ('poly', [(90, 98), (96, 98), (96, 102), (90, 102)])
    assert clearance(left[4], left[5], left[6], over) == -2.5

    # track on B.Cu: 3mm from the right hole centre, less half width and radius
    track = [f for f in feats if f[0] == 'track'][0]
    assert abs(clearance(right[4], right[5], right[6], track[3]) - 1.25) < 1e-9

    # a pad is copper, an NPTH hole is not
    assert sorted(f[0] for f in feats) == ['pad~', 'pad~', 'track', 'zone']
    assert abs(fp_rotate(1, 0, 90)[1] + 1) < 1e-9       # +x rotates to -y
    print("selftest ok")
    os.unlink(path)


def main():
    args = [a for a in sys.argv[1:] if a != '--selftest']
    if '--selftest' in sys.argv[1:]:
        selftest()
        return 0
    if not args:
        print(__doc__)
        return 2
    return report(args[0], args[1:])


if __name__ == '__main__':
    sys.exit(main())
