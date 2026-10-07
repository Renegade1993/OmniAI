"""lane_spread.py: omni_spread.py for a second isolated install (a "lane"), September 26th.

Written by make_lane_spread.py from omni_spread.py. K: "this lane is the perpetual laggard". One
game uses about 3.2 of the machine's 24 cores, and the watch copy (VCMI/watch) runs the identical
engine, Nullkiller, BattleAI, duel-v2 map and nkai-settings (sha256 checked), so it can carry a
test beside the main chain. Everything is as in omni_spread.py except: --lane picks the install;
the ai_match snapshot has that install's PATCHED_DIR, client exe, USER_DIR and DECISIONS written
in; the engine check waits only for that lane's own client; the MapGen engine window (the GAP
files) belongs to the main install and is left out. runs/STOP stops every lane.


omni_spread.py - run N headless OmniAI-vs-Nullkiller matches and KEEP each one.

ai_repeat.py prints a summary and throws the per-match decision logs away (the
next launch deletes them). The TRUTH telemetry lives in those logs, so this
driver copies decisions-*.log and the result JSON out after every match.

    py omni_spread.py --map Maps/AIBench/duel-v2 --runs 20 --tag A_duel
    py omni_spread.py --map Maps/Twins --runs 6 --seconds 1800 --tag A_twins --swap

Kill switches: a STOP file in the output folder ends the run between matches;
--max-hours caps total wall time; every match has ai_match.py's own deadline.
Runs hidden: ai_match launches the client on an invisible desktop.
"""
import argparse, glob, io, json, os, shutil, subprocess, sys, time

# ai_match.py as the chain runs it: a snapshot taken at the start of each
# spread into the spread's own output folder (runs/<tag>.ai_match.py), so an
# edit to the Tools copy mid-spread cannot split one measurement across two
# harness versions. It was a hand-kept copy in %TEMP% until September 25th;
# K's isolation rule keeps every tool inside the project folder.
TOOLS_SOURCE = r"C:\AI Projects\Heroes 3\! LLM Files\Tools\ai_match.py"
TOOLS = TOOLS_SOURCE
DECISIONS = None   # the lane's own OMNIAI_DIR, set from --lane
LANES = {
    "watch": (r"C:\AI Projects\Heroes 3\VCMI\watch", "VCMI_watch.exe"),
    "lane3": (r"C:\AI Projects\Heroes 3\VCMI\lane3", "VCMI_lane3.exe"),
    "lane4": (r"C:\AI Projects\Heroes 3\VCMI\lane4", "VCMI_lane4.exe"),
    "lane5": (r"C:\AI Projects\Heroes 3\VCMI\lane5", "VCMI_lane5.exe"),
    "lane6": (r"C:\AI Projects\Heroes 3\VCMI\lane6", "VCMI_lane6.exe"),
    "lane7": (r"C:\AI Projects\Heroes 3\VCMI\lane7", "VCMI_lane7.exe"),
    "lane8": (r"C:\AI Projects\Heroes 3\VCMI\lane8", "VCMI_lane8.exe"),
    "lane9": (r"C:\AI Projects\Heroes 3\VCMI\lane9", "VCMI_lane9.exe"),
    "lane10": (r"C:\AI Projects\Heroes 3\VCMI\lane10", "VCMI_lane10.exe"),
    "lane11": (r"C:\AI Projects\Heroes 3\VCMI\lane11", "VCMI_lane11.exe"),
}
LANE_EXE = None
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
OPUS = r"C:\AI Projects\Heroes 3\.tmp\opus"
GAP_OPEN = os.path.join(OPUS, "GAP_OPEN")
GAP_WANTED = os.path.join(OPUS, "GAP_WANTED")


def client_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s" % LANE_EXE, "/NH"],
                             capture_output=True, text=True, creationflags=NO_WINDOW).stdout
        return LANE_EXE in out
    except Exception:
        return False


