#!/usr/bin/env bash
# build_v2.sh TAG: build the engine-v2 worktree (C:\AI Projects\Heroes 3\OmniAI-v2) with its build.bat (hidden, through
# build_v2_run.cmd) and archive dlls/<TAG>_<sha8>.dll beside its OmniAI.cpp/.h and a tarball of engine/. October 4th
# (megaplan, option B). Bounded: the build has a 15-minute wait. Nothing is deployed or queued.
set -u
cd "C:/AI Projects/Heroes 3/.tmp/omni"
V2="C:/AI Projects/Heroes 3/OmniAI-v2"
tag=$1
log="build_${tag}.log"
rm -f "${log:?}"
rm -f build_v2_current.log
powershell.exe -NoProfile -Command "Start-Process -WindowStyle Hidden -FilePath 'C:\AI Projects\Heroes 3\.tmp\omni\build_v2_run.cmd'" >/dev/null 2>&1
end=$((SECONDS + 900))
until grep -q "BUILD OK\|BUILD FAILED\|CONFIGURE FAILED" build_v2_current.log 2>/dev/null || [ $SECONDS -ge $end ]; do sleep 3; done
sleep 2
cp build_v2_current.log "$log" 2>/dev/null
if grep -q "BUILD OK" "$log" 2>/dev/null; then
  h=$(sha256sum "$V2/build/AI/OmniAI.dll" | cut -c1-8 | tr a-f A-F)
  cp "$V2/build/AI/OmniAI.dll" "dlls/${tag}_$h.dll"
  cp "$V2/OmniAI.cpp" "dlls/OmniAI.cpp.$tag"
  cp "$V2/OmniAI.h" "dlls/OmniAI.h.$tag"
  ( cd "$V2" && tar -czf "C:/AI Projects/Heroes 3/.tmp/omni/dlls/engine.$tag.tgz" engine 2>/dev/null )
  echo "$tag -> ${tag}_$h ($(git -C "$V2" rev-parse --short HEAD))"
else
  echo "$tag: build failed, see $log"
  grep -n "error" "$log" | head -40
fi
