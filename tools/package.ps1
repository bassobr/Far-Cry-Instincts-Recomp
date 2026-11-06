# Packs the recompiler and the semantics test of the tools preset (build.bat tools) into
# out\dist\far-cry-instincts-recomp-tools-<version>.zip with SHA256SUMS. Never packs the game, its code or anything
# built from it.
#   powershell -File tools\package.ps1
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
if (-not $version) { throw 'VERSION is empty' }
$name = "far-cry-instincts-recomp-tools-$version"
$dist = Join-Path $root 'out\dist'
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"
$build = Join-Path $root 'out\build\tools'

# what goes into the package, nothing else
$files = [ordered]@{
    'fci-recomp.exe'         = Join-Path $build 'fci-recomp.exe'
    'fci-semtest.exe'        = Join-Path $build 'fci-semtest.exe'
    'README.md'              = Join-Path $root 'README.md'
    'LICENSE'                = Join-Path $root 'LICENSE'
    'THIRD_PARTY_NOTICES.md' = Join-Path $root 'THIRD_PARTY_NOTICES.md'
    'VERSION'                = Join-Path $root 'VERSION'
    'docs\ARCHITECTURE.md'   = Join-Path $root 'docs\ARCHITECTURE.md'
}
foreach ($src in $files.Values) {
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "missing $src (run build.bat tools)" }
}

if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'docs') | Out-Null
foreach ($entry in $files.GetEnumerator()) {
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $stage $entry.Key)
}

# the package must never contain the game or anything derived from it
$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object {
    $_.Name -match '^(FarCryInstincts\.exe|assets|asset_fc1|generated|shader_cache|textures|data)$' -or
    $_.Name -match '\.(xbe|iso|xiso|profdata|profraw|hlsl|dds|png|bmp|pak)$'
}
if ($forbidden) { throw "refusing to package: $($forbidden.FullName -join ', ')" }
$packed = @(Get-ChildItem -LiteralPath $stage -Recurse -File).Count
if ($packed -ne $files.Count) { throw "expected $($files.Count) files, found $packed" }

# entry names with forward slashes (Windows PowerShell's Compress-Archive writes backslashes)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($key in $files.Keys) {
        $entry = "$name/" + ($key -replace '\\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $stage $key), $entry,
            [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS'), "$hash  $name.zip`n")

Write-Host "package: $zip"
Write-Host "version: $version"
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object { Write-Host "  $($_.FullName.Substring($stage.Length + 1))" }
Get-Content (Join-Path $dist 'SHA256SUMS')
