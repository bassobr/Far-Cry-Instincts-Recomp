# Regenerates config/game_code.profdata, the profile the release build optimizes the recompiled code with (FCI_PGO).
# Run from the repository root after anything changes the generated code (recompiler, symbols, hle.txt):
#   powershell -File tools\pgo.ps1
# Builds the instrumented preset pgo-gen, plays the single-player script (title, cutscene, swim, wreck) for 250 s,
# writes the counters at 245 s and merges them. A stale profile is not an error: functions whose code changed are
# just optimized without it (the build stays correct, only slower).
param([int]$Seconds = 250)
$ErrorActionPreference = "Stop"
$root = (Get-Location).Path
if (-not (Test-Path "$root\assets\default.xbe") -and -not (Get-ChildItem "$root\assets\*.iso" -ErrorAction SilentlyContinue)) {
    throw "run from the repository root (assets\ missing)"
}
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User") +
    ";C:\Program Files (x86)\Microsoft Visual Studio\Installer"
[Environment]::CurrentDirectory = $root
& .\build.bat pgo-gen
if ($LASTEXITCODE -ne 0) { throw "build of pgo-gen failed" }

$dir = "$root\out\build\pgo-gen"
$raw = "$dir\game_code.profraw"
Remove-Item -Force $raw -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "$dir\data\hdd\partition1\UDATA\55530008\9FBDF33E7A0C" -ErrorAction SilentlyContinue
$vars = @{
    "FCI_INPUT_SCRIPT" = "28:start,34:a,42:a,47:a,52:a,57:a,62:a,186:a,191:a,196:a,198:rup:0.15,199:lup:70"
    "FCI_FPS_LIMIT" = "90"; "FCI_PGO_WRITE" = [string]($Seconds - 5); "LLVM_PROFILE_FILE" = $raw
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
$p = Start-Process -FilePath "$dir\FarCryInstincts.exe" -WorkingDirectory $dir -PassThru
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force }
if (-not (Test-Path $raw)) { throw "no profile written (see $dir\fci.log, 'pgo:' line)" }

$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\VC\Tools\Llvm\x64\bin\llvm-profdata.exe" merge -o "$root\config\game_code.profdata" $raw
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata failed" }
"config\game_code.profdata written; build.bat release uses it"