def lane_snapshot(lane, dst):
    """ai_match.py with the lane's install written in place of the test install's."""
    lane_dir, exe = LANES[lane]
    text = io.open(TOOLS_SOURCE, encoding="utf-8").read()
    pairs = [
        ('PATCHED_DIR = r"C:\\AI Projects\\Heroes 3\\VCMI\\testinstall"', 'PATCHED_DIR = r"%s"' % lane_dir),
        ('CLIENT = os.path.join(PATCHED_DIR, "VCMI_client.exe")', 'CLIENT = os.path.join(PATCHED_DIR, "%s")' % exe),
        ('USER_DIR = r"C:\\AI Projects\\Heroes 3\\VCMI\\testinstall\\userdata"', 'USER_DIR = r"%s\\userdata"' % lane_dir),
        ('DECISIONS = r"C:\\AI Projects\\Heroes 3\\VCMI\\testinstall\\userdata\\OmniAI"', 'DECISIONS = r"%s\\userdata\\OmniAI"' % lane_dir),
    ]
    for a, b in pairs:
        if text.count(a) != 1:
            raise SystemExit("ai_match.py changed: %r found %d times; lane snapshot not written" % (a, text.count(a)))
        text = text.replace(a, b)
    # The day cap (September 26th, tested in .tmp/omni/daycap): an optional --max-day N; 0, the
    # default, changes nothing. Added to the lane's snapshot only, never to the shared file.
    if "--max-day" not in text:
        cap = [
            ('    ap.add_argument("--seconds", type=int, default=1800)\n',
             '    ap.add_argument("--seconds", type=int, default=1800)\n'
             '    ap.add_argument("--max-day", type=int, default=0)\n'),
            ('def analyse(red_ai, blue_ai):',
             'def _reached_day():\n'
             '    best = 0\n'
             '    for f in glob.glob(os.path.join(DECISIONS, "decisions-*.log")):\n'
             '        try:\n'
             '            with open(f, "rb") as fh:\n'
             '                fh.seek(0, 2)\n'
             '                fh.seek(max(0, fh.tell() - 65536))\n'
             '                tail = fh.read().decode("utf-8", "replace")\n'
             '        except OSError:\n'
             '            continue\n'
             '        days = re.findall(r"^DAY (\\d+) ", tail, re.M)\n'
             '        if days:\n'
             '            best = max(best, int(days[-1]))\n'
             '    return best\n\n\n'
             'def analyse(red_ai, blue_ai):'),
            ('                if proc.poll() is not None:\n'
             '                    break\n'
             '                time.sleep(1.0)',
             '                if proc.poll() is not None:\n'
             '                    break\n'
             '                if args.max_day and _reached_day() > args.max_day:\n'
             '                    print("[match] day %d started: stopping at the day cap %d" % (_reached_day(), args.max_day))\n'
             '                    break\n'
             '                if time.time() > deadline - args.seconds + 240 and _reached_day() == 0:\n'
             '                    print("[match] no day started in 240 s: stopping (a start-up stall)")\n'
             '                    break\n'
             '                time.sleep(1.0)'),
        ]
        for a, b in cap:
            if text.count(a) != 1:
                raise SystemExit("ai_match.py changed: the day cap's anchor %r found %d times" % (a[:60], text.count(a)))
            text = text.replace(a, b)
    io.open(dst, "w", encoding="utf-8", newline="").write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="Maps/AIBench/duel-v2")
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--seconds", type=int, default=900)
    ap.add_argument("--swap", action="store_true")
    ap.add_argument("--alternate", action="store_true",
                    help="swap seats on every other run (50/50 seating)")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs"))
    ap.add_argument("--max-hours", type=float, default=4.0)
    ap.add_argument("--seed-base", type=int, default=0,
                    help="run i plays with seed base+i (0: the server picks); two builds "
                         "run with the same base meet the same starts, game by game")
    ap.add_argument("--open-gap", action="store_true",
                    help="open an engine window for the MapGen lane before the first match")
    ap.add_argument("--difficulty", type=int, default=-1,
                    help="passed to ai_match: scenario difficulty 0..4, Nullkiller's tier and the "
                         "engine's resource cheats (equal for both AIs); -1 keeps the scenario's own")
    ap.add_argument("--omni-difficulty", type=int, default=-1,
                    help="passed to ai_match: OmniAI's own profile 0..4; -1 follows the scenario")
    ap.add_argument("--max-day", type=int, default=0,
                    help="end each game once day N+1 starts (0: play it out)")
    ap.add_argument("--lane", required=True, choices=sorted(LANES),
                    help="the isolated install to play in (VCMI/<lane>)")
    ap.add_argument("--first-run", type=int, default=1,
                    help="start at run N to finish a spread that was stopped: the same seeds, "
                         "seats and file names, results appended to the tag's .jsonl, and the "
                         "spread's own ai_match snapshot kept so one measurement uses one harness")
    args = ap.parse_args()
    global DECISIONS, LANE_EXE
    lane_dir, LANE_EXE = LANES[args.lane]
    DECISIONS = os.path.join(lane_dir, "userdata", "OmniAI")
    _sb = os.path.join(args.out, args.tag + ".seedbase")
    if os.path.exists(_sb):
        args.seed_base = int(io.open(_sb, encoding="utf-8").read().strip())
        print("seed base from %s: %d" % (_sb, args.seed_base))
    _mp = os.path.join(args.out, args.tag + ".map")
    if os.path.exists(_mp):
        args.map = io.open(_mp, encoding="utf-8").read().strip()
        print("map from %s: %s" % (_mp, args.map))
    _sc = os.path.join(args.out, args.tag + ".seconds")
    if os.path.exists(_sc):
        args.seconds = int(io.open(_sc, encoding="utf-8").read().strip())
        print("match cap from %s: %d s" % (_sc, args.seconds))
    # October 7th: <tag>.firstrun plays from run N, so a queue line can ask for the swapped seat alone
    # (--alternate swaps the even runs: first run 2 of 2 is one game with OmniAI blue).
    _fr = os.path.join(args.out, args.tag + ".firstrun")
    if os.path.exists(_fr):
        args.first_run = int(io.open(_fr, encoding="utf-8").read().strip())
        print("first run from %s: %d" % (_fr, args.first_run))
    os.makedirs(OPUS, exist_ok=True)

    os.makedirs(args.out, exist_ok=True)
    stop = os.path.join(args.out, "STOP")
    t0 = time.time()
    results = os.path.join(args.out, args.tag + ".jsonl")
    global TOOLS
    TOOLS = os.path.join(args.out, args.tag + ".ai_match.py")
    if args.first_run > 1 and os.path.exists(TOOLS):
        print("resuming %s at run %d with the spread's own ai_match snapshot" % (args.tag, args.first_run))
    else:
        lane_snapshot(args.lane, TOOLS)
    for i in range(args.first_run, args.runs + 1):
        if os.path.exists(stop):
            print("STOP file present, ending")
            break
        if time.time() - t0 > args.max_hours * 3600:
            print("max-hours reached, ending")
            break
        # Never share the engine: wait out any client that is not ours.
        w0 = time.time()
        while client_running() and time.time() - w0 < 600:
            time.sleep(5)
        swap = args.swap or (args.alternate and i % 2 == 0)
        cmd = [sys.executable, TOOLS, "--map", args.map, "--seconds", str(args.seconds), "--json"]
        if swap:
            cmd.append("--swap")
        if args.seed_base > 0:
            cmd += ["--seed", str(args.seed_base + i)]
        if args.difficulty >= 0:
            cmd += ["--difficulty", str(args.difficulty)]
        if args.omni_difficulty >= 0:
            cmd += ["--omni-difficulty", str(args.omni_difficulty)]
        if args.max_day > 0:
            cmd += ["--max-day", str(args.max_day)]
        p = subprocess.run(cmd, capture_output=True, text=True, creationflags=NO_WINDOW)
        out = p.stdout or ""
        i0 = out.find("{")
        if i0 < 0:
            print("run %d: ERROR %s" % (i, (p.stderr or out)[-200:].replace("\n", " ")))
            sys.stdout.flush()
            continue
        try:
            r = json.loads(out[i0:])
        except Exception as e:
            print("run %d: bad json %s" % (i, e))
            continue
        r["run"] = i
        r["tag"] = args.tag
        r["swap"] = swap
        omni = "blue" if swap else "red"
        r["omni_seat"] = omni
        # The .jsonl twin (OmniAI R7w and later) goes with its log: MapGen's
        # dashboard and the observer read the events from it.
        for f in (glob.glob(os.path.join(DECISIONS, "decisions-*.log"))
                  + glob.glob(os.path.join(DECISIONS, "decisions-*.jsonl"))):
            dst = os.path.join(args.out, "%s_r%02d_%s" % (args.tag, i, os.path.basename(f)))
            try:
                shutil.copy2(f, dst)
            except Exception:
                pass
        with io.open(results, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(r) + "\n")
        st = (r.get("standing") or {}).get(omni) or {}
        if r.get("error"):
            verdict = "ERROR " + r["error"]
        elif r.get("decided"):
            verdict = "winner " + str(r.get("winner_ai"))
        else:
            verdict = "undecided"
        print("run %d (%s): day %-4d %-22s towns %s td %s crash=%s"
              % (i, omni, r.get("days", 0), verdict, st.get("towns"), st.get("town_days"),
                 r.get("crashed")))
        sys.stdout.flush()
    # Cross-game audit (K, September 24th): flag what happened in every game
    # or in none. Bounded and windowless; a failure here never fails the run.
    # Absolute path: three_class.sh runs a copy of this script from %TEMP%.
    audit = r"C:\AI Projects\Heroes 3\.tmp\omni\audit_spread.py"
    try:
        rc = subprocess.run([sys.executable, audit, args.out, args.tag],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300,
                            creationflags=NO_WINDOW).returncode
        print("audit: %s" % (os.path.join(args.out, args.tag + ".audit.txt") if rc == 0
                             else "failed, exit %d" % rc))
    except Exception as exc:
        print("audit failed: %s" % exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
