OmniAI for VCMI 1.7.5
=====================

OmniAI is a replacement adventure-map AI for VCMI. It scores every visitable
object on the map, checks how strong a guard is before committing a hero,
moves whole multi-tile paths in one turn instead of one step, recruits at
dwellings and in towns, hires heroes, explores toward unrevealed ground, and
fights its way out when guards seal it in. Battles are handled by VCMI's own
BattleAI, so combat plays exactly as it always did.


INSTALL
-------

1. Close VCMI. The installer refuses to run while it is open, because
   Windows holds the plugin file locked.

2. Double-click "Install OmniAI.bat".

   It finds your VCMI folder, copies one file into VCMI\AI\, adds an entry to
   the mod launcher, and sets computer opponents to use OmniAI. Your
   settings.json is backed up to settings.json.omniai-backup first.

   If it cannot find VCMI it will ask. The folder it wants is the one holding
   VCMI_client.exe.

3. Start VCMI and play any map with computer players.

Use "Install OmniAI (allies too).bat" instead if you want computer ALLIES to
play as OmniAI as well. The plain installer leaves allies on whatever they
were using, which makes it easy to play OmniAI against Nullkiller.


CROSS-GAME LEARNING
-------------------

Off by default. With it on, OmniAI remembers three things between games:
how much each kind of map object turned out to be worth, how much stronger a
guard has to be before it refuses a fight, and which one-shot objects such as
witch huts and shrines it has already used up.

To switch it on: open the VCMI launcher, find OmniAI in the mod list, and
enable the Learning submod underneath it.

It is off by default because a trained AI plays differently from a fresh one,
and that should be your choice rather than a surprise. To reset a trained AI,
delete this file:

    Documents\My Games\vcmi\OmniAI\memory.json


REMOVING IT
-----------

Double-click "Uninstall OmniAI.bat". It deletes the plugin, removes the
launcher entry, and puts computer opponents back to Nullkiller.


WHAT IT NEEDS
-------------

VCMI 1.7.0 or newer, on Windows 64-bit. Nothing else. OmniAI depends on
VCMI_lib.dll, tbb12.dll and the Microsoft C++ runtime, and every VCMI install
already ships all of them.


TROUBLESHOOTING
---------------

Computer players do nothing, or the game says it cannot load the AI:
  Check that VCMI\AI\OmniAI.dll exists and that tbb12.dll is in your VCMI
  folder next to VCMI_client.exe. Both are normal parts of a VCMI install.

The installer says it cannot find VCMI:
  Give it the path yourself when it asks, or drop this whole folder inside
  your VCMI folder and run it from there.

You want to check which AI is actually selected:
  Open Documents\My Games\vcmi\config\settings.json and look at the "ai"
  block. "adventureEnemyAI" is computer opponents, "adventureAlliedAI" is
  computer allies.
