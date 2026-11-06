# Prints the header, sections and linked XDK libraries of an XBE.
#   powershell -File tools\xbeinfo.ps1 assets\default.xbe
param([Parameter(Mandatory = $true)][string]$Xbe)

$ErrorActionPreference = 'Stop'
$b = [IO.File]::ReadAllBytes((Resolve-Path $Xbe).Path)
if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'XBEH') { throw "not an XBE" }

function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
$base = U32 0x104
$headerSize = U32 0x108
function Off($va) {
    if ($va - $base -lt $headerSize) { return [int]($va - $base) }
    $sh = [int]((U32 0x120) - $base)
    for ($i = 0; $i -lt (U32 0x11C); $i++) {
        $s = $sh + $i * 56
        $sva = U32 ($s + 4)
        if ($va -ge $sva -and $va -lt $sva + (U32 ($s + 16))) { return [int]((U32 ($s + 12)) + $va - $sva) }
    }
    throw ("address 0x{0:X8} not in file" -f $va)
}
function CStr($va) { $o = Off $va; $e = $o; while ($b[$e]) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }

# retail and debug XOR keys for the entry point and kernel thunk
$entryRaw = U32 0x128
$thunkRaw = U32 0x158
$retail = (($entryRaw -bxor 0xA8FC57AB) -band 0xF0000000) -eq 0
$entry = if ($retail) { $entryRaw -bxor 0xA8FC57AB } else { $entryRaw -bxor 0x94859D4B }
$thunk = if ($retail) { $thunkRaw -bxor 0x5B6D40B6 } else { $thunkRaw -bxor 0xEFB1F152 }

$cert = Off (U32 0x118)
$titleId = U32 ($cert + 8)
$titleName = [Text.Encoding]::Unicode.GetString($b, $cert + 0xC, 80).TrimEnd([char]0)

"Title       : {0} ({1:X8})" -f $titleName, $titleId
"Base        : 0x{0:X8}  image size 0x{1:X}" -f $base, (U32 0x10C)
"Entry point : 0x{0:X8} ({1})" -f $entry, $(if ($retail) { 'retail' } else { 'debug' })
"Kernel thunk: 0x{0:X8}" -f $thunk
"TLS         : 0x{0:X8}" -f (U32 0x12C)
"Init flags  : 0x{0:X8}" -f (U32 0x124)
"Debug path  : {0}" -f (CStr (U32 0x14C))
""
"Sections:"
$n = U32 0x11C
$sh = Off (U32 0x120)
for ($i = 0; $i -lt $n; $i++) {
    $s = $sh + $i * 56
    $flags = U32 $s
    $kind = @()
    if ($flags -band 4) { $kind += 'exec' }
    if ($flags -band 1) { $kind += 'write' }
    if ($flags -band 2) { $kind += 'preload' }
    "  {0,-10} va 0x{1:X8} vsize 0x{2,-7:X} raw 0x{3:X8} rsize 0x{4,-7:X} {5}" -f (CStr (U32 ($s + 20))), (U32 ($s + 4)), (U32 ($s + 8)), (U32 ($s + 12)), (U32 ($s + 16)), ($kind -join ',')
}
""
"Libraries:"
$n = U32 0x160
$lv = Off (U32 0x164)
for ($i = 0; $i -lt $n; $i++) {
    $l = $lv + $i * 16
    "  {0,-8} {1}.{2}.{3} flags 0x{4:X4}" -f [Text.Encoding]::ASCII.GetString($b, $l, 8).TrimEnd([char]0), (U16 ($l + 8)), (U16 ($l + 10)), (U16 ($l + 12)), (U16 ($l + 14))
}
""
"Kernel imports:"
$o = Off $thunk
$ords = @()
while (($v = U32 $o) -ne 0) { $ords += ($v -band 0x7FFFFFFF); $o += 4 }
"  {0} imports: {1}" -f $ords.Count, (($ords | Sort-Object) -join ' ')
