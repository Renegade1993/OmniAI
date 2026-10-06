"""recall_check.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k]: were the v2 main hero's threat recalls needed? October 6th.
Read-only.

An episode is a run of days on which the main hero's job is a town job scored 1e10 or more (the threat recall), to one
town. For each: its length in days, the main hero's army at the start, and from the HEROES and TOWNS lines of the
episode and the five days after it: the nearest any blue hero came to the town (Chebyshev, same level), whether the
town changed hands, and the strongest blue army that came within 3 tiles. 'came' = a blue hero within 3 tiles of the
town; 'fell' = the town not red at some dawn up to five days after the episode."""
import collections
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s5k")
HB = re.compile(r"(\d+):([^@:]+)@(\d+),(\d+),(\d+):(\d+):\d+(?::\d+:\d+)?")
REC = re.compile(r"\s+V2 p\d+ (.+?)\* town (.+?) at \((\d+) (\d+) (\d+)\) .* s=(\d{11,}) ")
TW = re.compile(r"(\d+),(\d+),(\d+):(\w+):")

tot = collections.Counter()
for pre in args:
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        day = 0
        blue, red, towns, rec = {}, {}, {}, {}
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.match(r"DAY (\d+)", line)
            if mm:
                day = int(mm.group(1))
                continue
            mm = re.search(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", line)
            if mm:
                d = int(mm.group(1))
                red[d] = [(h.group(2), (int(h.group(3)), int(h.group(4)), int(h.group(5))), int(h.group(6))) for h in HB.finditer(mm.group(2))]
                blue[d] = [(h.group(2), (int(h.group(3)), int(h.group(4)), int(h.group(5))), int(h.group(6))) for h in HB.finditer(mm.group(3))]
                continue
            mm = re.search(r"TOWNS d(\d+) (.*)", line)
            if mm:
                towns[int(mm.group(1))] = {(int(t.group(1)), int(t.group(2)), int(t.group(3))): t.group(4) for t in TW.finditer(mm.group(2))}
                continue
            mm = REC.match(line)
            if mm and day not in rec:
                rec[day] = (mm.group(2), (int(mm.group(3)), int(mm.group(4)), int(mm.group(5))), mm.group(1))
        days = sorted(rec)
        eps = []
        for d in days:
            if eps and eps[-1][1] == d - 1 and eps[-1][2] == rec[d][1]:
                eps[-1][1] = d
            else:
                eps.append([d, d, rec[d][1], rec[d][0], rec[d][2]])
        for a, b, tile, name, hero in eps:
            near, strongest, fell = 999, 0, False
            for d in range(a, b + 6):
                for _, p, army in blue.get(d, []):
                    if p[2] == tile[2]:
                        dd = max(abs(p[0] - tile[0]), abs(p[1] - tile[1]))
                        near = min(near, dd)
                        if dd <= 3:
                            strongest = max(strongest, army)
                tw = towns.get(d, {})
                # the town record is keyed by the town's position; the visitable tile is one below it
                own = [o for t, o in tw.items() if t[2] == tile[2] and abs(t[0] - tile[0]) <= 2 and abs(t[1] - tile[1]) <= 1]
                if own and own[0] != "red":
                    fell = True
            harmy = next((army for n, _, army in red.get(a, []) if n == hero), 0)
            came = near <= 3
            tot["episodes"] += 1
            tot["days"] += b - a + 1
            tot["came"] += came
            tot["fell"] += fell
            if not came:
                tot["days, nobody came"] += b - a + 1
            print("%s%s d%d-%d (%d days) %s to %s: main army %d; nearest blue %s tiles, strongest within 3: %d; %s"
                  % (pre, m, a, b, b - a + 1, hero, name, harmy, near if near < 999 else "-", strongest,
                     "FELL" if fell else "held"))
print("== %s" % dict(tot))
