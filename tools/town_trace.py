"""town_trace.py PREFIX [PREFIX ...] --maps=15 [--last=21]: every town on a map, game by game: who saw it first and when,
its garrison then, who took it and when, how far it is from each start (tiles, same level only), and for OmniAI the day it
first became a "capture" option and what OmniAI's main hero did on the days between first sight and capture. October 8th
(lead: why OmniAI leaves map 15's neutral towns until late). Read-only. Source: the TOWNS line (every town, owner,
garrison strength, who sees it today) and the V2 option lines of OmniAI's own log; Nullkiller's choices are not logged,
so for it only sight and capture days are known."""
import glob
import io
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "15").split(",")
last = int(opts.get("last", "21"))

summary = Counter()
for p in args:
    for f in sorted(glob.glob(os.path.join(HERE, "runs", p + "*_s5k_r0[12]_decisions-*.log"))):
        m = re.search(r"SM(\d+)_s5k_r0\d_decisions-(red|blue)\.log$", f)
        if not m or m.group(1) not in maps:
            continue
        me = m.group(2)
        nk = "blue" if me == "red" else "red"
        t = io.open(f, encoding="utf-8", errors="replace").read()
        rows = {}
        start = {}
        day = 0
        main_jobs = {}
        cap_opt = {}
        for line in t.splitlines():
            dm = re.match(r"DAY (\d+)", line)
            if dm:
                day = int(dm.group(1))
                continue
            tm = re.match(r"\s+TOWNS d(\d+) (.*)", line)
            if tm:
                d = int(tm.group(1))
                for e in tm.group(2).split():
                    pos, owner, gar, seen = e.split(":")
                    r = rows.setdefault(pos, {"first": {}, "gar_seen": {}, "taken": None, "owner0": owner})
                    if d == 1 and owner in ("red", "blue"):
                        start[owner] = tuple(int(x) for x in pos.split(","))
                    for side, c in (("red", "r"), ("blue", "b")):
                        if c in seen and side not in r["first"]:
                            r["first"][side] = d
                            r["gar_seen"][side] = int(gar)
                    if r["taken"] is None and r["owner0"] == "neutral" and owner in ("red", "blue"):
                        r["taken"] = (owner, d, int(gar))
                continue
            om = re.match(r"\s+V2 p\d+ (\S+?)(\*?) (capture) (.+?) at \((\d+) (\d+) (\d+)\)", line)
            if om:
                key = "%s,%s,%s" % om.group(5, 6, 7)
                cap_opt.setdefault(key, day)
            jm = re.match(r"\s+V2 p1 (\S+)\* (\w+) (.+?) at \(", line)
            if jm:
                main_jobs.setdefault(day, "%s %s" % (jm.group(2), jm.group(3)[:28]))
        tag = os.path.basename(f).split("_s5k")[0]
        print("== %s (OmniAI %s, start %s; Nullkiller start %s)" % (tag, me, start.get(me), start.get(nk)))
        for pos, r in sorted(rows.items(), key=lambda kv: (kv[1]["taken"] or ("", 999))[1]):
            if r["owner0"] != "neutral":
                continue
            xyz = tuple(int(x) for x in pos.split(","))
            dist = {}
            for side in ("red", "blue"):
                s = start.get(side)
                dist[side] = max(abs(s[0] - xyz[0]), abs(s[1] - xyz[1])) if s and s[2] == xyz[2] else None
            tk = r["taken"]
            taken = "%s d%d (garrison %d)" % (("OmniAI" if tk[0] == me else "NK"), tk[1], tk[2]) if tk else "-"
            if tk and tk[1] <= last:
                summary[("OmniAI" if tk[0] == me else "NK")] += 1
            print("  %-10s dist ours %-4s NK %-4s | seen ours d%-3s (gar %-5s) NK d%-3s (gar %-5s) | capture option d%-3s | taken %s"
                  % (pos, dist[me], dist[nk], r["first"].get(me, "-"), r["gar_seen"].get(me, "-"), r["first"].get(nk, "-"),
                     r["gar_seen"].get(nk, "-"), cap_opt.get(pos, "-"), taken))
        jobs = Counter(v.split()[0] for d, v in main_jobs.items() if d <= 14)
        print("  OmniAI main hero's first job each day, days 1-14: %s" % ", ".join("%s %d" % kv for kv in jobs.most_common()))
print("== towns taken by day %d over all games: %s" % (last, dict(summary)))
