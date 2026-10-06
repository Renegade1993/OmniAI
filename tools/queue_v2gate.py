"""queue_v2gate.py DLL [maps] [days=60]: queue the M1 gate of PREREG-V2M1.md for a v2 build: ten pilot maps on seedbases
6000, 6002, 6004 (seeds 6001, 6003, 6005), tags V2M1Gnn_s6k, V2M1G2nn_s6k, V2M1G3nn_s6k, side files beside them, 3600 s
cap. October 5th. Appends to runs/queue.txt; refuses if any gate tag already has an .out (a gate is never replayed on the
same seeds)."""
import io, os, sys
runs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs")
dll = sys.argv[1]
maps = (sys.argv[2] if len(sys.argv) > 2 else "01,02,03,04,05,07,10,13,16,19").split(",")
days = sys.argv[3] if len(sys.argv) > 3 else "60"
if not os.path.exists(os.path.join(runs, "..", "dlls", dll + ".dll")):
    sys.exit("no such dll " + dll)
sets = (("V2M1G", "6000"), ("V2M1G2", "6002"), ("V2M1G3", "6004"))
for pre, _ in sets:
    for m in maps:
        if os.path.exists(os.path.join(runs, "%s%s.out" % (pre, m))):
            sys.exit("gate tag %s%s already played; the gate is replayed on new seeds only" % (pre, m))
lines = ["# M1 gate (PREREG-V2M1.md, amendment 1) with %s: maps %s, day cap %s, seedbases 6000/6002/6004" % (dll, ",".join(maps), days)]
for m in maps:          # map-major, so an early stop leaves whole maps rather than whole seeds
    for pre, sb in sets:
        t = "%s%s_s6k" % (pre, m)
        mp = open(os.path.join(runs, "SW2S4TN%s_s5k.map" % m)).read().strip()
        open(os.path.join(runs, t + ".map"), "w").write(mp + "\n")
        open(os.path.join(runs, t + ".seconds"), "w").write("3600\n")
        open(os.path.join(runs, t + ".seedbase"), "w").write(sb + "\n")
        lines.append("%s:%s:off:runs/%s%s.out:1:%s" % (t, dll, pre, m, days))
q = os.path.join(runs, "queue.txt")
txt = io.open(q, encoding="utf-8").read()
if not txt.endswith("\n"):
    txt += "\n"
io.open(q, "w", encoding="utf-8", newline="").write(txt + "\n".join(lines) + "\n")
print("queued", len(lines) - 1)
