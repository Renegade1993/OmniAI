"""audit_spread.py <runs folder> <tag> [--history TAG ...] [--src OmniAI.cpp]

Cross-game audit of one spread. K, September 24th: "clearly we need wayyyy
better logging". The raw logging was already there; what was missing is a
pass that looks at the WHOLE spread and flags anything that happens in every
game or in none. A Barracks built 0 times in 32 games went unnoticed by log
reading and by watching, because nothing ever counted it across games.

What it counts, per game, from the saved decision logs and <tag>.jsonl:
  event kinds   every log line reduced to its keyword ("ESCAPE", "RAID
                toward their capital", "cannot afford", "<N> spent movement
                without leaving"): which fire in 0% or 100% of games, which
                fired in older spreads and stopped, and which ALL-CAPS
                keywords exist in OmniAI.cpp but were never logged anywhere
  buildings     our BUILD lines against the standard building IDs, with the
                usual first-build order
  army tiers    which creature tiers our field hero ever carried (GROW
                field=), next to the biggest enemy army we saw (tier=)
  hero levels   our best level at the end against Nullkiller's (TRUTH maxL)
  TRUTH fields  our final-day towns, buildings, fort, heroes, dwellings,
                mines, manned; any field that is zero or identical in every
                game

Output goes to stdout and to <folder>/<tag>.audit.txt. Read-only otherwise.
Flags are facts, not verdicts: a line at 100% may be a real constant of the
map (one town each on duel-v2), and the reader decides which.
"""
import collections, glob, io, json, os, re, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.normpath(os.path.join(HERE, "..", "..", "OmniAI", "OmniAI.cpp"))

BUILDING = {
    0: "Mage Guild 1", 1: "Mage Guild 2", 2: "Mage Guild 3", 3: "Mage Guild 4", 4: "Mage Guild 5",
    5: "Tavern", 6: "Shipyard", 7: "Fort", 8: "Citadel", 9: "Castle",
    10: "Village Hall", 11: "Town Hall", 12: "City Hall", 13: "Capitol", 14: "Marketplace",
    15: "Resource Silo", 16: "Blacksmith", 17: "Special 1", 18: "Horde 1", 19: "Horde 1 up",
    20: "Ship", 21: "Special 2", 22: "Special 3", 23: "Special 4", 24: "Horde 2",
    25: "Horde 2 up", 26: "Grail",
}
for _l in range(7):
    BUILDING[30 + _l] = "Dwelling %d" % (_l + 1)
    BUILDING[37 + _l] = "Dwelling %d up" % (_l + 1)
# The ones a Castle-type town can buy on duel-v2. Shipyard, Ship, Grail,
# Special 4 and Horde 2 exist only for some factions or maps, so their
# absence is not flagged; they still print if any game built them.
EXPECTED = [i for i in BUILDING if i not in (6, 20, 23, 24, 25, 26)]

NAME = re.compile(r"^[A-Z][a-z][A-Za-z'\-]*[,:]?$")
CAPS = re.compile(r"^[A-Z]{2,}$")
LOWER = re.compile(r"^[a-z][a-z'\-]*$")
BUILD = re.compile(r"^\s*BUILD .*?: building (\d+)")
TRUTH = re.compile(r"TRUTH d(\d+) (\w+)(\*?) (.*)")
GROW = re.compile(r"GROW d(\d+) field=([\d/]+|-) \| foe=\d+ tier=([\d/]+|-)")
FIELDS = ("towns", "bld", "fort", "heroes", "dw", "mines", "maxL", "manned")


def kind_of(line):
    """The keyword a log line starts with, names and numbers stripped."""
    s = line.strip()
    if not s or s[0] in "|=~-(#[<>*":
        return None
    toks = s.split()
    out = []
    i = 0
    if NAME.match(toks[0]):
        # A line that opens on a hero, town or object name: "Adela spent
        # movement without leaving". Collapse the name, keep the verb.
        while i < len(toks) and NAME.match(toks[i]):
            i += 1
        out.append("<N>")
    for t in toks[i:]:
        bare = t.rstrip(",:;.!")
        if not (CAPS.match(bare) or LOWER.match(bare)):
            break
        out.append(bare)
        if bare != t or len(out) >= 6:
            break
    if not out or out == ["<N>"]:
        return None
    return " ".join(out)


