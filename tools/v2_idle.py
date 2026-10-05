"""v2_idle.py PREFIX [--maps=...] [--last=42]: why v2 heroes end a day on the tile they started it on. October 4th.

For every hero that stands on the same tile at two consecutive dawns (HEROES lines, days 1..last), the hero's V2 lines of that day
decide the class: HOLD (main hero holding a threatened town), no job (no V2 line for the hero), STOP (a walk cut short by a
guard), a job of kind K that moved and came back or failed (by the kind of its last V2 line). Prints counts per class over all
games, and the share of hero-days. Read-only."""
import collections
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
last = int(opts.get("last", "42"))

for pre in args:
    cnt = collections.Counter()
    total = still = 0
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_s5k_r01_decisions-red.log" % (pre, m))
        if not os.path.exists(f):
            continue
        day = 0
        heroes = {}
        lines = collections.defaultdict(list)   # (day, name) -> list of V2 lines
        holds = set()
        for line in io.open(f, encoding="utf-8", errors="replace"):
            mm = re.match(r"DAY (\d+)", line)
            if mm:
                day = int(mm.group(1))
                continue
            mm = re.match(r"\s+HEROES d(\d+) red (.*?) \|", line)
            if mm:
                heroes[int(mm.group(1))] = {h.group(2).strip(): (int(h.group(3)), int(h.group(4)), int(h.group(5)))
                                            for h in re.finditer(r"(\d+):([^@]+)@(\d+),(\d+),(\d+):\d+:\d+", mm.group(2))}
                continue
            mm = re.match(r"\s+V2 p\d+ (.+?)\*? (visit|fight|town|gather|explore|hunt|capture) ", line)
            if mm:
                lines[(day, mm.group(1).strip().rstrip("*").strip())].append(mm.group(2))
                continue
            mm = re.match(r"\s+V2 STOP (.+?) short of", line)
            if mm:
                lines[(day, mm.group(1).strip())].append("STOP")
                continue
            mm = re.match(r"\s+V2 HOLD (.+?) stays in", line)
            if mm:
                holds.add((day, mm.group(1).strip()))
        for d, pos in heroes.items():
            nxt = heroes.get(d + 1)
            if not nxt or d > last:
                continue
            for name, p in pos.items():
                if name not in nxt:
                    continue
                total += 1
                if nxt[name] != p:
                    continue
                still += 1
                ls = lines.get((d, name), [])
                if (d, name) in holds:
                    k = "HOLD"
                elif not ls:
                    k = "no job"
                elif "STOP" in ls:
                    k = "STOP"
                else:
                    k = "job " + ls[-1]
                cnt[k] += 1
    print("== %s: %d of %d hero-days end on the starting tile (%.0f%%)" % (pre, still, total, 100.0 * still / max(1, total)))
    for k, v in cnt.most_common():
        print("   %-16s %5d  %4.1f%% of hero-days" % (k, v, 100.0 * v / max(1, total)))
