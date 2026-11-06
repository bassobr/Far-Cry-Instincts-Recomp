# End-to-end test of skip_intro the way a player sets it: F10 menu, close the game, start it again.
#   powershell -File tools\tests\skip_intro.ps1 [-Dir <folder with FarCryInstincts.exe>]
# 1. start without the key in FarCryInstincts.ini: the three logo videos play (fmv: video ...)
# 2. start with FCI_MENU_SCRIPT: F10, End, Up x3 (SKIP STARTUP VIDEOS, the 4th row from the end of the SETTINGS page),
#    Right, Escape; the menu saves skip_intro = 1; the game is closed like the window's X button (WM_CLOSE)
# 3. start again: the log has "video: skip_intro on" and "... skipped (skip_intro)" for Ubisoft, Crytek, Intro_FCI
# The folder's FarCryInstincts.ini is set aside first and put back at the end. Waits while another FarCryInstincts runs.
param([string]$Dir = (Join-Path $PSScriptRoot "..\..\out\build\release"))
$ErrorActionPreference = "Stop"
$Dir = (Resolve-Path $Dir).Path
$exe = Join-Path $Dir "FarCryInstincts.exe"
$ini = Join-Path $Dir "FarCryInstincts.ini"
$log = Join-Path $Dir "fci.log"
if (-not (Test-Path $exe)) { throw "no $exe" }

function Run([int]$Seconds, [string]$Script = "") {
    while (Get-Process FarCryInstincts -ErrorAction SilentlyContinue) { Start-Sleep 5 }
    foreach ($v in "FCI_SKIP_INTRO", "FCI_START_LEVEL", "FCI_INPUT_SCRIPT", "FCI_MENU_SCRIPT", "FCI_DUMP_FRAMES") {
        Remove-Item "Env:$v" -ErrorAction SilentlyContinue
    }
    if ($Script) { $env:FCI_MENU_SCRIPT = $Script }
    $p = Start-Process -FilePath $exe -WorkingDirectory $Dir -PassThru
    Start-Sleep $Seconds
    [void]$p.CloseMainWindow()  # as the window's X button / Alt+F4
    if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force }
    Remove-Item Env:FCI_MENU_SCRIPT -ErrorAction SilentlyContinue
    Start-Sleep 1
    return Get-Content $log
}

$saved = $null
if (Test-Path $ini) { $saved = Get-Content -Raw $ini; Remove-Item $ini }
$failed = @()
try {
    $first = Run 20
    if (-not ($first -match "fmv: video ubisoft\.bik")) { $failed += "without the setting the Ubisoft video did not play" }
    $menu = Run 20 "6:f10,8:end,9:up,9.7:up,10.4:up,11.5:right,13:escape"
    if (-not ($menu -match "settings: saved to FarCryInstincts\.ini: skip_intro=1")) { $failed += "the menu did not save skip_intro=1" }
    if (-not ((Get-Content $ini) -match "^\s*skip_intro\s*=\s*1")) { $failed += "FarCryInstincts.ini has no skip_intro = 1" }
    $next = Run 15
    if (-not ($next -match "video: skip_intro on")) { $failed += "the next start did not read skip_intro = 1" }
    foreach ($v in "Ubisoft", "Crytek", "Intro_FCI") {
        if (-not ($next -match "video: .*\\$v\.bik skipped \(skip_intro\)")) { $failed += "$v.bik was not skipped" }
    }
} finally {
    Remove-Item $ini -ErrorAction SilentlyContinue
    if ($null -ne $saved) { Set-Content -NoNewline -Encoding ascii -Path $ini -Value $saved }
}
if ($failed.Count) { "FAIL:"; $failed | ForEach-Object { "  $_" }; exit 1 }
"PASS: menu -> FarCryInstincts.ini -> next start skips Ubisoft, Crytek, Intro_FCI"
