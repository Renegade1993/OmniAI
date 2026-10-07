"""short_days.py PREFIX [PREFIX ...] [--maps=...] [--seed=s5k] [--max=4]: our hero-days (days 1-42, the m1_gate.py measure) that
moved --max tiles or fewer dawn to dawn, by what the hero did that day: the kinds of its V2 job lines (explore, visit, fight,
town, gather, ...; "none" when it had no job line), main hero or support. October 7th (the tiles bar: 14.6% of AG's hero-days
moved 4 tiles or fewer). Either seat. Read-only."""
import collections
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
maps = opts.get("maps", "01,02,03,04,05,07,10,13,16,19").split(",")
seed = opts.get("seed", "s5k")
mx = int(opts.get("max", "4"))
HX = re.compile(r"(\d+):([^@:]+)@(\d+),(\d+),(\d+):(\d+):\d+(?::\d+:\d+)?")
JOB = re.compile(r"V2 p\d+ (.+?)(\*?) (\w+) .* at \(")
count = collections.Counter()
tot = collections.Counter()
for pre in args:
    for m in maps:
        logs = sorted(glob.glob(os.path.join(HERE, "runs", "%s%s_%s_r0[12]_decisions-*.log" % (pre, m, seed))))
        if not logs:
            continue
        me = "blue" if logs[0].endswith("-blue.log") else "red"
        t = io.open(logs[0], encoding="utf-8", errors="replace").read()
        if not re.search(r"TRUTH d7 ", t):
            continue
        # per day: our heroes' positions at dawn (HEROES line), their job kinds that day, the main hero
        days = {}
        cur = None
        for line in t.split("\n"):
            mm = re.match(r"DAY (\d+)", line)
            if mm:
                cur = days.setdefault(int(mm.group(1)), {"pos": {}, "jobs": collections.defaultdict(set), "main": None})
                continue
            if cur is None:
                continue
            mm = re.search(r"HEROES d\d+ red (.*?) \| blue (.*?) \|", line)
            if mm:
                part = mm.group(1) if me == "red" else mm.group(2)
                cur["pos"] = {h.group(2).strip(): (int(h.group(3)), int(h.group(4)), int(h.group(5))) for h in HX.finditer(part)}
                continue
            mm = JOB.search(line)
            if mm:
                cur["jobs"][mm.group(1).strip()].add(mm.group(3))
                if mm.group(2):
                    cur["main"] = mm.group(1).strip()
        for d in sorted(days):
            if d > 42 or d + 1 not in days:
                continue
            a, b = days[d]["pos"], days[d + 1]["pos"]
            for name, p in a.items():
                q = b.get(name)
                if not q or q[2] != p[2]:
                    continue
                role = "main" if name == days[d]["main"] else "support"
                tot[role] += 1
                if max(abs(p[0] - q[0]), abs(p[1] - q[1])) <= mx:
                    kinds = "+".join(sorted(days[d]["jobs"].get(name, {"none"})))
                    count[(role, kinds)] += 1
print("== %s: hero-days moving %d tiles or fewer, by the day's job kinds (main %d hero-days, support %d)" % (" ".join(args), mx, tot["main"], tot["support"]))
for (role, kinds), n in count.most_common(25):
    print("  %-8s %-34s %5d  (%.1f%% of that role's hero-days)" % (role, kinds, n, 100.0 * n / max(1, tot[role])))
