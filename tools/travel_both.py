"""travel_both.py PREFIX [PREFIX ...] [--maps=...] [--last=42]: tiles moved per hero-day and the share of hero-days that end
on the starting tile, for our heroes (red) and Nullkiller's (blue), from the HEROES lines (dawn-to-dawn Chebyshev
displacement, same level only, days 1..last). October 5th: M1 gate measures 1 and 2 beside Nullkiller's own. Read-only."""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
last = int(opts.get("last", "42"))
seed = opts.get("seed", "s5k")

HX = re.compile(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?")

for pre in args:
    tot = {"red": [0, 0, 0], "blue": [0, 0, 0]}   # tiles, hero-days, still
    rows = []
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        per = {"red": [0, 0, 0], "blue": [0, 0, 0]}
        prev = None
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.search(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", line)
            if not mm:
                continue
            d = int(mm.group(1))
            pos = {side: {h.group(1): (int(h.group(2)), int(h.group(3)), int(h.group(4))) for h in HX.finditer(txt)}
                   for side, txt in (("red", mm.group(2)), ("blue", mm.group(3)))}
            if prev and prev[0] == d - 1 and d <= last:
                for side in ("red", "blue"):
                    for hid, p in pos[side].items():
                        q = prev[1][side].get(hid)
                        if q and q[2] == p[2]:
                            dist = max(abs(p[0] - q[0]), abs(p[1] - q[1]))
                            per[side][0] += dist
                            per[side][1] += 1
                            per[side][2] += dist == 0
            prev = (d, pos)
        for side in ("red", "blue"):
            for k in range(3):
                tot[side][k] += per[side][k]
        rows.append("  %s  ours %4.1f tiles %3.0f%% still | NK %4.1f tiles %3.0f%% still" % (
            m, per["red"][0] / max(1, per["red"][1]), 100.0 * per["red"][2] / max(1, per["red"][1]),
            per["blue"][0] / max(1, per["blue"][1]), 100.0 * per["blue"][2] / max(1, per["blue"][1])))
    print("== %s (days 1-%d)" % (pre, last))
    print("\n".join(rows))
    print("  all ours %.1f tiles per hero-day, %.0f%% on the starting tile | Nullkiller %.1f, %.0f%%" % (
        tot["red"][0] / max(1, tot["red"][1]), 100.0 * tot["red"][2] / max(1, tot["red"][1]),
        tot["blue"][0] / max(1, tot["blue"][1]), 100.0 * tot["blue"][2] / max(1, tot["blue"][1])))
