"""walk_eff.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k]: how straight the v2 heroes walk. October 6th (the tiles gap:
equal movement allowance, less ground per movement point than Nullkiller). Read-only.

Per red hero-day (day d dawn to day d+1 dawn, same level): tiles walked = the sum of steps= of that day's V2 job lines
for the hero (a walk the server cut short still counts in full, so this is an upper bound), displacement = Chebyshev
tiles dawn to dawn, walks = how many job lines. Straightness = displacement / tiles walked. Split by the kind of job
that took most of the day's steps (explore, visit, fight, town, gather, ...). Also: the share of walks of 1-3 steps,
and the daily mpLeft from the V2 dN summary line."""
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
HX = re.compile(r"(\d+):([^@:]+)@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?")
JOB = re.compile(r"\s+V2 p\d+ (.+?)\*? (visit|fight|town|gather|explore|hunt|capture) .* steps=(\d+)")
SUM = re.compile(r"\s+V2 d(\d+) passes=(\d+) moves=(\d+) .*mpLeft=(\d+)%")


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1])) if a[2] == b[2] else None


for pre in args:
    bykind = collections.defaultdict(lambda: [0, 0, 0, 0])   # hero-days, walked, displaced, walks
    short = [0, 0]
    mpl, passes = [], []
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        dawn = {}
        walks = collections.defaultdict(lambda: collections.defaultdict(list))   # day -> hero -> [(kind, steps)]
        day = 0
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.match(r"DAY (\d+)", line)
            if mm:
                day = int(mm.group(1))
                continue
            mm = re.search(r"HEROES d(\d+) red (.*?) \|", line)
            if mm:
                dawn[int(mm.group(1))] = {h.group(2).strip(): (int(h.group(3)), int(h.group(4)), int(h.group(5)))
                                          for h in HX.finditer(mm.group(2))}
                continue
            mm = JOB.match(line)
            if mm:
                n = int(mm.group(3))
                walks[day][mm.group(1).strip()].append((mm.group(2), n))
                short[0] += n <= 3
                short[1] += 1
                continue
            mm = SUM.match(line)
            if mm:
                passes.append(int(mm.group(2)))
                mpl.append(int(mm.group(4)))
        for d, heroes in walks.items():
            if d not in dawn or d + 1 not in dawn:
                continue
            for name, ws in heroes.items():
                if name not in dawn[d] or name not in dawn[d + 1]:
                    continue
                disp = cheb(dawn[d][name], dawn[d + 1][name])
                if disp is None:
                    continue
                per = collections.Counter()
                for k, n in ws:
                    per[k] += n
                kind = per.most_common(1)[0][0]
                b = bykind[kind]
                b[0] += 1
                b[1] += sum(n for _, n in ws)
                b[2] += disp
                b[3] += len(ws)
    tot = [sum(b[i] for b in bykind.values()) for i in range(4)]
    print("== %s: %d hero-days with a walk; walked %.1f tiles a day, displaced %.1f, straightness %.2f, %.1f walks a day; "
          "walks of 1-3 steps %d%%; mpLeft mean %.1f%%; passes mean %.1f"
          % (pre, tot[0], tot[1] / max(1, tot[0]), tot[2] / max(1, tot[0]), tot[2] / max(1, tot[1]), tot[3] / max(1, tot[0]),
             100 * short[0] // max(1, short[1]), sum(mpl) / max(1, len(mpl)), sum(passes) / max(1, len(passes))))
    for k, b in sorted(bykind.items(), key=lambda kv: -kv[1][0]):
        print("   %-8s %5d hero-days  walked %5.1f  displaced %5.1f  straightness %.2f  walks %.1f"
              % (k, b[0], b[1] / b[0], b[2] / b[0], b[2] / max(1, b[1]), b[3] / b[0]))
