"""map_shares.py MAP PREFIX [PREFIX ...] [--seed=s5k]: army share of Nullkiller's on days 7/14/21/28/42/56 and the game's end, one
row per prefix, for one map. October 7th (which builds lose map 13, and when). Either seat. Read-only."""
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from traj import army  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
seed = opts.get("seed", "s5k")
m = args[0]
for pre in args[1:]:
    logs = sorted(glob.glob(os.path.join(HERE, "runs", "%s%s_%s_r0[12]_decisions-*.log" % (pre, m, seed))))
    if not logs:
        print("  %-12s no log" % pre)
        continue
    t = io.open(logs[0], encoding="utf-8", errors="replace").read()
    cells = []
    for d in (7, 14, 21, 28, 42, 56):
        o = re.search(r"TRUTH d%d [a-z]+\* [^\n]*" % d, t)
        b = re.search(r"TRUTH d%d [a-z]+ [^*][^\n]*" % d, t)
        cells.append("d%d %s" % (d, "%.2f" % (army(o.group(0)) / army(b.group(0))) if o and b and army(b.group(0)) else "  - "))
    out = os.path.join(HERE, "runs", pre + m + ".out")
    end = ""
    if os.path.exists(out):
        mm = re.search(r"run \d+ \((?:red|blue)\): day (\d+)\s+(winner \S+|undecided|ERROR.*?)\s", io.open(out, encoding="utf-8", errors="replace").read())
        end = mm.group(1) + " " + mm.group(2) if mm else ""
    print("  %-12s %s   %s" % (pre + m, "  ".join(cells), end))
