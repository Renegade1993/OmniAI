"""switch_trace.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k] [--examples=N] [--cause=TEXT]: why v2 heroes drop a job they have not
reached, ranked by hero-days lost. October 5th (lead: trace the 52% of far jobs dropped halfway). Needs the V2 SWITCH lines
(V2M1AC and later). Read-only.

Each SWITCH line is joined to the hero's previous job line (the dropped job: kind, tile, d = days away when chosen, day
and pass) and to the new job line after it. The cause is the SWITCH reason, refined:
  gone            the dropped target is no longer offered; split by the dropped kind (explore: the fog moved the frontier;
                  gather: the main hero moved; visit/fight/capture: the object was taken or killed)
  unscorable      still offered, but this hero may no longer take it (fight or guard estimate, value 0, out of reach)
  outscored       still offered and scorable; split by the margin new/old (under 1.5x, 1.5-3x, 3x and over)
Hero-days lost: the tiles the hero had closed on the dropped target and not on the new one, over 9.2 tiles a day (the v2 average). Across days,
from the HEROES dawn positions (day it was chosen to day it was dropped); inside one day, from the drop in d where the
SWITCH line gives it (outscored), else counted as 0 and shown as 'same day, unknown'."""
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
nex = int(opts.get("examples", "3"))
TPD = 9.2
HX = re.compile(r"(\d+):([^@:]+)@(\d+),(\d+),(\d+):\d+:\d+(?::\d+:\d+)?")
JOB = re.compile(r"\s+V2 p(\d+) (.+?)(\*?) (visit|fight|town|gather|explore|hunt|capture) (.*?) at \((\d+) (\d+) (\d+)\) r=(\S+) loss=\S+ d=([\d.]+) s=(\S+)")
SW = re.compile(r"\s+V2 SWITCH (.+?) from \((\d+) (\d+) (\d+)\) to \((\d+) (\d+) (\d+)\): (.*)")


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1])) if a[2] == b[2] else None


rows = []   # (cause, lost, sameday, example)
for pre in args:
    for m in maps:
        f = os.path.join(HERE, "runs", "%s%s_%s_r01_decisions-red.log" % (pre, m, seed))
        if not os.path.exists(f):
            continue
        day = 0
        dawn = {}          # day -> name -> pos
        job = {}           # name -> dict of the hero's current job
        pending = None     # a SWITCH waiting for its new job line
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
            mm = SW.match(line)
            if mm:
                pending = (mm.group(1).strip(), (int(mm.group(2)), int(mm.group(3)), int(mm.group(4))), mm.group(8).strip())
                continue
            mm = JOB.match(line)
            if not mm:
                continue
            p, name, kind = int(mm.group(1)), mm.group(2).strip(), mm.group(4)
            tile = (int(mm.group(6)), int(mm.group(7)), int(mm.group(8)))
            new = {"kind": kind, "what": mm.group(5), "tile": tile, "d": float(mm.group(10)), "day": day, "pass": p,
                   "r": mm.group(9), "s": mm.group(11)}
            if pending and pending[0] == name:
                old = job.get(name)
                reason = pending[2]
                if old and old["tile"] == pending[1]:
                    if reason == "gone":
                        cause = "gone, dropped %s" % old["kind"]
                    elif reason.startswith("unscorable"):
                        cause = "unscorable %s" % old["kind"]
                        if old["kind"] == "town":
                            cause += (" (threat recall)" if old["s"] == "1000000000000" else
                                      " (idle go-home)" if old["r"] == "0" else " (courier/garrison)")
                    else:
                        o = re.match(r"outscored (\d+) < (\d+) \((\w+) d=([\d.]+)\)", reason)
                        ratio = float(o.group(2)) / max(1.0, float(o.group(1))) if o else 0
                        band = "under 1.5x" if ratio < 1.5 else ("1.5-3x" if ratio < 3 else "3x and over")
                        cause = "outscored %s, %s" % (band, old["kind"])
                        if old["s"] == "1000000000000":
                            cause = "outscored, threat recall"
                    lost, same = 0.0, False
                    if day > old["day"] and old["day"] in dawn and day in dawn and name in dawn[old["day"]] and name in dawn[day]:
                        a = cheb(dawn[old["day"]][name], old["tile"])
                        b = cheb(dawn[day][name], old["tile"])
                        a2 = cheb(dawn[old["day"]][name], tile)
                        b2 = cheb(dawn[day][name], tile)
                        if a is not None and b is not None:
                            # progress on the dropped job that did not also close on the new one
                            kept = max(0, a2 - b2) if a2 is not None and b2 is not None else 0
                            lost = max(0, (a - b) - kept) / TPD
                    elif reason.startswith("outscored"):
                        o = re.match(r"outscored \d+ < \d+ \(\w+ d=([\d.]+)\)", reason)
                        near = cheb(old["tile"], tile)
                        if o and (near is None or near > 4):
                            lost = max(0.0, old["d"] - float(o.group(1)))
                    else:
                        same = True
                    ex = "%s%s d%d p%d %s: dropped %s %s at %s (chosen d%d p%d, %.2f days away, r=%s) for %s %s at %s (%.2f days, r=%s); %s" % (
                        pre, m, day, p, name, old["kind"], old["what"], old["tile"], old["day"], old["pass"], old["d"], old["r"],
                        kind, new["what"], tile, new["d"], new["r"], reason)
                    rows.append((cause, lost, same, ex))
                pending = None
            job[name] = new

by = collections.defaultdict(lambda: [0, 0.0, 0, []])
for cause, lost, same, ex in rows:
    b = by[cause]
    b[0] += 1
    b[1] += lost
    b[2] += same
    b[3].append((lost, ex))
total = sum(b[1] for b in by.values())
print("== %s: %d dropped jobs, %.1f hero-days lost in all" % (" ".join(args), len(rows), total))
for cause, b in sorted(by.items(), key=lambda kv: -kv[1][1]):
    print("  %-34s %5d drops  %6.1f hero-days (%4.1f%%)  same day, unknown %d" % (cause, b[0], b[1], 100.0 * b[1] / max(0.01, total), b[2]))
want = opts.get("cause")
if want:
    pool = [(lost, ex) for cause, b in by.items() if want in cause for lost, ex in b[3]]
    label = "causes matching '%s'" % want
else:
    top = max(by.items(), key=lambda kv: kv[1][1]) if by else None
    pool = top[1][3] if top else []
    label = "the top cause (%s)" % (top[0] if top else "-")
if pool and nex:
    print("  examples of %s, largest losses:" % label)
    for lost, ex in sorted(pool, reverse=True)[:nex]:
        print("   %.2f hero-days | %s" % (lost, ex))
