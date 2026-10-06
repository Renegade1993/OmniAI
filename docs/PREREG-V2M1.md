# PREREG-V2M1: the v2 engine's execution gate (milestone M1)

Written by OmniAI Dev on October 4th, 2026 (19:10 by the clock), before any game of this batch. Megaplan-OmniAI-2026-10-04.md, section 4.8, milestone M1: "the loop, evaluator, planner and mover. Gate on counts against Nullkiller, not on late shares".

## Build
The newest v2 engine cut that has passed a ten-map development smoke without a crash, named in an amendment below before its first gate game (DLL name and the engine-v2 commit). Development smokes use seedbase 5000; nothing on seeds 6001 and up has been played by any v2 build.

## Games
Ten pilot maps (01 02 03 04 05 07 10 13 16 19), seeds 6001, 6003, 6005 (seedbase 6000, 6002, 6004), 30 games, against stock Nullkiller, reveal off, knight tier both sides, 3600 s cap, day cap 60 (the counts below are read by day 56). Tags V2M1Gnn_s6k, V2M1G2nn_s6k, V2M1G3nn_s6k. Lanes sized to free commit (TESTING.md rule 10), feeders with a stop time, registered.

## Measures (tools: v2_sum.py, v2_idle.py, mine_days.py, towns_seen.py, hero_travel.py)
1. Tiles moved per hero-day, ours, days 1 to 42 (dawn-to-dawn displacement from the HEROES lines).
2. Share of hero-days that end on the starting tile, ours, days 1 to 42.
3. Mines owned on day 14, ours over Nullkiller's (median over games).
4. Neutral towns taken by day 56, ours over Nullkiller's (sum over the 30 games).
5. Reported, not gated: army share of Nullkiller's on days 7, 14, 21, 28, 42, 56; buildings; best hero level; fights won and lost; games lost before day 60.

## Gate (all four)
- Tiles per hero-day at least 10.
- Hero-days on the starting tile under 5 percent.
- Mines on day 14 at least 0.85 of Nullkiller's.
- Neutral towns taken by day 56 at least 0.6 of Nullkiller's count.

## Reading rules
- Pass: all four met. Milestone M2 (economy and expansion) starts; M2's gate is army share at least 0.95 on days 28 and 56 over 40 paired games on fresh seeds.
- Fail: any one missed. The missing count is traced in the logs to its cause, fixed, and the gate is played again on new seeds (6007, 6009, 6011), never on these.
- Stop rule (section 4.8 point 6): if after M2's build cycles the M1 counts are still missed by more than a third of their bar, the plan stops and asks K whether to adopt Nullkiller's execution core for that layer.
- Voided games (crash, restart, undecided far short of the cap, ERROR) are replayed and listed. No release, no push, nothing accepted without K.

## Amendment 1 (October 5th, 11:20 by the clock, before any gate game)
Build: V2M1T_7DB78C21, engine-v2 commit 07a3fe9 (local, not pushed). It passed the ten-map development smoke V2M1TSM (seedbase 5000, 45 days) with no crash: 9 undecided at day 46 to 47, 1 lost (map 13, day 16). Its smoke counts, for the record and not part of the gate: tiles per hero-day 9.0 (Nullkiller's own 10.2), hero-days on the starting tile 2%, mines day 14 6.0/6.0, towns day 42 2.0/2.0 median. Measures 1 and 2 are read with travel_both.py (same displacement rule as v2_sum.py, and Nullkiller's own figure beside ours); v2_sum.py and v2_idle.py take --seed=s6k. Queue tool queue_v2gate.py writes the 30 items with day cap 60 and the 3600 s cap. Lanes 3-11, feeders of 10:27 and 10:59 (stop 23:00).

## Result (October 5th, 11:47 by the clock; m1_gate.py, saved as runs\M1GATE_V2M1T.txt)
FAIL, two of four. 30 of 30 games played, none voided (27 undecided at day 61 to 62, 3 lost: V2M1G213 day 35, V2M1G301 day 41, V2M1G313 day 13).
- 1. Tiles per hero-day, days 1-42: 9.0 (bar at least 10). MISSED. Nullkiller's own, same rule: 9.7.
- 2. Hero-days on the starting tile: 3.1% (bar under 5%). MET. Nullkiller 0.7%.
- 3. Mines on day 14, ours over Nullkiller's, median over 29 games: 1.00 (bar at least 0.85). MET.
- 4. Neutral towns taken by day 56: ours 44, Nullkiller 88, ratio 0.50 (bar at least 0.6). MISSED.
- Reported: army share of Nullkiller's, median, days 7/14/21/28/42/56: 0.92/1.02/0.74/0.68/0.77/0.69. Won 0, lost 3, undecided 27. Fights won 720, lost 68.
Reading rule applied: the two missed counts are traced, fixed, and the gate is played again on seeds 6007, 6009, 6011 (seedbases 6006, 6008, 6010). Both misses are within a third of their bar (9.0 against 6.7; 0.50 against 0.40), so the stop rule is not reached.
Note (11:50): this gate was played out of order. The lead's order of about 11:18 (no gate until a ten-map smoke meets all four bars) was unread in OmniAI Dev's mailbox when the gate was queued at 11:20. The result stands as recorded; the next gate is played only after a smoke meets all four bars, on seeds 6007, 6009, 6011.