def source_keywords(src):
    """ALL-CAPS words that open a string literal in the AI source: the
    vocabulary of decision lines it can print."""
    if not os.path.exists(src):
        return set()
    text = io.open(src, encoding="utf-8", errors="replace").read()
    words = set()
    for m in re.finditer(r'"\s*([A-Z]{3,})[ :"]', text):
        w = m.group(1)
        if "_" not in w:
            words.add(w)
    return words


HISTORY_CACHE = {}


def history_game(folder, path):
    """parse_game's kinds and builds for a history game, cached in one file, <folder>/.audit_history.json, keyed by the
    log's name and modification time. October 6th: re-parsing every older log on every call (5.8 GB by then) ran past
    the 300 s bound lane_spread.py gives the audit, after every game, and the audit wrote nothing; a cache file per log
    still took 400 s to open. Lanes that write at the same moment can lose each other's newest entries, which are
    parsed again next time."""
    if not HISTORY_CACHE:
        try:
            HISTORY_CACHE.update(json.load(io.open(os.path.join(folder, ".audit_history.json"), encoding="utf-8")))
        except Exception:
            HISTORY_CACHE["_"] = 0
    key = os.path.basename(path)
    mtime = os.path.getmtime(path)
    c = HISTORY_CACHE.get(key)
    if isinstance(c, list) and c[0] == mtime:
        return dict(kinds=collections.Counter(c[1]), builds=[tuple(x) for x in c[2]])
    g = parse_game(path)
    HISTORY_CACHE[key] = [mtime, dict(g["kinds"]), g["builds"]]
    HISTORY_CACHE["_dirty"] = 1
    return g


def save_history_cache(folder):
    if not HISTORY_CACHE.pop("_dirty", 0):
        return
    try:
        dst = os.path.join(folder, ".audit_history.json")
        tmp = dst + ".%d.tmp" % os.getpid()
        json.dump(HISTORY_CACHE, io.open(tmp, "w", encoding="utf-8"))
        os.replace(tmp, dst)
    except Exception:
        pass


def parse_game(path):
    g = dict(kinds=collections.Counter(), builds=[], tiers=set(), foe_tiers=set(),
             ours={}, nk={}, last_day=0, peakL=0, nk_peakL=0,
             first_us={}, first_nk={})
    day = 0
    for ln in io.open(path, encoding="utf-8", errors="replace"):
        if ln.startswith("DAY "):
            try:
                day = int(ln.split()[1])
            except ValueError:
                pass
            g["last_day"] = day
        k = kind_of(ln)
        if k:
            g["kinds"][k] += 1
        m = BUILD.match(ln)
        if m:
            g["builds"].append((day, int(m.group(1))))
            continue
        m = GROW.search(ln)
        if m:
            for key, grp in (("tiers", 2), ("foe_tiers", 3)):
                if m.group(grp) != "-":
                    for t, n in enumerate(m.group(grp).split("/"), 1):
                        if int(n) > 0:
                            g[key].add(t)
            continue
        m = TRUTH.search(ln)
        if m:
            vals = dict(re.findall(r"(\w+)=(-?\d+)", m.group(4)))
            side = "ours" if m.group(3) else "nk"
            g[side] = {f: int(vals[f]) for f in FIELDS if f in vals}
            if "maxL" in vals:
                key = "peakL" if side == "ours" else "nk_peakL"
                g[key] = max(g[key], int(vals["maxL"]))
            # "built: Town+11 Other+7+8" names what each side raised since the
            # last pass; the first day each id appears is when it went up.
            b = re.search(r"built: (.*)$", m.group(4))
            if b:
                first = g["first_us" if side == "ours" else "first_nk"]
                d = int(m.group(1))
                for _town, ids in re.findall(r"(\S+?)\+([\d+,]+)", b.group(1)):
                    for x in re.split(r"[+,]", ids):
                        if x.isdigit():
                            first.setdefault(int(x), d)
    return g


