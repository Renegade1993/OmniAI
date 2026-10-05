"""hero_travel.py LABEL=PREFIX,PREFIX ... : dawn-to-dawn travel of our heroes against Nullkiller's, from the HEROES lines.

October 4th (megaplan diagnosis). For each game, each hero id present at two consecutive dawns on the same level: the
Chebyshev distance between the two dawn tiles (a lower bound on the day's walk; teleports and portals count in full). Per band
of days: mean tiles per hero-day, share of hero-days with no movement, number of heroes alive per day, tiles of the army-richest hero. Read-only."""
import glob, io, os, re, statistics, sys
HERE = os.path.dirname(os.path.abspath(__file__))
BANDS = [(1, 14), (15, 28), (29, 56), (57, 84)]
for spec in sys.argv[1:]:
    label, pres = spec.split("=", 1)
    acc = {b: {"red": [], "blue": [], "rn": [], "bn": [], "rmain": [], "bmain": []} for b in BANDS}
    for pre in pres.split(","):
        for f in glob.glob(os.path.join(HERE, "runs", "%s[0-9][0-9]_s5k_r01_decisions-red.log" % pre)):
            prev = {}
            for line in io.open(f, encoding="utf-8", errors="replace"):
                m = re.match(r"\s+HEROES d(\d+) red (.*?) \| blue (.*)", line)
                if not m:
                    continue
                d = int(m.group(1))
                cur = {}
                for side, txt in (("red", m.group(2)), ("blue", m.group(3))):
                    for h in re.finditer(r"(\d+):([^@]+)@(\d+),(\d+),(\d+):(\d+):(\d+)", txt):
                        cur[(side, h.group(1))] = (int(h.group(3)), int(h.group(4)), int(h.group(5)), int(h.group(6)))
                band = next((b for b in BANDS if b[0] <= d <= b[1]), None)
                if band and prev.get("d") == d - 1:
                    for key, pos in cur.items():
                        if key in prev["h"] and prev["h"][key][2] == pos[2]:
                            dist = max(abs(pos[0] - prev["h"][key][0]), abs(pos[1] - prev["h"][key][1]))
                            acc[band][key[0]].append(dist)
                    for side in ("red", "blue"):
                        armies = [(v[3], k) for k, v in cur.items() if k[0] == side]
                        acc[band]["rn" if side == "red" else "bn"].append(len(armies))
                        if armies:
                            main = max(armies)[1]
                            if main in prev["h"] and prev["h"][main][2] == cur[main][2]:
                                dist = max(abs(cur[main][0] - prev["h"][main][0]), abs(cur[main][1] - prev["h"][main][1]))
                                acc[band]["rmain" if side == "red" else "bmain"].append(dist)
                prev = {"d": d, "h": cur}
    print("== %s" % label)
    for b in BANDS:
        a = acc[b]
        def mean(x): return "%.1f" % statistics.mean(x) if x else "-"
        def still(x): return "%.0f%%" % (100.0 * sum(1 for v in x if v == 0) / len(x)) if x else "-"
        print("d%d-%d  tiles per hero-day ours %s / NK %s | still hero-days ours %s / NK %s | army-richest hero tiles per day ours %s / NK %s | heroes ours %s / NK %s" % (
            b[0], b[1], mean(a["red"]), mean(a["blue"]), still(a["red"]), still(a["blue"]), mean(a["rmain"]), mean(a["bmain"]), mean(a["rn"]), mean(a["bn"])))
