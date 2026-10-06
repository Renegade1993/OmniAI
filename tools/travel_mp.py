"""travel_mp.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k] [--last=42]: movement allowance and travel per allowance, ours
(red) and Nullkiller's (blue), from HEROES lines that carry ':limit:remaining' (V2M1X and later). Per hero-day: the land
movement allowance at dawn; tiles moved dawn to dawn (Chebyshev, same level); tiles per 1000 movement points; and for
blue (it moves after us, so its remaining is what its last turn left) the share of allowance left unspent. Split by
role: each side's largest army at dawn against the rest. October 5th. Read-only."""
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
HX = re.compile(r"(\d+):[^@:]+@(\d+),(\d+),(\d+):(\d+):\d+:(\d+):(\d+)")

for pre in args:
    acc = {(s, r): [0, 0, 0, 0] for s in ("red", "blue") for r in ("main", "other")}   # tiles, hero-days, limit, left
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
                pos[s] = {h.group(1): ((int(h.group(2)), int(h.group(3)), int(h.group(4))), int(h.group(5)),
                                       int(h.group(6)), int(h.group(7))) for h in HX.finditer(x)}
            if prev and prev[0] == d - 1 and d <= last:
                for s in ("red", "blue"):
                    old = prev[1][s]
                    if not old:
                        continue
                    main = max(old, key=lambda k: old[k][1])
                    for hid, now in pos[s].items():
                        q = old.get(hid)
                        if not q or q[0][2] != now[0][2]:
                            continue
                        a = acc[(s, "main" if hid == main else "other")]
                        a[0] += max(abs(now[0][0] - q[0][0]), abs(now[0][1] - q[0][1]))
                        a[1] += 1
                        a[2] += q[2]
                        # blue's remaining on today's line is what yesterday's allowance left
                        a[3] += now[3] if s == "blue" else 0
            prev = (d, pos)
    print("== %s (days 1-%d, seed %s)" % (pre, last, seed))
    for s, who in (("red", "ours"), ("blue", "Nullkiller")):
        for r in ("main", "other"):
            a = acc[(s, r)]
            n = max(1, a[1])
            extra = ", %.0f%% left unspent" % (100.0 * a[3] / max(1, a[2])) if s == "blue" else ""
            print("  %-10s %-5s %4d hero-days: allowance %5.0f, %4.1f tiles, %4.2f tiles per 100 points%s" % (
                who, r, a[1], a[2] / n, a[0] / n, 100.0 * a[0] / max(1, a[2]), extra))