def pct(n, d):
    return "%3d%%" % round(100.0 * n / d) if d else "  -"


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2
    folder, tag = args[0], args[1]
    history, src = None, DEFAULT_SRC
    rest = args[2:]
    while rest:
        a = rest.pop(0)
        if a == "--history":
            history = []
            while rest and not rest[0].startswith("--"):
                history.append(rest.pop(0))
        elif a == "--src":
            src = rest.pop(0)

    logs = sorted(glob.glob(os.path.join(folder, tag + "_r*_decisions-*.log")))
    if not logs:
        print("no decision logs for tag %s in %s" % (tag, folder))
        return 1
    games = [parse_game(p) for p in logs]
    n = len(games)
    results = []
    jl = os.path.join(folder, tag + ".jsonl")
    if os.path.exists(jl):
        results = [json.loads(l) for l in io.open(jl, encoding="utf-8") if l.strip()]

    # Older spreads, for "used to fire, stopped". Every other tag in the
    # folder unless --history names them.
    # One listing of the folder, grouped by tag (a glob per tag listed the folder 1600 times).
    by_tag = collections.defaultdict(list)
    for p in glob.glob(os.path.join(folder, "*_r[0-9][0-9]_decisions-*.log")):
        by_tag[re.sub(r"_r\d\d_decisions-.*$", "", os.path.basename(p))].append(p)
    if history is None:
        history = sorted(t for t in by_tag if t != tag)
    hist_games = 0
    hist_kinds = collections.Counter()
    hist_builds = collections.Counter()
    for t in history:
        for p in sorted(by_tag.get(t, [])):
            hg = history_game(folder, p)
            hist_games += 1
            for k in hg["kinds"]:
                hist_kinds[k] += 1
            for b in set(b for _, b in hg["builds"]):
                hist_builds[b] += 1
    save_history_cache(folder)

    out = []
    w = out.append
    wins = sum(1 for r in results if r.get("winner_ai") == "OmniAI")
    days = [r.get("days", 0) for r in results]
    w("AUDIT %s: %d games with logs, %d results, OmniAI won %d, median last day %s"
      % (tag, n, len(results), wins, statistics.median(days) if days else "-"))
    w("History for 'stopped firing': %d games across %d other tags." % (hist_games, len(history)))
    w("")

    # Event kinds.
    present = collections.Counter()
    per_game_counts = collections.defaultdict(list)
    for g in games:
        for k, c in g["kinds"].items():
            present[k] += 1
            per_game_counts[k].append(c)
    always = sorted(k for k, c in present.items() if c == n)
    w("== Fired in EVERY game (%d kinds; median count per game) ==" % len(always))
    line = []
    for k in always:
        line.append("%s x%g" % (k, statistics.median(per_game_counts[k])))
    w("  " + "; ".join(line))
    w("")
    stopped = sorted(((hist_kinds[k] / hist_games, k) for k in hist_kinds
                      if k not in present and hist_games and hist_kinds[k] / hist_games >= 0.25),
                     reverse=True)
    w("== Fired in 25%%+ of older games, NEVER in this spread (%d) ==" % len(stopped))
    for share, k in stopped:
        w("  %-60s history %s" % (k, pct(round(share * hist_games), hist_games)))
    w("")
    seen_words = set(t for k in list(present) + list(hist_kinds) for t in k.split())
    never_src = sorted(sw for sw in source_keywords(src) if sw not in seen_words)
    fresh = sorted(k for k in present if k not in hist_kinds)
    w("== New in this spread, never in older games (%d) ==" % len(fresh))
    for k in fresh:
        w("  %-60s %s of games" % (k, pct(present[k], n)))
    w("")
    w("== ALL-CAPS keywords in %s never logged in ANY spread in this folder (%d) =="
      % (os.path.basename(src), len(never_src)))
    w("  " + ", ".join(never_src))
    w("  (a keyword here is either dead code, a rare branch, or not a decision line)")
    w("")

    # Buildings.
    built_games = collections.Counter()
    first_day = collections.defaultdict(list)
    orders = collections.Counter()
    for g in games:
        seen = {}
        for d, b in g["builds"]:
            seen.setdefault(b, d)
        for b, d in seen.items():
            built_games[b] += 1
            first_day[b].append(d)
        orders[tuple(b for _, b in g["builds"][:5])] += 1
    w("== Our buildings: share of games that built each, median first day ==")
    ids = sorted(set(EXPECTED) | set(built_games))
    rows = []
    for b in ids:
        c = built_games.get(b, 0)
        flag = "  NEVER" if c == 0 else ("  ALWAYS" if c == n else "")
        if c == 0 and hist_builds.get(b):
            flag += " (built in %s of older games)" % pct(hist_builds[b], hist_games).strip()
        rows.append("  %2d %-16s %s  day %-5s%s" % (
            b, BUILDING.get(b, "id %d" % b), pct(c, n),
            statistics.median(first_day[b]) if first_day[b] else "-", flag))
    out.extend(rows)
    w("  first five builds, commonest orders:")
    for seq, c in orders.most_common(3):
        w("    %d games: %s" % (c, " > ".join(BUILDING.get(b, str(b)) for b in seq)))
    w("")

    # K's targets (September 24th, via the lead): tier-7 creature generation
    # by the end of week 3 on reasonably resourced maps, and the Capitol
    # early. Level-7 dwelling ids: 36 base, 43 upgraded, 50/57/64/71 the
    # further upgrades some factions have.
    T7 = (36, 43, 50, 57, 64, 71)
    def target(key, ids):
        days = []
        for g in games:
            hit = [g[key][i] for i in ids if i in g[key]]
            days.append(min(hit) if hit else None)
        got = [d for d in days if d is not None]
        by21 = sum(1 for d in got if d <= 21)
        return "%d of %d games (by day 21: %d), first day median %s" % (
            len(got), n, by21, statistics.median(got) if got else "-")
    w("== K's targets: tier-7 dwelling by day 21, Capitol early ==")
    w("  tier-7 dwelling  ours %s" % target("first_us", T7))
    w("                   Nullkiller %s" % target("first_nk", T7))
    w("  Capitol          ours %s" % target("first_us", (13,)))
    w("                   Nullkiller %s" % target("first_nk", (13,)))
    w("  City Hall        ours %s" % target("first_us", (12,)))
    w("                   Nullkiller %s" % target("first_nk", (12,)))
    w("")

    # Army tiers.
    w("== Creature tiers our field hero ever carried, vs the biggest enemy army we saw ==")
    for t in range(1, 8):
        ours = sum(1 for g in games if t in g["tiers"])
        foe = sum(1 for g in games if t in g["foe_tiers"])
        flag = "  NEVER" if ours == 0 else ("  ALWAYS" if ours == n else "")
        w("  tier %d  ours %s   enemy %s%s" % (t, pct(ours, n), pct(foe, n), flag))
    w("")

    # Hero levels and TRUTH fields, final day.
    ours_L = [g["ours"].get("maxL") for g in games if g["ours"].get("maxL") is not None]
    nk_L = [g["nk"].get("maxL") for g in games if g["nk"].get("maxL") is not None]
    pairs = [(g["ours"]["maxL"], g["nk"]["maxL"]) for g in games
             if "maxL" in g["ours"] and "maxL" in g["nk"]]
    ge = sum(1 for a, b in pairs if a >= b)
    w("== Best hero level: peak during the game, and on the last logged day ==")
    pk = [(g["peakL"], g["nk_peakL"]) for g in games if g["peakL"] and g["nk_peakL"]]
    if pk:
        a = [x for x, _ in pk]
        b = [y for _, y in pk]
        ge_pk = sum(1 for x, y in pk if x >= y)
        w("  peak: ours median %g (%d..%d), Nullkiller %g (%d..%d); ours at or above theirs in %s of games%s"
          % (statistics.median(a), min(a), max(a), statistics.median(b), min(b), max(b),
             pct(ge_pk, len(pk)).strip(), "  NEVER" if ge_pk == 0 else ("  ALWAYS" if ge_pk == len(pk) else "")))
    if ours_L and nk_L:
        w("  last day: ours median %g (min %d, max %d); Nullkiller median %g (min %d, max %d)"
          % (statistics.median(ours_L), min(ours_L), max(ours_L),
             statistics.median(nk_L), min(nk_L), max(nk_L)))
        w("  ours at or above theirs in %s of games%s" % (
            pct(ge, len(pairs)).strip(), "  NEVER" if ge == 0 else ("  ALWAYS" if ge == len(pairs) else "")))
    w("")
    w("== Final-day TRUTH fields: ours vs Nullkiller (median); flagged if ours never varies,")
    w("   or if Nullkiller's median is at least three times ours ==")
    for f in FIELDS:
        ov = [g["ours"][f] for g in games if f in g["ours"]]
        nv = [g["nk"][f] for g in games if f in g["nk"]]
        if not ov:
            continue
        flag = ""
        if max(ov) == 0:
            flag = "  ZERO in every game"
        elif len(set(ov)) == 1:
            flag = "  SAME (%d) in every game" % ov[0]
        if nv and statistics.median(nv) > 0 and statistics.median(nv) >= 3 * statistics.median(ov):
            flag += "  FAR BELOW Nullkiller"
        w("  %-7s ours %-6g (%d..%d)   NK %-6s%s" % (
            f, statistics.median(ov), min(ov), max(ov),
            ("%g" % statistics.median(nv)) if nv else "-", flag))

    text = "\n".join(out)
    print(text)
    io.open(os.path.join(folder, tag + ".audit.txt"), "w", encoding="utf-8").write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
