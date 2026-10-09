"""side_shift.py OLD NEW [--maps=...] [--seats=red,blue] [--days=14,21,28,42,56]: when NEW reads lower than OLD against
Nullkiller, which side moved? October 8th. Read-only. Paired by map, seed and seat as paired.py; for each day the mean log
ratio NEW/OLD of OmniAI's own army and, separately, of Nullkiller's army (TRUTH lines), with 90% bands. Games that did
not reach the day are left out."""
import io
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
days = [int(x) for x in opts.get("days", "14,21,28,42,56").split(",")]
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seats = opts.get("seats", "red,blue").split(",")
old, new = args


def armies(build, sfx, m, seat):
    f = os.path.join(HERE, "runs", "V2M1%s%s%sSM%s_s5k_%s_decisions-%s.log"
                     % (build, "B" if seat == "blue" else "", sfx, m, "r01" if seat == "red" else "r02", seat))
    if not os.path.exists(f):
        return None
    t = io.open(f, encoding="utf-8", errors="replace").read()
    out = {}
    for d in days:
        o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
        b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
        if o and b and army(o.group(0)) and army(b.group(0)):
            out[d] = (army(o.group(0)), army(b.group(0)))
    return out


def band(v):
    n = len(v)
    mean = sum(v) / n
    se = math.sqrt(sum((x - mean) ** 2 for x in v) / (n - 1)) / math.sqrt(n)
    return "x%.2f (x%.2f to x%.2f)" % (math.exp(mean), math.exp(mean - 1.7 * se), math.exp(mean + 1.7 * se))


print("== %s against %s, paired by map, seed and seat (%s): army of each side, NEW/OLD" % (new, old, ",".join(seats)))
for d in days:
    ours, nk = [], []
    for st in seats:
        for sfx in ("", "2", "3"):
            for m in maps:
                a, b = armies(old, sfx, m, st), armies(new, sfx, m, st)
                if a and b and d in a and d in b:
                    ours.append(math.log(b[d][0] / a[d][0]))
                    nk.append(math.log(b[d][1] / a[d][1]))
    if len(ours) >= 3:
        print("  d%-2d %3d pairs: OmniAI %s   Nullkiller %s" % (d, len(ours), band(ours), band(nk)))
