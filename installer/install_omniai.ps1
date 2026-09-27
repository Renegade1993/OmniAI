<#
    install_omniai.ps1 - put OmniAI into a VCMI install and select it.

    What it actually has to do is small. OmniAI.dll depends on VCMI_lib.dll,
    tbb12.dll and the MSVC runtime, and a stock VCMI 1.7.5 install ships all
    of them already, so nothing but the plugin itself has to be copied. The
    engine loads adventure AIs from <VCMI>\AI\<name>.dll, chosen by the
    ai.adventureEnemyAI and ai.adventureAlliedAI keys in the user's
    settings.json. No schema edit is needed: JsonUtils::validate only logs a
    warning for an unknown enum value and keeps the value.

    Usage:
        .\install_omniai.ps1                      find VCMI, ask, install
        .\install_omniai.ps1 -VcmiDir "D:\VCMI"   skip detection
        .\install_omniai.ps1 -Allied              also play allied AI slots
        .\install_omniai.ps1 -Uninstall           put everything back
        .\install_omniai.ps1 -NonInteractive      never prompt, fail instead
#>
[CmdletBinding()]
param(
    [string]$VcmiDir = "",
    [switch]$Allied,
    [switch]$Uninstall,
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$payload = Join-Path $here "payload"

function Say($msg)  { Write-Host "  $msg" }
function Warn($msg) { Write-Host "  ! $msg" -ForegroundColor Yellow }
function Die($msg)  { Write-Host "  x $msg" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "OmniAI for VCMI"
Write-Host "==============="
Write-Host ""

# ---------------------------------------------------------------- find it --
function Find-Vcmi {
    $seen = New-Object System.Collections.Generic.List[string]

    # Run from inside the install, which is where a friend is most likely to
    # have dropped the folder.
    $up = Split-Path -Parent $here
    if ($up) { $seen.Add($up) }

    foreach ($root in @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
                        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
                        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall")) {
        if (-not (Test-Path $root)) { continue }
        foreach ($key in Get-ChildItem $root) {
            try { $p = Get-ItemProperty $key.PSPath } catch { continue }
            if ($p.DisplayName -and $p.DisplayName -match "VCMI") {
                if ($p.InstallLocation) { $seen.Add($p.InstallLocation) }
            }
        }
    }

    foreach ($guess in @("$env:ProgramFiles\VCMI",
                         "${env:ProgramFiles(x86)}\VCMI",
                         "$env:LOCALAPPDATA\Programs\VCMI",
                         "C:\VCMI")) {
        $seen.Add($guess)
    }

    foreach ($cand in $seen) {
        if ([string]::IsNullOrWhiteSpace($cand)) { continue }
        if (Test-Path (Join-Path $cand "VCMI_client.exe")) { return $cand }
    }
    return ""
}

if ([string]::IsNullOrWhiteSpace($VcmiDir)) {
    $VcmiDir = Find-Vcmi
    if ($VcmiDir) { Say "found VCMI at $VcmiDir" }
}

if ([string]::IsNullOrWhiteSpace($VcmiDir)) {
    if ($NonInteractive) { Die "could not find VCMI. Pass -VcmiDir <path>." }
    Write-Host "  Could not find VCMI automatically."
    Write-Host "  It is the folder holding VCMI_client.exe."
    $VcmiDir = Read-Host "  Paste that folder path"
}

$VcmiDir = $VcmiDir.Trim('"').TrimEnd('\')
if (-not (Test-Path (Join-Path $VcmiDir "VCMI_client.exe"))) {
    Die "no VCMI_client.exe in $VcmiDir"
}

$aiDir = Join-Path $VcmiDir "AI"
$dllPath = Join-Path $aiDir "OmniAI.dll"

# The engine holds the dll open. Copying over a running client fails with a
# sharing violation and leaves a half-installed mess, so refuse up front.
$running = @(Get-Process -Name "VCMI_client", "VCMI_server", "VCMI_launcher" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    Die "close VCMI first (running: $(($running | ForEach-Object { $_.ProcessName }) -join ', '))"
}

# ------------------------------------------------------------- user files --
$userDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\vcmi"
$settingsPath = Join-Path $userDir "config\settings.json"
$modsDir = Join-Path $userDir "Mods"

function Read-Json($path) {
    if (-not (Test-Path $path)) { return $null }
    $raw = Get-Content -Path $path -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
    return $raw | ConvertFrom-Json
}

function Write-Json($path, $obj) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    # Depth matters: PowerShell 5.1 silently flattens past depth 2.
    $json = $obj | ConvertTo-Json -Depth 30
    [System.IO.File]::WriteAllText($path, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Ensure-Node($parent, $name) {
    if ($null -eq $parent.$name) {
        $parent | Add-Member -MemberType NoteProperty -Name $name -Value (New-Object PSObject) -Force
    }
    return $parent.$name
}

function Set-Key($node, $name, $value) {
    $node | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

# ------------------------------------------------------------- uninstall ---
if ($Uninstall) {
    if (Test-Path $dllPath) { Remove-Item $dllPath -Force; Say "removed $dllPath" }
    else { Say "no OmniAI.dll in $aiDir" }

    # A tbb12.dll in the AI folder is left alone on purpose. We only ever put
    # one there when the install had none at all, and in that case every other
    # TBB-using plugin is leaning on it too.
    if (Test-Path (Join-Path $aiDir "tbb12.dll")) {
        Say "left tbb12.dll alone, other AI plugins may need it"
    }

    $omniMod = Join-Path $modsDir "OmniAI"
    if (Test-Path $omniMod) { Remove-Item $omniMod -Recurse -Force; Say "removed the OmniAI mod entry" }

    $s = Read-Json $settingsPath
    if ($s -and $s.ai) {
        $changed = $false
        foreach ($k in @("adventureEnemyAI", "adventureAlliedAI")) {
            if ($s.ai.$k -eq "OmniAI") { Set-Key $s.ai $k "Nullkiller"; $changed = $true }
        }
        if ($changed) {
            Copy-Item $settingsPath "$settingsPath.omniai-backup" -Force
            Write-Json $settingsPath $s
            Say "put the AI selection back to Nullkiller"
        }
    }
    Write-Host ""
    Write-Host "  OmniAI removed."
    Write-Host ""
    exit 0
}

# --------------------------------------------------------------- install ---
$srcDll = Join-Path $payload "AI\OmniAI.dll"
if (-not (Test-Path $srcDll)) { Die "payload\AI\OmniAI.dll is missing from this folder" }

if (-not (Test-Path $aiDir)) { New-Item -ItemType Directory -Force -Path $aiDir | Out-Null }
Copy-Item $srcDll $dllPath -Force
Say "installed OmniAI.dll into $aiDir"

# tbb12.dll ships with VCMI and lives beside the exe, which is on the search
# path for a plugin the engine loads. Only copy ours if that one is absent.
if (-not (Test-Path (Join-Path $VcmiDir "tbb12.dll"))) {
    $srcTbb = Join-Path $payload "AI\tbb12.dll"
    if (Test-Path $srcTbb) {
        Copy-Item $srcTbb (Join-Path $aiDir "tbb12.dll") -Force
        Say "this install had no tbb12.dll, so one was copied in"
    } else {
        Warn "no tbb12.dll here or in the payload. OmniAI will fail to load."
    }
}

# The launcher entry, which is also where the Learning toggle lives.
$srcMod = Join-Path $payload "Mods\OmniAI"
if (Test-Path $srcMod) {
    $dstMod = Join-Path $modsDir "OmniAI"
    if (-not (Test-Path $modsDir)) { New-Item -ItemType Directory -Force -Path $modsDir | Out-Null }
    Copy-Item $srcMod $modsDir -Recurse -Force
    Say "installed the launcher entry at $dstMod"
}

# ------------------------------------------------------------- select it ---
$s = Read-Json $settingsPath
if ($null -eq $s) { $s = New-Object PSObject }
if (Test-Path $settingsPath) { Copy-Item $settingsPath "$settingsPath.omniai-backup" -Force }

$ai = Ensure-Node $s "ai"
Set-Key $ai "adventureEnemyAI" "OmniAI"
if ($Allied) { Set-Key $ai "adventureAlliedAI" "OmniAI" }
Write-Json $settingsPath $s

Say "settings.json now selects OmniAI for computer opponents"
if ($Allied) { Say "computer allies will use OmniAI too" }
else { Say "computer allies keep whatever they were using" }

Write-Host ""
Write-Host "  Done. Start VCMI and play a map with computer players."
Write-Host ""
Write-Host "  Cross-game learning is off by default. To switch it on, open the"
Write-Host "  VCMI launcher, find OmniAI in the mod list, and enable the"
Write-Host "  Learning submod underneath it."
Write-Host ""
Write-Host "  To remove OmniAI again, run Uninstall OmniAI.bat."
Write-Host ""
exit 0
