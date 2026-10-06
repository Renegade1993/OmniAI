"""v2_switch.py PREFIX [--maps=...] [--seed=s5k]: how often a v2 hero drops a target it has not reached for a different
one. From the V2 job lines (hero, kind, target tile, d = days away): a job whose target is more than a day away, followed
by the same hero's next job at a different tile on the same day or the next, counts as switched; the same target again
counts as kept. Split by the kind of the dropped job. October 5th, tracing the M1 travel count. Read-only."""
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
JOB = re.compile(r"\s+V2 p\d+ (.+?)\*? (visit|fight|town|gather|explore|hunt|capture) .* at \((\d+) (\d+) (\d+)\) r=\S+ loss=\S+ d=([\d.]+)")

for pre in args:
    kept = collections.Counter()
    switched = collections.Counter()
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        last = {}   # hero -> (kind, tile, d)
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = JOB.match(line)
            if not mm:
                continue
            hero = mm.group(1).strip().rstrip("*").strip()
            kind, tile, d = mm.group(2), (mm.group(3), mm.group(4), mm.group(5)), float(mm.group(6))
            prev = last.get(hero)
            if prev and prev[2] > 1.0:
                if prev[1] == tile:
                    kept[prev[0]] += 1
                else:
                    switched[prev[0]] += 1
            last[hero] = (kind, tile, d)
    print("== %s: jobs more than a day away, then the same hero's next job" % pre)
    for k in sorted(set(kept) | set(switched), key=lambda k: -(kept[k] + switched[k])):
        n = kept[k] + switched[k]
        print("   %-8s %5d  kept %5.1f%%  switched %5.1f%%" % (k, n, 100.0 * kept[k] / n, 100.0 * switched[k] / n))
    n = sum(kept.values()) + sum(switched.values())
    print("   all      %5d  kept %5.1f%%  switched %5.1f%%" % (n, 100.0 * sum(kept.values()) / max(1, n), 100.0 * sum(switched.values()) / max(1, n)))
