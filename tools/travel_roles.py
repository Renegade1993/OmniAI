"""travel_roles.py PREFIX [PREFIX ...] [--maps=...] [--seed=s6k] [--last=42]: tiles moved per hero-day split by role, ours
(red) and Nullkiller's (blue): the hero holding the side's largest army at dawn against the others, from the HEROES lines
(dawn-to-dawn Chebyshev displacement, same level). October 5th, tracing the M1 gate's travel count. Read-only."""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s6k")
last = int(opts.get("last", "42"))
HX = re.compile(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):(\d+):\d+(?::\d+:\d+)?")

acc = {(s, r): [0, 0, 0] for s in ("red", "blue") for r in ("main", "other")}
for pre in args:
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        prev = None
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.search(r"HEROES d(\d+) red (.*?) \| blue (.*?) \|", line)
            if not mm:
                continue
            d = int(mm.group(1))
            pos = {}
            for s, x in (("red", mm.group(2)), ("blue", mm.group(3))):
                hs = {h.group(1): ((int(h.group(2)), int(h.group(3)), int(h.group(4))), int(h.group(5))) for h in HX.finditer(x)}
                pos[s] = hs
            if prev and prev[0] == d - 1 and d <= last:
                for s in ("red", "blue"):
                    old = prev[1][s]
                    if not old:
                        continue
                    main = max(old, key=lambda k: old[k][1])
                    for hid, (p, _) in pos[s].items():
                        q = old.get(hid)
                        if q and q[0][2] == p[2]:
                            dist = max(abs(p[0] - q[0][0]), abs(p[1] - q[0][1]))
                            a = acc[(s, "main" if hid == main else "other")]
                            a[0] += dist
                            a[1] += 1
                            a[2] += dist == 0
            prev = (d, pos)
print("== %s (days 1-%d, seed %s)" % (" ".join(args), last, seed))
for s, who in (("red", "ours"), ("blue", "Nullkiller")):
    print("  %-10s main %4.1f tiles over %4d hero-days | others %4.1f tiles over %4d hero-days (%.0f%% still)" % (
        who, acc[(s, "main")][0] / max(1, acc[(s, "main")][1]), acc[(s, "main")][1],
        acc[(s, "other")][0] / max(1, acc[(s, "other")][1]), acc[(s, "other")][1],
        100.0 * acc[(s, "other")][2] / max(1, acc[(s, "other")][1])))
