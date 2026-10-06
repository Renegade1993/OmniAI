"""fort_days.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k]: the day our first Citadel and Castle were built (BUILD lines,
building 8 and 9) and the first day each side's summed fort level (TRUTH fort=, 0-3 per town) reached 2 and 3. October 6th
(Nullkiller had its walls about four days before ours in V2M1AG). Medians over the games; 99 = never by the game's end.
Read-only."""
import io
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s5k")

for pre in args:
    rows = []
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        day, cit, cas, fort = 0, 99, 99, {"red*": [], "blue": []}
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.match(r"DAY (\d+)", line)
            if mm:
                day = int(mm.group(1))
                continue
            mm = re.search(r"BUILD .*: building (\d+)[, ]", line)
            if mm:
                b = int(mm.group(1))
                if b == 8:
                    cit = min(cit, day)
                if b == 9:
                    cas = min(cas, day)
                continue
            mm = re.search(r"TRUTH d(\d+) (red\*|blue) .*?fort=(\d+)", line)
            if mm:
                fort[mm.group(2)].append((int(mm.group(1)), int(mm.group(3))))
        first = lambda side, lv: next((d for d, v in fort[side] if v >= lv), 99)
        rows.append((cit, cas, first("red*", 2), first("red*", 3), first("blue", 2), first("blue", 3)))
    if not rows:
        continue
    med = lambda i: statistics.median(r[i] for r in rows)
    print("%s (%d games): ours Citadel built d%s, Castle d%s; fort sum 2/3 ours d%s/d%s, Nullkiller d%s/d%s"
          % (pre, len(rows), med(0), med(1), med(2), med(3), med(4), med(5)))
