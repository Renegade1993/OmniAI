# OmniAI

An adventure-map AI for [VCMI](https://github.com/vcmi/vcmi), the open-source engine for Heroes of
Might and Magic III, written against VCMI 1.7.5. It scores every visitable object on the map,
prices a guard before committing a hero to it, walks whole routes in one turn, recruits in towns and
at dwellings, hires heroes, explores toward unseen ground and fights its way past guards that seal
it in. Battles are left to VCMI's own BattleAI.

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
