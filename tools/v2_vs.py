"""v2_vs.py NEWPREFIX [OLDPREFIX] [--maps=05,01,10] [--days=7,14,21,28,42]: one game per map, the v2 engine against the
legacy build on the same map and seed, both against Nullkiller. October 4th (megaplan option B, development smokes).

For each map: army share of Nullkiller's, mines ours/NK, towns ours/NK, buildings ours/NK, best level ours/NK on each day,
then fights won and lost, V2 STOP and LOST lines, heroes lost, and the game's end. OLDPREFIX defaults to SW2S4TNP (the
plain legacy build, seed 5001). Read-only.
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
new = args[0]
old = args[1] if len(args) > 1 else "SW2S4TNP"
maps = opts.get("maps", "05,01,10").split(",")
days = [int(d) for d in opts.get("days", "7,14,21,28,42").split(",")]


def field(rx, s):
    m = re.search(rx, s)
    return m.group(1) if m else "-"


def read(tag):
    f = os.path.join(HERE, "runs", tag + "_s5k_r01_decisions-red.log")
    if not os.path.exists(f):
        return None
    return io.open(f, encoding="utf-8", errors="replace").read()


def end(tag):
    f = os.path.join(HERE, "runs", tag + ".out")
    if not os.path.exists(f):
        return "-"
    m = re.search(r"run 1 \(red\): (day \d+\s+\S+(?: \S+)?)", io.open(f, encoding="utf-8", errors="replace").read())
    return re.sub(r"\s+", " ", m.group(1)) if m else "running"


for m in maps:
    print("== map %s" % m)
    for tag in (new + m, old + m):
        t = read(tag)
        if t is None:
            print("  %-14s no log" % tag)
            continue
        cells = []
        for d in days:
            o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
            b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
            if not (o and b) or not army(b.group(0)):
                cells.append("d%d -" % d)
                continue
            os_, bs = o.group(0), b.group(0)
            cells.append("d%d %.2f m%s/%s t%s/%s b%s/%s L%s/%s" % (
                d, army(os_) / army(bs), field(r"mines=(\d+)", os_), field(r"mines=(\d+)", bs),
                field(r"towns=(\d+)", os_), field(r"towns=(\d+)", bs), field(r"bld=(\d+)", os_), field(r"bld=(\d+)", bs),
                field(r"maxL=(\d+)", os_), field(r"maxL=(\d+)", bs)))
        won = len(re.findall(r"FIGHT won", t))
        lost = len(re.findall(r"FIGHT LOST", t))
        stops = len(re.findall(r"V2 STOP", t))
        heroes_lost = len(re.findall(r"V2 LOST", t))
        print("  %-14s %s" % (tag, " | ".join(cells)))
        print("  %-14s fights won %d lost %d, V2 STOP %d, V2 LOST %d, end: %s" % ("", won, lost, stops, heroes_lost, end(tag)))
