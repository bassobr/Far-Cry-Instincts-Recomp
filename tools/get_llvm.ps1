# Downloads the official LLVM release the release ships (compiler\clang.exe, compiler\lld-link.exe) and CI builds
# with, checks it against the pinned SHA-256 and extracts what is needed into -Out:
#   powershell -File tools\get_llvm.ps1 -Out C:\llvm
# Leaves -Out\bin\{clang.exe, clang-cl.exe, lld-link.exe, llvm-lib.exe, llvm-mt.exe, llvm-profdata.exe},
# -Out\lib\clang\<major> (clang's own headers and runtime libraries, for building the executable) and
# -Out\LICENSE.TXT. The major version must be the one that wrote config\game_code.profdata (tools\pgo.ps1 merges
# with the same major's llvm-profdata); the first start compiles with that profile.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Cache = "",  # a folder that keeps the download between runs (CI cache), default: -Out
    [string]$SevenZip = ""  # 7z.exe or 7za.exe; default: 7z on PATH or in Program Files, else Windows' tar
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

# Windows' own curl. The archive is extracted with 7-Zip when there is one (preinstalled on GitHub's Windows
# runners, where Windows' bsdtar hangs on this archive; 7-Zip decodes the xz stream on several threads), else with
# Windows' own bsdtar (a Git for Windows tar earlier in PATH would take C:\... for a remote host)
$curlExe = Join-Path $env:SystemRoot "System32\curl.exe"
$tarExe = Join-Path $env:SystemRoot "System32\tar.exe"
if (-not $SevenZip) {
    $found = Get-Command 7z.exe, 7za.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $SevenZip = $found.Source }
    elseif (Test-Path "$env:ProgramFiles\7-Zip\7z.exe") { $SevenZip = "$env:ProgramFiles\7-Zip\7z.exe" }
}
$clock = [Diagnostics.Stopwatch]::StartNew()
function Step([string]$Text) { Write-Host ("[{0,6:N1} s] {1}" -f $clock.Elapsed.TotalSeconds, $Text) }

function Get-Pinned([string]$Url, [string]$File, [string]$Sha) {
    if (-not (Test-Path $File) -or (Get-FileHash $File -Algorithm SHA256).Hash -ne $Sha) {
        Step "downloading $Url"
        & $curlExe -fsSL --retry 3 -o "$File.part" $Url
        if ($LASTEXITCODE -ne 0) { throw "downloading $Url failed ($LASTEXITCODE)" }
        Move-Item -Force "$File.part" $File
        Step "downloaded $File ($([math]::Round((Get-Item $File).Length / 1MB, 1)) MB)"
    } else {
        Step "using $File"
    }
    $hash = (Get-FileHash $File -Algorithm SHA256).Hash
    if ($hash -ne $Sha) { throw "$File has SHA-256 $hash, expected $Sha" }
    Step "SHA-256 ok: $hash"
}

$tar = (Join-Path (Resolve-Path $Cache).Path $Archive)
Get-Pinned $ArchiveUrl $tar $ArchiveSha256
Get-Pinned $LicenseUrl (Join-Path $Out "LICENSE.TXT") $LicenseSha256

$top = "clang+llvm-$Version-x86_64-pc-windows-msvc"
$members = @("bin/clang.exe", "bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-lib.exe", "bin/llvm-mt.exe",
             "bin/llvm-profdata.exe", "lib/clang/$Major") | ForEach-Object { "$top/$_" }
$stage = Join-Path $Out "extract"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
if ($SevenZip) {
    Step "extracting $($members.Count) members with $SevenZip"
    # xz into tar through a pipe of cmd, from a script file (PowerShell 5.1 would re-encode the piped bytes and
    # mangle the quotes of an inline command)
    $quoted = ($members | ForEach-Object { "`"$_`"" }) -join " "
    $script = Join-Path $Out "extract.cmd"
    $pipe = "@`"$SevenZip`" x -so -txz -bd `"$tar`" | `"$SevenZip`" x -si -ttar -y -bd `"-o$stage`" $quoted >nul"
    [IO.File]::WriteAllText($script, "$pipe`r`n", [Text.Encoding]::Default)
    & cmd.exe /d /c $script
    Remove-Item -Force $script
} else {
    Step "extracting $($members.Count) members with $tarExe"
    & $tarExe -xf $tar -C $stage @members
}
if ($LASTEXITCODE -ne 0) { throw "extracting $tar failed ($LASTEXITCODE)" }
Step "extracted"
foreach ($dir in "bin", "lib") {
    if (Test-Path (Join-Path $Out $dir)) { Remove-Item -Recurse -Force (Join-Path $Out $dir) }
    Move-Item (Join-Path $stage "$top\$dir") (Join-Path $Out $dir)
}
Remove-Item -Recurse -Force $stage

$reported = & (Join-Path $Out "bin\clang.exe") --version | Select-Object -First 1
if ($reported -notmatch "clang version $([regex]::Escape($Version))\b") { throw "unexpected clang: $reported" }
Step "LLVM $Version in $Out ($reported)"
