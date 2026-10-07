"""truth_cmp.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k] [--days=7,14,21,28,42]: ours against Nullkiller's on every TRUTH field,
median over games, by day. October 7th (the day 21-28 dip in army share: what Nullkiller has more of by then). Either seat
(the log's colour is ours, m1_gate.py's rule). Fields: gold income a day, towns, buildings, fort sum, heroes, dwellings,
mines, army, experience, the best hero's level, its army and its attack/defence/power/knowledge. Read-only."""
import glob
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
days = [int(x) for x in opts.get("days", "7,14,21,28,42").split(",")]
F = [("inc", r"inc=(\d+)/d"), ("towns", r"towns=(\d+)"), ("bld", r"bld=(\d+)"), ("fort", r"fort=(\d+)"), ("heroes", r"heroes=(\d+)"),
     ("dw", r"dw=(\d+)"), ("mines", r"mines=(\d+)"), ("army", r"army=(\d+)"), ("exp", r"exp=(\d+)"), ("maxL", r"maxL=(\d+)"),
     ("bestArmy", r"best=[^:]*:(\d+)@"), ("att", r" a(\d+)/d"), ("def", r"/d(\d+)/p"), ("pow", r"/p(\d+)/k"), ("kno", r"/k(\d+) ")]
vals = {d: {f: ([], []) for f, _ in F} for d in days}
n = 0
for pre in args:
    for m in maps:
        logs = sorted(glob.glob(os.path.join(HERE, "runs", "%s%s_%s_r0[12]_decisions-*.log" % (pre, m, seed))))
        if not logs:
            continue
        t = io.open(logs[0], encoding="utf-8", errors="replace").read()
        if not re.search(r"TRUTH d7 ", t):
            continue
        n += 1
        for d in days:
            o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
            b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
            if not (o and b):
                continue
            for f, rx in F:
                mo, mb = re.search(rx, o.group(0)), re.search(rx, b.group(0))
                if mo and mb:
                    vals[d][f][0].append(int(mo.group(1)))
                    vals[d][f][1].append(int(mb.group(1)))
med = lambda v: statistics.median(v) if v else float("nan")
print("== %s, %d games; median ours / Nullkiller's" % (" ".join(args), n))
print("  day " + "".join("%14s" % f for f, _ in F))
for d in days:
    print("  d%-3d" % d + "".join("%14s" % ("%s/%s" % (("%.0f" % med(vals[d][f][0])), ("%.0f" % med(vals[d][f][1])))) for f, _ in F))
