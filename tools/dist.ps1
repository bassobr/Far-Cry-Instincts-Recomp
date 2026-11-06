# Assembles the release folder (and its zip): the executable without any game code, and the compiler folder it
# builds the game code with on the player's first start (src/app/game_code.cpp). Runs without game files (CI):
#   powershell -File tools\get_llvm.ps1 -Out C:\llvm
#   powershell -File tools\dist.ps1 -Llvm C:\llvm [-Build out\build\release] [-Out out\dist] [-Zip]
# -Build is a build of the release preset (with or without assets\default.xbe: game_code\ is never packed).
# Refuses anything game-derived: no game_code.dll, no generated code, no shader cache, no xbe/iso.
# Packs no DLL at all: the HLSL compiler d3dcompiler_47.dll is the system's (Windows 8.1 and later, Proton's builtin).
param(
    [Parameter(Mandatory = $true)][string]$Llvm,   # tools\get_llvm.ps1's -Out: bin\clang.exe, bin\lld-link.exe, LICENSE.TXT
    [string]$Build = "",
    [string]$Out = "",
    [string]$Name = "",
    [switch]$Zip
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Build) { $Build = Join-Path $root "out\build\release" }
if (-not $Out) { $Out = Join-Path $root "out\dist" }
$version = if (Test-Path (Join-Path $root "VERSION")) { (Get-Content -Raw (Join-Path $root "VERSION")).Trim() } else { "dev" }
if (-not $Name) { $Name = "FarCryInstincts-Recomp-v$version-win64" }
$LlvmMajor = "19"  # the major version that wrote config\game_code.profdata (tools\get_llvm.ps1 pins the release)

$stage = Join-Path $Out $Name
$compiler = Join-Path $Build "compiler"

# what goes into the release, nothing else: destination (relative to the release folder) -> source
$files = [ordered]@{
    "FarCryInstincts.exe"                   = Join-Path $Build "FarCryInstincts.exe"
    "compiler\clang.exe"                    = Join-Path $Llvm "bin\clang.exe"
    "compiler\lld-link.exe"                 = Join-Path $Llvm "bin\lld-link.exe"
    "compiler\LLVM-LICENSE.txt"             = Join-Path $Llvm "LICENSE.TXT"
    "compiler\fci-recomp.exe"               = Join-Path $compiler "fci-recomp.exe"
    "compiler\FarCryInstincts.lib"          = Join-Path $compiler "FarCryInstincts.lib"
    "compiler\include\runtime\guest.h"      = Join-Path $compiler "include\runtime\guest.h"
    "compiler\config\hle.txt"               = Join-Path $compiler "config\hle.txt"
    "compiler\config\symbols.txt"           = Join-Path $compiler "config\symbols.txt"
    "compiler\config\symbols_fci.txt"       = Join-Path $compiler "config\symbols_fci.txt"
    "compiler\config\game_code.profdata"    = Join-Path $root "config\game_code.profdata"
}
foreach ($h in Get-ChildItem (Join-Path $compiler "include\freestanding") -File) {
    $files["compiler\include\freestanding\$($h.Name)"] = $h.FullName
}
# the documents the repository has (README, changelog, licence, notices, version)
foreach ($doc in "README.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY_NOTICES.md", "VERSION") {
    if (Test-Path (Join-Path $root $doc)) { $files[$doc] = Join-Path $root $doc }
    else { Write-Warning "$doc is missing in $root, not packed" }
}
foreach ($src in $files.Values) {
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "missing $src (build the release preset, run tools\get_llvm.ps1)" }
}

# the bundled clang must be the major that consumes the profile
$reported = & (Join-Path $Llvm "bin\clang.exe") --version | Select-Object -First 1
if ($reported -notmatch "clang version $LlvmMajor\.") { throw "bundled clang is '$reported', the profile needs LLVM $LlvmMajor" }

# an executable with the game code linked in (the old static build) is some 60 MB and carries its exports
$exe = Get-Item $files["FarCryInstincts.exe"]
if ($exe.Length -gt 25MB) { throw "FarCryInstincts.exe is $([int]($exe.Length / 1MB)) MB: it contains game code" }
$bytes = [IO.File]::ReadAllBytes($exe.FullName)
$text = [Text.Encoding]::ASCII.GetString($bytes)
if ($text -match "\?[fo]_[0-9A-F]{8}@@") { throw "FarCryInstincts.exe contains recompiled functions" }

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($entry in $files.GetEnumerator()) {
    $dst = Join-Path $stage $entry.Key
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $dst
}
$stage = (Get-Item -LiteralPath $stage).FullName
# where the game goes (its .iso, or the disc's files with default.xbe)
New-Item -ItemType Directory -Force (Join-Path $stage "assets") | Out-Null
$assetsNote = "Put the game here: its disc image (.iso), or the disc's extracted files (default.xbe with the folders beside it).`r`n" +
    "The first start compiles the game's code for this PC once (game_code\ beside FarCryInstincts.exe).`r`n"
[IO.File]::WriteAllText((Join-Path $stage "assets\README.txt"), $assetsNote)

# never the game or anything derived from it
$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object {
    $_.Name -match '^(game_code|generated|work|shader_cache|textures|data|asset_fc1)$' -or
    $_.Name -match '\.(xbe|iso|xiso|profraw|hlsl|dds|png|bmp|pak|pdb|obj|dll\.tmp)$' -or
    $_.Name -match '\.dll$' -or
    ($_.Name -match '\.cpp$') -or
    ($_.Name -eq "funcs.h")
}
if ($forbidden) { throw "refusing to package: $($forbidden.FullName -join ', ')" }
$packed = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
if ($packed.Count -ne $files.Count + 1) { throw "expected $($files.Count + 1) files, found $($packed.Count)" }

$size = ($packed | Measure-Object Length -Sum).Sum
Write-Host "release folder: $stage ($([math]::Round($size / 1MB, 1)) MB, $($packed.Count) files, bundled $reported)"
$packed | Sort-Object FullName | ForEach-Object {
    Write-Host ("  {0,-44} {1,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.Length)
}

if ($Zip) {
    $zipFile = Join-Path $Out "$Name.zip"
    Remove-Item -Force $zipFile -ErrorAction SilentlyContinue
    # entry names with forward slashes (Windows PowerShell's Compress-Archive writes backslashes)
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zipFile, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $packed) {
            $entry = "$Name/" + ($f.FullName.Substring($stage.Length + 1) -replace '\\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipFile).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $Out "SHA256SUMS"), "$hash  $Name.zip`n")
    Write-Host "zip: $zipFile ($([math]::Round((Get-Item $zipFile).Length / 1MB, 1)) MB), sha256 $hash"
}
