"""build_first.py PREFIX [PREFIX ...] [--maps=...] [--last=28]: which buildings each side puts up by day --last, and when.
October 8th (the army-growth gap: what Nullkiller builds by days 14-28 that OmniAI does not). Read-only.

From the TRUTH "built: <town>+<id>" suffixes (a building raised since the last dawn) of every game PREFIXnn_s5k, either seat
(the log's colour is ours, m1_gate.py's rule): per building id, the share of games in which each side raised it by day
--last and the median first day among those games. Every town counts (a captured town's buildings too). Ids are VCMI
BuildingIDs (audit_spread.BUILDING names them where it knows them)."""
import glob
import io
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
try:
    from audit_spread import BUILDING
except Exception:  # noqa: BLE001
    BUILDING = {}

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "")
last = int(opts.get("last", "28"))

first = {"ours": defaultdict(list), "nk": defaultdict(list)}
games = 0
for p in args:
    for f in sorted(glob.glob(os.path.join(HERE, "runs", p + "*_s5k_r0[12]_decisions-*.log"))):
        m = re.search(r"SM(\d+)_s5k_r0\d_decisions-(red|blue)\.log$", f)
        if not m or (maps and m.group(1) not in maps.split(",")):
            continue
        me = m.group(2)
        t = io.open(f, encoding="utf-8", errors="replace").read()
        if not re.search(r"TRUTH d7 ", t):
            continue
        games += 1
        seen = {"ours": {}, "nk": {}}
        for tm in re.finditer(r"TRUTH d(\d+) (red|blue)(\*?) [^\n]*?built: ([^\n]*)", t):
            d = int(tm.group(1))
            if d > last:
                continue
            side = "ours" if tm.group(2) == me else "nk"
            for b in re.findall(r"\+(\d+)", tm.group(4)):
                seen[side].setdefault(int(b), d)
        for side in seen:
            for b, d in seen[side].items():
                first[side][b].append(d)


def med(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


print("== %s, %d games, buildings raised by day %d: share of games (median first day), ours | Nullkiller" % (" ".join(args), games, last))
ids = sorted(set(first["ours"]) | set(first["nk"]))
rows = []
for b in ids:
    o, n = first["ours"].get(b, []), first["nk"].get(b, [])
    rows.append((len(n) - len(o), b, o, n))
for diff, b, o, n in sorted(rows, key=lambda r: -abs(r[0])):
    name = BUILDING.get(b, "") if isinstance(BUILDING, dict) else ""
    print("  %3d %-22s ours %3.0f%% (d%s) | NK %3.0f%% (d%s)   NK-ours %+d games"
          % (b, name[:22], 100.0 * len(o) / max(1, games), med(o), 100.0 * len(n) / max(1, games), med(n), diff))
