"""paired.py OLD NEW [--days=21,28,42,56] [--maps=...] [--seed=s5k]: two v2 builds compared game by game on the same map and
seed. October 6th (30-game medians and win counts could not tell any of the day's builds apart). Read-only.

OLD and NEW are build letters as in the smoke tags (AG, AM, AN): the games V2M1<X>SM, V2M1<X>2SM and V2M1<X>3SM. For each
day: the army share of Nullkiller's (TRUTH army, red over blue) of each game, the per-game log ratio NEW/OLD where both
games reached that day, its mean as a factor with a 90% band (t, 1.7 standard errors), and how many games NEW led.
A game that ended before the day (a loss) counts as share 0.01, so a loss weighs against its build. Voids (no TRUTH
line on day 7) are left out of both sides."""
import io
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from traj import army  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
days = [int(x) for x in opts.get("days", "21,28,42,56").split(",")]
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s5k")
old, new = args[0], args[1]


def shares(build, sfx, m):
    f = os.path.join(HERE, "runs", "V2M1%s%sSM%s_%s_r01_decisions-red.log" % (build, sfx, m, seed))
    if not os.path.exists(f):
        return None
    t = io.open(f, encoding="utf-8", errors="replace").read()
    if not re.search(r"TRUTH d7 ", t):
        return None
    last = max([int(x) for x in re.findall(r"^DAY (\d+)", t, re.M)] or [0])
    out = {}
    for d in days:
        o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
        b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
        if o and b and army(b.group(0)):
            out[d] = max(0.01, army(o.group(0)) / army(b.group(0)))
        elif last < d:
            out[d] = 0.01
    return out


print("== %s against %s, paired by map and seed" % (new, old))
for d in days:
    diffs = []
    for sfx in ("", "2", "3"):
        for m in maps:
            a, b = shares(old, sfx, m), shares(new, sfx, m)
            if a is None or b is None or d not in a or d not in b:
                continue
            diffs.append(math.log(b[d] / a[d]))
    n = len(diffs)
    if n < 3:
        print("  d%d: %d pairs" % (d, n))
        continue
    mean = sum(diffs) / n
    sd = math.sqrt(sum((x - mean) ** 2 for x in diffs) / (n - 1))
    se = sd / math.sqrt(n)
    print("  d%-2d %2d pairs: %s/%s x%.2f (90%% band x%.2f to x%.2f), %s led in %d"
          % (d, n, new, old, math.exp(mean), math.exp(mean - 1.7 * se), math.exp(mean + 1.7 * se), new, sum(x > 0 for x in diffs)))
