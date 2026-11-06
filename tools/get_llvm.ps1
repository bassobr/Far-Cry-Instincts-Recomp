# Downloads the official LLVM release the release ships (compiler\clang.exe, compiler\lld-link.exe) and CI builds
# with, checks it against the pinned SHA-256 and extracts what is needed into -Out:
#   powershell -File tools\get_llvm.ps1 -Out C:\llvm
# Leaves -Out\bin\{clang.exe, clang-cl.exe, lld-link.exe, llvm-lib.exe, llvm-mt.exe, llvm-profdata.exe},
# -Out\lib\clang\<major> (clang's own headers and runtime libraries, for building the executable) and
# -Out\LICENSE.TXT. The major version must be the one that wrote config\game_code.profdata (tools\pgo.ps1 merges
# with the same major's llvm-profdata); the first start compiles with that profile.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Cache = ""  # a folder that keeps the download between runs (CI cache), default: -Out
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# ---- the pinned release ----
$Version = "19.1.5"
$Archive = "clang+llvm-$Version-x86_64-pc-windows-msvc.tar.xz"
$ArchiveUrl = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$Version/$Archive"
$ArchiveSha256 = "467D1A73CA938F47734AF3BAAC2E78C5E730285469096EE088BB5C9590CABD70"
$LicenseUrl = "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$Version/llvm/LICENSE.TXT"
$LicenseSha256 = "8D85C1057D742E597985C7D4E6320B015A9139385CFF4CBAE06FFC0EBE89AFEE"
$Major = $Version.Split(".")[0]

if (-not $Cache) { $Cache = $Out }
New-Item -ItemType Directory -Force $Out, $Cache | Out-Null

function Get-Pinned([string]$Url, [string]$File, [string]$Sha) {
    if (-not (Test-Path $File) -or (Get-FileHash $File -Algorithm SHA256).Hash -ne $Sha) {
        Write-Host "downloading $Url"
        Invoke-WebRequest -UseBasicParsing $Url -OutFile "$File.part"
        Move-Item -Force "$File.part" $File
    }
    $hash = (Get-FileHash $File -Algorithm SHA256).Hash
    if ($hash -ne $Sha) { throw "$File has SHA-256 $hash, expected $Sha" }
}

$tar = Join-Path $Cache $Archive
Get-Pinned $ArchiveUrl $tar $ArchiveSha256
Get-Pinned $LicenseUrl (Join-Path $Out "LICENSE.TXT") $LicenseSha256

$top = "clang+llvm-$Version-x86_64-pc-windows-msvc"
$members = @("bin/clang.exe", "bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-lib.exe", "bin/llvm-mt.exe",
             "bin/llvm-profdata.exe", "lib/clang/$Major") | ForEach-Object { "$top/$_" }
$stage = Join-Path $Out "extract"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
& tar.exe -xf $tar -C $stage @members
if ($LASTEXITCODE -ne 0) { throw "extracting $tar failed" }
foreach ($dir in "bin", "lib") {
    if (Test-Path (Join-Path $Out $dir)) { Remove-Item -Recurse -Force (Join-Path $Out $dir) }
    Move-Item (Join-Path $stage "$top\$dir") (Join-Path $Out $dir)
}
Remove-Item -Recurse -Force $stage

$reported = & (Join-Path $Out "bin\clang.exe") --version | Select-Object -First 1
if ($reported -notmatch "clang version $([regex]::Escape($Version))\b") { throw "unexpected clang: $reported" }
Write-Host "LLVM $Version in $Out ($reported)"
