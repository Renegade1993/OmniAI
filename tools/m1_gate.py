"""m1_gate.py [PREFIX ...] [--maps=...] [--seed=s6k]: the four M1 gate measures of PREREG-V2M1.md pooled over every game of the
given prefixes (default V2M1G V2M1G2 V2M1G3), with the reported-only measures beside them. October 5th. Read-only.

1. tiles moved per hero-day, ours, days 1-42 (dawn-to-dawn Chebyshev displacement, same level), Nullkiller's beside it
2. share of hero-days ending on the starting tile, ours, days 1-42
3. mines on day 14, ours over Nullkiller's, median over games
4. neutral towns taken by day 56 (neutral on the first TOWNS line, ours or Nullkiller's on the last TOWNS line at or
   before day 56), ours over Nullkiller's, summed over games
Reported: army share of Nullkiller's (median) on days 7, 14, 21, 28, 42, 56; wins, losses, undecided; fights won and lost.
October 7th: a game lost before a day counts as share 0 on that day and later days, a game won before it as share 10, so a
loss weighs against its build (the Opus Reviewer's correction of October 6th). A void (no TRUTH line on day 7: the map never
started) is counted and left out of every measure. Either seat: a game's log is <tag>_r01_decisions-red.log (OmniAI red) or
<tag>_r02_decisions-blue.log (OmniAI blue, queue_v2smoke.py seat blue); ours is the log's colour."""
import glob
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

trav = {"ours": [0, 0, 0], "nk": [0, 0, 0]}
mine_ratio = []
taken = {"ours": 0, "nk": 0}
share = {d: [] for d in DAYS}
ends = {"won": 0, "lost": 0, "undecided": 0, "missing": 0, "void": 0}
seats = {"red": 0, "blue": 0}
fw = fl = games = 0
rows = []
for pre in args:
    for m in maps:
        tag = "%s%s" % (pre, m)
        logs = sorted(glob.glob(os.path.join(HERE, "runs", "%s_%s_r0[12]_decisions-*.log" % (tag, seed))))
        out = os.path.join(HERE, "runs", tag + ".out")
        if not logs:
            ends["missing"] += 1
            continue
        f = logs[0]
        me = "blue" if f.endswith("-blue.log") else "red"
        side = {me: "ours", ("red" if me == "blue" else "blue"): "nk"}
        t = io.open(f, encoding="utf-8", errors="replace").read()
        if not re.search(r"TRUTH d7 ", t):
            ends["void"] += 1
            rows.append("  %-9s void (no TRUTH line on day 7)" % tag)
            continue
        games += 1
        seats[me] += 1
        end, result = "", ""
        if os.path.exists(out):
            mm = re.search(r"run \d+ \((?:red|blue)\): day (\d+)\s+(winner \S+|undecided|ERROR.*)", io.open(out, encoding="utf-8", errors="replace").read())
            if mm:
                end = "%s %s" % (mm.group(1), mm.group(2))
                if "winner" in mm.group(2):
                    result = "won" if "OmniAI" in mm.group(2) else "lost"
                    ends[result] += 1
                else:
                    ends["undecided"] += 1
        fw += len(re.findall(r"FIGHT won", t))
        fl += len(re.findall(r"FIGHT LOST", t))
        # 1, 2
        prev = None
        for mm in re.finditer(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", t):
            d = int(mm.group(1))
            pos = {side[s]: {h.group(1): (int(h.group(2)), int(h.group(3)), int(h.group(4))) for h in HX.finditer(x)}
                   for s, x in (("red", mm.group(2)), ("blue", mm.group(3)))}
            if prev and prev[0] == d - 1 and d <= 42:
                for s in ("ours", "nk"):
                    for hid, p in pos[s].items():
                        q = prev[1][s].get(hid)
                        if q and q[2] == p[2]:
                            dist = max(abs(p[0] - q[0]), abs(p[1] - q[1]))
                            trav[s][0] += dist
                            trav[s][1] += 1
                            trav[s][2] += dist == 0
            prev = (d, pos)
        # 3 and the shares
        lastday = max([int(x) for x in re.findall(r"^DAY (\d+)", t, re.M)] or [0])
        for d in DAYS:
            if result and lastday < d:
                share[d].append(0.0 if result == "lost" else 10.0)
                continue
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
        g = {"ours": 0, "nk": 0}
        if tl:
            first = {x.split(":")[0]: x.split(":")[1] for x in tl[0][1].split() if x.count(":") >= 2}
            lastl = [x for x in tl if x[0] <= 56]
            last = {x.split(":")[0]: x.split(":")[1] for x in (lastl[-1][1] if lastl else tl[0][1]).split() if x.count(":") >= 2}
            for k, own in first.items():
                if own == "neutral" and last.get(k) in ("red", "blue"):
                    g[side[last[k]]] += 1
        taken["ours"] += g["ours"]
        taken["nk"] += g["nk"]
        rows.append("  %-9s %-4s %-24s towns taken %d/%d" % (tag, me, end, g["ours"], g["nk"]))


def med(v):
    return statistics.median(v) if v else float("nan")


print("== M1 gate, %d games (%s), seed %s; OmniAI red in %d, blue in %d" % (games, " ".join(args), seed, seats["red"], seats["blue"]))
print("\n".join(rows))
tp = trav["ours"][0] / max(1, trav["ours"][1])
st = 100.0 * trav["ours"][2] / max(1, trav["ours"][1])
mr = med(mine_ratio)
tr = taken["ours"] / max(1, taken["nk"])
print("  1. tiles per hero-day %.2f (bar at least 10; Nullkiller %.2f)  %s" % (tp, trav["nk"][0] / max(1, trav["nk"][1]), "MET" if tp >= 10 else "MISSED"))
print("  2. hero-days on the starting tile %.1f%% (bar under 5%%; Nullkiller %.1f%%)  %s" % (st, 100.0 * trav["nk"][2] / max(1, trav["nk"][1]), "MET" if st < 5 else "MISSED"))
print("  3. mines day 14, ours over Nullkiller's, median %.2f over %d games (bar at least 0.85)  %s" % (mr, len(mine_ratio), "MET" if mr >= 0.85 else "MISSED"))
print("  4. neutral towns taken by day 56: ours %d, Nullkiller %d, ratio %.2f (bar at least 0.6)  %s" % (taken["ours"], taken["nk"], tr, "MET" if tr >= 0.6 else "MISSED"))
print("  army share of Nullkiller's, median (a lost game counts 0 after its end): " + "  ".join("d%d %.2f (n%d)" % (d, med(share[d]), len(share[d])) for d in DAYS))
print("  games: won %d, lost %d, undecided %d, voids %d (left out), missing %d | fights won %d, lost %d"
      % (ends["won"], ends["lost"], ends["undecided"], ends["void"], ends["missing"], fw, fl))
