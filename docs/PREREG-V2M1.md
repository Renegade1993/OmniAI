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
