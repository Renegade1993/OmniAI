"""town_trips.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k]: how often the v2 main hero stands in one of our towns, and
what it collects there. October 6th (V2M1AI removed most threat recalls and lost army on days 28-56; did the recalls double
as trips home to collect the recruits?). Read-only.

Per game: main-hero dawns in one of our towns (HEROES position on a red town's tile or beside it, from TOWNS), the main
hero's army jumps of 20% or more from one dawn to the next made in a town, and the army share of Nullkiller's on day 42
and 56 (red's TRUTH army over blue's)."""
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
TW = re.compile(r"(\d+),(\d+),(\d+):(\w+):")
ARMY = re.compile(r"TRUTH d(\d+) (red|blue)\*? .*? army=(\d+)")

for pre in args:
    tot = [0, 0, 0, 0]
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        red, towns, army, main = {}, {}, {}, {}
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.search(r"HEROES d(\d+) red (.*?) \|", line)
            if mm:
                red[int(mm.group(1))] = {h.group(2): ((int(h.group(3)), int(h.group(4)), int(h.group(5))), int(h.group(6)))
                                         for h in HB.finditer(mm.group(2))}
                continue
            mm = re.search(r"TOWNS d(\d+) (.*)", line)
            if mm:
                towns[int(mm.group(1))] = [(int(t.group(1)), int(t.group(2)), int(t.group(3))) for t in TW.finditer(mm.group(2))
                                           if t.group(4) == "red"]
                continue
            mm = ARMY.search(line)
            if mm:
                army.setdefault(int(mm.group(1)), {})[mm.group(2)] = int(mm.group(3))
        indays, jumps, days = 0, 0, 0
        for d in sorted(red):
            if not red[d]:
                continue
            name = max(red[d], key=lambda n: red[d][n][1])
            pos, a = red[d][name]
            days += 1
            intown = any(t[2] == pos[2] and abs(t[0] - pos[0]) <= 2 and abs(t[1] - pos[1]) <= 1 for t in towns.get(d, []))
            indays += intown
            nxt = red.get(d + 1, {}).get(name)
            if intown and nxt and nxt[1] >= 1.2 * max(1, a):
                jumps += 1
        sh = lambda d: army.get(d, {}).get("red", 0) / max(1, army.get(d, {}).get("blue", 1)) if d in army else float("nan")
        tot[0] += indays
        tot[1] += days
        tot[2] += jumps
        print("  %s%s main-hero dawns in a town %d of %d, army jumps there %d, share d42 %.2f d56 %.2f"
              % (pre, m, indays, days, jumps, sh(42), sh(56)))
    print("== %s: dawns in a town %d of %d (%.1f%%), army jumps there %d" % (pre, tot[0], tot[1], 100.0 * tot[0] / max(1, tot[1]), tot[2]))
