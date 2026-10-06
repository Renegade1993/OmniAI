"""v2_sum.py PREFIX [PREFIX ...] [--maps=01,02,03,04,05,07,10,13,16,19] [--days=7,14,21,28,42]: medians over the maps of
one game per map, each prefix against Nullkiller. October 4th (megaplan option B, development smokes).

Per day: army share of Nullkiller's, mines ours/NK, towns ours/NK, buildings ours/NK, best level ours/NK (medians over the
maps that reached the day); then fights won and lost, our heroes killed (FIGHT LOST lines), and how the games ended, and the
M1 count gates' measures: tiles per hero-day and the share of hero-days that end on the starting tile (days 1-42). Read-only.
"""
import io
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
days = [int(d) for d in opts.get("days", "7,14,21,28,42").split(",")]


def num(rx, s):
    m = re.search(rx, s)
    return int(m.group(1)) if m else None


def med(v):
    return statistics.median(v) if v else float("nan")


for pre in args:
    per = {d: {"share": [], "m": [], "mk": [], "t": [], "tk": [], "b": [], "bk": [], "l": [], "lk": [], "conc": [], "gold": []} for d in days}
    won = lost = games = 0
    ends = []
    moved = still = tiles = 0
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, opts.get("seed", "s5k")))
        if not os.path.exists(f):
            continue
        games += 1
        t = io.open(f, encoding="utf-8", errors="replace").read()
        for d in days:
            o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
            b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
            if not (o and b) or not army(b.group(0)):
                continue
            a, c = o.group(0), b.group(0)
            per[d]["share"].append(army(a) / army(c))
            bm = re.search(r"best=[^:]+:(\d+)@", a)
            if bm and army(a):
                per[d]["conc"].append(int(bm.group(1)) / army(a))
            per[d]["gold"].append(num(r"gold=(\d+)", a) or 0)
            for key, rx in (("m", r"mines=(\d+)"), ("t", r"towns=(\d+)"), ("b", r"bld=(\d+)"), ("l", r"maxL=(\d+)")):
                per[d][key].append(num(rx, a) or 0)
                per[d][key + "k"].append(num(rx, c) or 0)
        won += len(re.findall(r"FIGHT won", t))
        lost += len(re.findall(r"FIGHT LOST", t))
        out = os.path.join(HERE, "runs", "%s%s.out" % (pre, m))
        if os.path.exists(out):
            mm = re.search(r"run 1 \(red\): day (\d+)\s+(\S+)", io.open(out, encoding="utf-8", errors="replace").read())
            if mm:
                ends.append("%s:%s" % (mm.group(2)[0], mm.group(1)))
        # hero travel from the HEROES lines, days 1-42
        prev = None
        for mm in re.finditer(r"HEROES d(\d+) red (.*?) \|", t):
            d = int(mm.group(1))
            pos = {h.group(1): (int(h.group(2)), int(h.group(3)), int(h.group(4)))
                   for h in re.finditer(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?", mm.group(2))}
            if prev and prev[0] == d - 1 and d <= 42:
                for hid, p in pos.items():
                    q = prev[1].get(hid)
                    if q and q[2] == p[2]:
                        dist = max(abs(p[0] - q[0]), abs(p[1] - q[1]))
                        tiles += dist
                        moved += 1
                        if dist == 0:
                            still += 1
            prev = (d, pos)
    print("== %s (%d games)" % (pre, games))
    for d in days:
        p = per[d]
        if not p["share"]:
            continue
        print("  d%-2d n%-2d share %.2f  mines %.1f/%.1f  towns %.1f/%.1f  bld %.1f/%.1f  level %.1f/%.1f  main %.0f%%  gold %.0f" % (
            d, len(p["share"]), med(p["share"]), med(p["m"]), med(p["mk"]), med(p["t"]), med(p["tk"]),
            med(p["b"]), med(p["bk"]), med(p["l"]), med(p["lk"]), 100 * med(p["conc"]), med(p["gold"])))
    print("  fights won %d lost %d | ends %s | tiles per hero-day %.1f, hero-days on the starting tile %.0f%%" % (
        won, lost, " ".join(ends), tiles / max(1, moved), 100.0 * still / max(1, moved)))
