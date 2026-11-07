# OmniAI

OmniAI is a computer opponent for Heroes of Might and Magic III. It plays the adventure map: it
builds its towns, recruits and hires heroes, explores, and sends its heroes after the treasure,
mines and towns worth having. Battles themselves are fought by the game's own battle AI.

It weighs every object on the map against the guard in front of it before sending a hero, and with
its optional Learning add-on it remembers between games what things turned out to be worth.

OmniAI is still much weaker than the game's standard computer opponent and loses most games against
it. The current download is version 0.1.0 from September 27th; later work is not released yet.

## Known problems and in construction

Built is approximate: how much of the feature exists and has been seen working in a real game. The
table describes OmniAI as it plays in testing now; the 0.1.0 download is September's build.

| Feature | Status | Built |
|---|---|---|
| Plays fair: sees only what a player sees, gets no bonuses | Working | 80% |
| Exploring, flagging mines and collecting treasure | Working | 70% |
| Judging how strong a guard is before attacking it | Working | 70% |
| Hiring heroes and passing armies between them | Partly built | 60% |
| Building up its towns | Partly built | 50% |
| Explaining its moves in the chat ("AI thoughts") | Partly built | 50% |
| Spending its gold on troops | Known problem | 50% |
| Taking the treasure it fought for | Known problem | 40% |
| Keeping its heroes alive | Known problem | 30% |
| Standing up to a stronger enemy | Known problem | 30% |
| Difficulty levels, Easy to Impossible | Known problem | 30% |
| Learning add-on (remembers between games) | Partly built | 30% |
| Defending its towns | Known problem | 20% |
| Choosing OmniAI for any seat in Dead Man's Boots, a separate Heroes III engine | In construction | 20% |
| Travel spells and long trips | In construction | 10% |
| Playing differently from game to game | In construction | 10% |

What goes wrong today, in plain terms: it leaves gold unspent while troops are for sale, walks away
from treasure it just fought for, loses its weaker heroes early, retreats from a stronger enemy,
leaves its towns empty while enemy heroes are near, and, in the newer engine, plays every difficulty
level the same. The Learning add-on is off by default and has not been shown to make OmniAI
stronger.

## Building

Inside a VCMI 1.7.5 source tree, place this repository at `AI/OmniAI` and add it to
`AI/CMakeLists.txt` (`add_subdirectory(OmniAI)`); it builds with the engine and needs nothing else.

On its own, against a released VCMI 1.7.5 on Windows:

- the VCMI source for its headers, at `../vendor/vcmi` or wherever `-DVCMI_ROOT` points;
- an import library for the released `VCMI_lib.dll` in `../resources`, made once by
  `gen_import_lib.bat` from a Visual Studio developer prompt, with the environment variable
  `VCMI_BIN` naming a VCMI install folder;
- oneTBB and Boost's headers, found through `../resources` or CMake's usual search;
- Visual Studio 2022 Build Tools, CMake and Ninja; `build.bat` configures and builds into `build/`.

The unit tests build as `omniai_tests` in the same tree; `run_tests.ps1` runs them, again with
`VCMI_BIN` set.

## Installing into stock VCMI

OmniAI needs a VCMI 1.7.5 install, and the installer from the Releases page adds it to the VCMI you
already have.

The latest release carries a ready installer: download `OmniAI-0.1.0-installer.zip` from
[Releases](https://github.com/Renegade1993/OmniAI/releases/latest), unzip it, close VCMI and run
`Install OmniAI.bat`. `installer/README.txt` covers the installer, which copies the plugin into a
VCMI install and adds the OmniAI entry to the mod launcher.

## Settings

OmniAI reads these keys from the `"ai"` block of VCMI's `settings.json`, and two environment
variables:

| setting | values | effect |
|---|---|---|
| `omniaiLearningMode` | `"learn"`, `"pause"`, `"off"` | cross-game learning: learn, read the memory without writing it, or off. Unset, learning is on only if `omniaiLearning` is true, the launcher's OmniAI Learning submod is enabled, or `OMNIAI_LEARNING` is set |
| `omniaiResetBrain` | a number | any nonzero value wipes the learned memory at the next game start (the old file is kept as `memory.json.bak`) and is set back to 0 |
| `omniaiDifficultyBySlot` | an object of player colours, each 0 to 4 | that colour's decision tier, Easy to Impossible; -1 follows the scenario |
| `omniaiDifficulty` | 0 to 4 | one tier for every OmniAI player; -1 follows the scenario |
| `OMNIAI_DIR` (environment) | a folder | where OmniAI writes its decision logs and learned memory; default `Documents\My Games\vcmi\OmniAI` |
| `OMNIAI_NARRATE` (environment) | any value | narrate decisions in a watched game |

Each game writes a readable decision log per OmniAI player into that folder
(`decisions-<colour>.log`, with a `.jsonl` twin), replacing the previous game's.

## License

GNU General Public License v2.0 or later, the same as VCMI. See `LICENSE`.
