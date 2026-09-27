# -*- coding: utf-8 -*-
"""build_installer.py - assemble the OmniAI installer payload.

Copies a built plugin and the launcher mod entry into payload\\, beside the
scripts a friend actually runs. Pass the DLL to ship; without one it takes
build\\AI\\OmniAI.dll, whatever build.bat made last.

    py build_installer.py [path\\to\\OmniAI.dll]

payload\\ is build output (ignored by git). The mod entry's source is
mod\\OmniAI in this repository; tbb12.dll is only a fallback copy, taken
from the project's test install when it is there.
"""
import io, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAYLOAD = os.path.join(HERE, "payload")

DLL = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 \
    else os.path.join(HERE, "..", "build", "AI", "OmniAI.dll")
TBB = os.path.join(HERE, "..", "..", "VCMI", "testinstall", "tbb12.dll")
MOD = os.path.join(HERE, "mod", "OmniAI")


def main():
    if not os.path.isfile(DLL):
        print("OmniAI.dll not found, run build.bat first")
        return 1

    if os.path.isdir(PAYLOAD):
        shutil.rmtree(PAYLOAD)
    ai = os.path.join(PAYLOAD, "AI")
    os.makedirs(ai)
    shutil.copy2(DLL, os.path.join(ai, "OmniAI.dll"))
    print("  OmniAI.dll   %d bytes" % os.path.getsize(DLL))

    # Only a fallback. Every stock VCMI ships tbb12.dll beside the exe; the
    # installer copies this one solely when that is missing.
    if os.path.isfile(TBB):
        shutil.copy2(TBB, os.path.join(ai, "tbb12.dll"))
        print("  tbb12.dll    %d bytes (fallback copy)" % os.path.getsize(TBB))
    else:
        print("  tbb12.dll    NOT FOUND, installer will warn instead")

    if os.path.isdir(MOD):
        dst = os.path.join(PAYLOAD, "Mods", "OmniAI")
        shutil.copytree(MOD, dst)
        n = sum(len(f) for _r, _d, f in os.walk(dst))
        print("  launcher mod entry: %d files (includes the Learning toggle)" % n)
    else:
        print("  launcher mod entry NOT FOUND at %s" % MOD)

    total = 0
    for root, _dirs, files in os.walk(PAYLOAD):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    print("\n  payload ready: %s (%.1f KB)" % (PAYLOAD, total / 1024.0))
    print("  ship the whole installer folder.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
