"""lane_effect.py BUILD [BUILD ...] [--maps=...] [--days=21,28,42,56]: does one test lane play differently? October 8th.
Read-only. For every v2 smoke game of the builds named (both seats, seedbase suffixes "", 2, 3), the game's composite: the
mean log army share of Nullkiller's over the days (a game Nullkiller won counts 0.05 after its end, a game cut short
undecided leaves out the days it missed, shares clipped to 0.05..20). Each game's residual is its composite minus the
mean composite of the same map, seed and seat over the builds named, so map and seed effects drop out. The residuals are
then averaged by the lane that played the game (from runs/laneN_feed*.out). A lane whose mean residual stands well
outside the others' would be a lane effect (a lane-local install difference), not build noise."""
import glob
import io
import math
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
days = [int(x) for x in opts.get("days", "21,28,42,56").split(",")]
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")

lane_of = {}
for f in glob.glob(os.path.join(HERE, "runs", "lane*_feed*.out")):
    lane = re.match(r"(lane\d+)_", os.path.basename(f)).group(1)
    for line in io.open(f, encoding="utf-8", errors="replace"):
        m = re.match(r"(V2M1\S+SM\d+)_s5k \(", line)
        if m:
            lane_of[m.group(1)] = lane  # the last feeder to take a tag played it


def composite(tag, seat):
    f = os.path.join(HERE, "runs", "%s_s5k_%s_decisions-%s.log" % (tag, "r01" if seat == "red" else "r02", seat))
    if not os.path.exists(f):
        return None
    t = io.open(f, encoding="utf-8", errors="replace").read()
    if not re.search(r"TRUTH d7 ", t):
        return None
    last = max([int(x) for x in re.findall(r"^DAY (\d+)", t, re.M)] or [0])
    of = os.path.join(HERE, "runs", tag + ".out")
    rm = re.search(r"run \d+ \((?:red|blue)\): day \d+\s+(winner \S+|undecided)",
                   io.open(of, encoding="utf-8", errors="replace").read()) if os.path.exists(of) else None
    lost = bool(rm and rm.group(1).startswith("winner") and "OmniAI" not in rm.group(1))
    logs = []
    for d in days:
        o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
        b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
        if o and b and army(b.group(0)):
            logs.append(math.log(min(20.0, max(0.05, army(o.group(0)) / army(b.group(0))))))
        elif last < d and lost:
            logs.append(math.log(0.05))
    return sum(logs) / len(logs) if len(logs) == len(days) else None


games = []  # (build, cell, lane, composite)
for build in args:
    for seat in ("red", "blue"):
        for sfx in ("", "2", "3"):
            for m in maps:
                tag = "V2M1%s%s%sSM%s" % (build, "B" if seat == "blue" else "", sfx, m)
                c = composite(tag, seat)
                if c is not None:
                    games.append((build, (m, sfx, seat), lane_of.get(tag, "?"), c))

cell = defaultdict(list)
for b, k, l, c in games:
    cell[k].append(c)
by_lane = defaultdict(list)
by_build_lane = defaultdict(list)
for b, k, l, c in games:
    if len(cell[k]) < 2:
        continue
    r = c - sum(cell[k]) / len(cell[k])
    by_lane[l].append(r)
    by_build_lane[(b, l)].append(r)

print("== residual composite by lane (game minus the mean of its map, seed and seat over %s), %d games" % (" ".join(args), len(games)))
for l in sorted(by_lane, key=lambda x: (len(x), x)):
    v = by_lane[l]
    n = len(v)
    mean = sum(v) / n
    se = math.sqrt(sum((x - mean) ** 2 for x in v) / (n - 1)) / math.sqrt(n) if n > 1 else float("nan")
    per = "  ".join("%s %+.2f (%d)" % (b, sum(by_build_lane[(b, l)]) / len(by_build_lane[(b, l)]), len(by_build_lane[(b, l)]))
                    for b in args if by_build_lane.get((b, l)))
    print("  %-7s n%-3d x%.2f (90%% band x%.2f to x%.2f) | %s" % (l, n, math.exp(mean), math.exp(mean - 1.7 * se), math.exp(mean + 1.7 * se), per))
