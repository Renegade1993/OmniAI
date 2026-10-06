"""m1_gate.py [PREFIX ...] [--maps=...] [--seed=s6k]: the four M1 gate measures of PREREG-V2M1.md pooled over every game of the
given prefixes (default V2M1G V2M1G2 V2M1G3), with the reported-only measures beside them. October 5th. Read-only.

1. tiles moved per hero-day, ours, days 1-42 (dawn-to-dawn Chebyshev displacement, same level), Nullkiller's beside it
2. share of hero-days ending on the starting tile, ours, days 1-42
3. mines on day 14, ours over Nullkiller's, median over games
4. neutral towns taken by day 56 (neutral on the first TOWNS line, ours or Nullkiller's on the last TOWNS line at or
   before day 56), ours over Nullkiller's, summed over games
Reported: army share of Nullkiller's (median) on days 7, 14, 21, 28, 42, 56; wins, losses, undecided; fights won and lost."""
import io
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")] or ["V2M1G", "V2M1G2", "V2M1G3"]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s6k")
DAYS = (7, 14, 21, 28, 42, 56)
HX = re.compile(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?")

trav = {"red": [0, 0, 0], "blue": [0, 0, 0]}
mine_ratio = []
taken = {"red": 0, "blue": 0}
share = {d: [] for d in DAYS}
ends = {"won": 0, "lost": 0, "undecided": 0, "missing": 0}
fw = fl = games = 0
rows = []
for pre in args:
    for m in maps:
        tag = "%s%s" % (pre, m)
        f = os.path.join(HERE, "runs", "%s_%s_r01_decisions-red.log" % (tag, seed))
        out = os.path.join(HERE, "runs", tag + ".out")
        if not os.path.exists(f):
            ends["missing"] += 1
            continue
        games += 1
        t = io.open(f, encoding="utf-8", errors="replace").read()
        end = ""
        if os.path.exists(out):
            mm = re.search(r"run 1 \(red\): day (\d+)\s+(winner \S+|undecided|ERROR.*)", io.open(out, encoding="utf-8", errors="replace").read())
            if mm:
                end = "%s %s" % (mm.group(1), mm.group(2))
                if "winner" in mm.group(2):
                    ends["won" if "OmniAI" in mm.group(2) else "lost"] += 1
                else:
                    ends["undecided"] += 1
        fw += len(re.findall(r"FIGHT won", t))
        fl += len(re.findall(r"FIGHT LOST", t))
        # 1, 2
        prev = None
        for mm in re.finditer(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", t):
            d = int(mm.group(1))
            pos = {s: {h.group(1): (int(h.group(2)), int(h.group(3)), int(h.group(4))) for h in HX.finditer(x)}
                   for s, x in (("red", mm.group(2)), ("blue", mm.group(3)))}
            if prev and prev[0] == d - 1 and d <= 42:
                for s in ("red", "blue"):
                    for hid, p in pos[s].items():
                        q = prev[1][s].get(hid)
                        if q and q[2] == p[2]:
                            dist = max(abs(p[0] - q[0]), abs(p[1] - q[1]))
                            trav[s][0] += dist
                            trav[s][1] += 1
                            trav[s][2] += dist == 0
            prev = (d, pos)
        # 3 and the shares
        for d in DAYS:
            o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
            b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
            if not (o and b):
                continue
            if d == 14:
                mo = int(re.search(r"mines=(\d+)", o.group(0)).group(1))
                mb = int(re.search(r"mines=(\d+)", b.group(0)).group(1))
                if mb:
                    mine_ratio.append(mo / mb)
            if army(b.group(0)):
                share[d].append(army(o.group(0)) / army(b.group(0)))
        # 4
        tl = [(int(mm.group(1)), mm.group(2)) for mm in re.finditer(r"TOWNS d(\d+) ([^\n]*)", t)]
        g = {"red": 0, "blue": 0}
        if tl:
            first = {x.split(":")[0]: x.split(":")[1] for x in tl[0][1].split() if x.count(":") >= 2}
            lastl = [x for x in tl if x[0] <= 56]
            last = {x.split(":")[0]: x.split(":")[1] for x in (lastl[-1][1] if lastl else tl[0][1]).split() if x.count(":") >= 2}
            for k, own in first.items():
                if own == "neutral" and last.get(k) in ("red", "blue"):
                    g[last[k]] += 1
        taken["red"] += g["red"]
        taken["blue"] += g["blue"]
        rows.append("  %-9s %-24s towns taken %d/%d" % (tag, end, g["red"], g["blue"]))


def med(v):
    return statistics.median(v) if v else float("nan")


print("== M1 gate, %d games (%s), seed %s" % (games, " ".join(args), seed))
print("\n".join(rows))
tp = trav["red"][0] / max(1, trav["red"][1])
st = 100.0 * trav["red"][2] / max(1, trav["red"][1])
mr = med(mine_ratio)
tr = taken["red"] / max(1, taken["blue"])
print("  1. tiles per hero-day %.1f (bar at least 10; Nullkiller %.1f)  %s" % (tp, trav["blue"][0] / max(1, trav["blue"][1]), "MET" if tp >= 10 else "MISSED"))
print("  2. hero-days on the starting tile %.1f%% (bar under 5%%; Nullkiller %.1f%%)  %s" % (st, 100.0 * trav["blue"][2] / max(1, trav["blue"][1]), "MET" if st < 5 else "MISSED"))
print("  3. mines day 14, ours over Nullkiller's, median %.2f over %d games (bar at least 0.85)  %s" % (mr, len(mine_ratio), "MET" if mr >= 0.85 else "MISSED"))
print("  4. neutral towns taken by day 56: ours %d, Nullkiller %d, ratio %.2f (bar at least 0.6)  %s" % (taken["red"], taken["blue"], tr, "MET" if tr >= 0.6 else "MISSED"))
print("  army share of Nullkiller's, median: " + "  ".join("d%d %.2f (n%d)" % (d, med(share[d]), len(share[d])) for d in DAYS))
print("  games: won %d, lost %d, undecided %d, missing %d | fights won %d, lost %d" % (ends["won"], ends["lost"], ends["undecided"], ends["missing"], fw, fl))
