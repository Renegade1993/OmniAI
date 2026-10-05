"""diag_table.py LABEL=PREFIX,PREFIX ... [--days=7,14,28,56,84]: one table of medians, ours / Nullkiller's, from the TRUTH lines.

October 4th (megaplan diagnosis). Per day: army, income per day, towns, mines (gold mines), heroes, buildings, best hero level,
total experience, gold held. Pooled over the seed prefixes' ten-map games. Read-only."""
import glob, io, os, re, statistics, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army
days = [7, 14, 28, 56, 84]
specs = []
for a in sys.argv[1:]:
    if a.startswith("--days="):
        days = [int(x) for x in a.split("=", 1)[1].split(",")]
    else:
        specs.append(a)
FIELDS = (("army", None), ("inc", r"inc=(\d+)"), ("towns", r"towns=(\d+)"), ("mines", r"mines=(\d+)"), ("gmines", r"mines=\d+\(g(\d+)\)"),
          ("heroes", r"heroes=(\d+)"), ("bld", r"bld=(\d+)"), ("maxL", r"maxL=(\d+)"), ("exp", r"exp=(\d+)"), ("gold", r"gold=(\d+)"))
def get(rx, s):
    m = re.search(rx, s)
    return int(m.group(1)) if m else None
for spec in specs:
    label, pres = spec.split("=", 1)
    print("== %s" % label)
    for d in days:
        us = {k: [] for k, _ in FIELDS}; them = {k: [] for k, _ in FIELDS}
        n = 0
        for pre in pres.split(","):
            for f in glob.glob(os.path.join(HERE, "runs", "%s[0-9][0-9]_s5k_r01_decisions-red.log" % pre)):
                t = io.open(f, encoding="utf-8", errors="replace").read()
                o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
                b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
                if not (o and b):
                    continue
                n += 1
                for side, line in ((us, o.group(0)), (them, b.group(0))):
                    for k, rx in FIELDS:
                        v = army(line) if k == "army" else get(rx, line)
                        if v is not None:
                            side[k].append(v)
        row = []
        for k, _ in FIELDS:
            if us[k] and them[k]:
                row.append("%s %d/%d" % (k, statistics.median(us[k]), statistics.median(them[k])))
        print("d%-2d n%-2d %s" % (d, n, "  ".join(row)))
