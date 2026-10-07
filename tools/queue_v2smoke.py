"""queue_v2smoke.py TAG DLL [maps=05,01,10] [days=45] [seedbase=5000] [seat=red]: queue one development smoke game per map for a v2
engine build, tags <TAG>SM<nn>_s5k, side files beside them. October 4th. Appends to runs/queue.txt.
seat blue (October 7th): OmniAI plays the blue seat (run 2 of 2 with --alternate, <tag>.firstrun 2) on the same game seed as
the red game of that seedbase (the side file holds seedbase - 1); its log is <tag>_r02_decisions-blue.log."""
import io, os, sys
runs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs")
tag, dll = sys.argv[1], sys.argv[2]
maps = (sys.argv[3] if len(sys.argv) > 3 else "05,01,10").split(",")
days = sys.argv[4] if len(sys.argv) > 4 else "45"
seedbase = sys.argv[5] if len(sys.argv) > 5 else "5000"
seat = sys.argv[6] if len(sys.argv) > 6 else "red"
assert seat in ("red", "blue"), seat
if not os.path.exists(os.path.join(runs, "..", "dlls", dll + ".dll")):
    sys.exit("no such dll " + dll)
lines = ["# V2 engine smoke %s (%s): maps %s, %s days, seedbase %s, seat %s" % (tag, dll, ",".join(maps), days, seedbase, seat)]
for m in maps:
    t = "%sSM%s_s5k" % (tag, m)
    mp = open(os.path.join(runs, "SW2S4TN%s_s5k.map" % m)).read().strip()
    open(os.path.join(runs, t + ".map"), "w").write(mp + "\n")
    open(os.path.join(runs, t + ".seconds"), "w").write("3600\n")
    if seat == "blue":
        open(os.path.join(runs, t + ".seedbase"), "w").write("%d\n" % (int(seedbase) - 1))
        open(os.path.join(runs, t + ".firstrun"), "w").write("2\n")
        lines.append("%s:%s:off:runs/%sSM%s.out:2:%s" % (t, dll, tag, m, days))
    else:
        open(os.path.join(runs, t + ".seedbase"), "w").write(seedbase + "\n")
        lines.append("%s:%s:off:runs/%sSM%s.out:1:%s" % (t, dll, tag, m, days))
q = os.path.join(runs, "queue.txt")
txt = io.open(q, encoding="utf-8").read()
if not txt.endswith("\n"):
    txt += "\n"
io.open(q, "w", encoding="utf-8", newline="").write(txt + "\n".join(lines) + "\n")
print("queued", len(lines) - 1)
