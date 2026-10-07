"""disp_hist.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k] [--last=42]: how the tiles-per-hero-day count of the M1 gate
is made up, ours (red) against Nullkiller's (blue). October 7th (the tiles bar: where do Nullkiller's extra tiles come from).
Read-only.

Per hero-day (dawn to next dawn, same level, days 1 to --last, the m1_gate.py measure): the Chebyshev displacement, bucketed;
for each bucket the share of hero-days and the share of all tiles. Also per side: hero-days, tiles per hero-day, and the
mean over hero-days that moved at most 20 tiles (walking days) and over the rest (long days: roads, monoliths, ships)."""
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s5k")
last = int(opts.get("last", "42"))
HX = re.compile(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?")
B = [(0, 0), (1, 4), (5, 8), (9, 12), (13, 16), (17, 20), (21, 30), (31, 999)]

d = {"red": [], "blue": []}
for pre in args:
    for m in maps:
        logs = sorted(glob.glob(os.path.join(HERE, "runs", "%s%s_%s_r0[12]_decisions-*.log" % (pre, m, seed))))
        if not logs:
            continue
        f = logs[0]
        # "red" below means ours and "blue" Nullkiller's, whichever seat OmniAI had (m1_gate.py's rule).
        flip = f.endswith("-blue.log")
        t = io.open(f, encoding="utf-8", errors="replace").read()
        if not re.search(r"TRUTH d7 ", t):
            continue
        prev = None
        for mm in re.finditer(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", t):
            day = int(mm.group(1))
            pos = {s: {h.group(1): (int(h.group(2)), int(h.group(3)), int(h.group(4))) for h in HX.finditer(x)}
                   for s, x in (("blue" if flip else "red", mm.group(2)), ("red" if flip else "blue", mm.group(3)))}
            if prev and prev[0] == day - 1 and day <= last:
                for s in ("red", "blue"):
                    for hid, p in pos[s].items():
                        q = prev[1][s].get(hid)
                        if q and q[2] == p[2]:
                            d[s].append(max(abs(p[0] - q[0]), abs(p[1] - q[1])))
            prev = (day, pos)

print("== %s (days 1-%d, seed %s)" % (" ".join(args), last, seed))
for s, name in (("red", "ours"), ("blue", "Nullkiller")):
    v = d[s]
    if not v:
        continue
    tot = float(sum(v))
    walk = [x for x in v if x <= 20]
    long_ = [x for x in v if x > 20]
    print("  %-10s %5d hero-days, %.2f tiles a day; walking days (<=20) %.2f over %d; long days %.1f over %d (%.1f%% of days, %.1f%% of tiles)"
          % (name, len(v), tot / len(v), sum(walk) / max(1, len(walk)), len(walk), sum(long_) / max(1, len(long_)), len(long_),
             100.0 * len(long_) / len(v), 100.0 * sum(long_) / max(1, tot)))
    print("     " + "  ".join("%s: %4.1f%% days %4.1f%% tiles" % ("%d-%d" % b if b[1] < 999 else "%d+" % b[0],
                                                               100.0 * sum(b[0] <= x <= b[1] for x in v) / len(v),
                                                               100.0 * sum(x for x in v if b[0] <= x <= b[1]) / max(1, tot)) for b in B))
