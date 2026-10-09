"""decide.py OLD NEW [--maps=...] [--seats=red,blue]: one build call in two parts, October 8th. Read-only.

AX against a rerun of itself read x0.92 (composite, 90% band x0.86 to x0.98) on 73 games: the band paired.py prints was too
narrow, because a lost game weighs about 2.7 log units against about 0.3 for an ordinary game, and losses are rare
(3 to 8 in 120 games), so a couple of extra losses move the composite by 5-8% and a normal band cannot carry that.
So the call is split:
  1. losses: games Nullkiller won, each build over the same maps, seeds and seats, with the exact two-sided p of the split
     under "same loss rate" (given the total number of losses, each falls on either build with chance 1/2 when both
     played the same number of games; binomial with the games-played share otherwise);
  2. the rest: paired.py's composite with every lost game and its pair left out (paired.py --losses=out).
A build is better only if part 2 clears the noise floor measured by AX against AX and part 1 does not go against it."""
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19")
seats = opts.get("seats", "red,blue").split(",")
old, new = args
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def gate(build):
    prefixes = []
    for st in seats:
        for sfx in ("", "2", "3"):
            prefixes.append("V2M1%s%s%sSM" % (build, "B" if st == "blue" else "", sfx))
    r = subprocess.run([sys.executable, os.path.join(HERE, "m1_gate.py")] + prefixes + ["--seed=s5k", "--maps=" + maps],
                       capture_output=True, text=True, creationflags=NO_WINDOW, timeout=600)
    m = re.search(r"games: won (\d+), lost (\d+), undecided (\d+), voids (\d+) \(left out\), missing (\d+)", r.stdout)
    return [int(x) for x in m.groups()] if m else None


def binom_two_sided(k, n, p):
    pk = [math.comb(n, i) * p ** i * (1 - p) ** (n - i) for i in range(n + 1)]
    return min(1.0, sum(x for x in pk if x <= pk[k] * (1 + 1e-9)))


go, gn = gate(old), gate(new)
lo, ln = go[1], gn[1]
po, pn = sum(go[:3]), sum(gn[:3])
tot = lo + ln
p = binom_two_sided(ln, tot, pn / float(po + pn)) if tot else 1.0
print("== %s against %s, seats %s" % (new, old, ",".join(seats)))
print("  1. lost: %s %d of %d games, %s %d of %d (two-sided p %.2f under one loss rate)" % (old, lo, po, new, ln, pn, p))
r = subprocess.run([sys.executable, os.path.join(HERE, "paired.py"), old, new, "--seats=" + ",".join(seats), "--maps=" + maps,
                    "--losses=out"], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=600)
print("  2. without lost games (paired.py --losses=out):")
for line in r.stdout.splitlines()[1:]:
    print("   " + line)
