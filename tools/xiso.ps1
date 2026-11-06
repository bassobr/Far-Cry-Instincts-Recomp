# Lists or extracts an Xbox XDVDFS image (XISO or full redump ISO).
#   powershell -File tools\xiso.ps1 -Iso game.iso            # list
#   powershell -File tools\xiso.ps1 -Iso game.iso -Out assets # extract
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [string]$Out
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class Xdvdfs {
    const int Sector = 2048;
    static readonly long[] PartitionOffsets = { 0, 0x18300000, 0x2080000, 0xFD90000 };

    public class Entry { public string Path; public uint Sector; public uint Size; public bool Dir; }

    public static List<Entry> Read(string iso, out long partition) {
        var list = new List<Entry>();
        using (var f = File.OpenRead(iso)) {
            partition = -1;
            foreach (long p in PartitionOffsets) {
                if (p + 0x10000 + 28 > f.Length) continue;
                f.Position = p + 0x10000;
                var b = new byte[28];
                f.Read(b, 0, 28);
                if (Encoding.ASCII.GetString(b, 0, 20) == "MICROSOFT*XBOX*MEDIA") {
                    partition = p;
                    Walk(f, p, BitConverter.ToUInt32(b, 20), BitConverter.ToUInt32(b, 24), "", list);
                    break;
                }
            }
            if (partition < 0) throw new InvalidDataException("no XDVDFS volume found");
        }
        return list;
    }

    static void Walk(FileStream f, long partition, uint sector, uint size, string prefix, List<Entry> list) {
        if (size == 0) return;
        var table = new byte[size];
        f.Position = partition + (long)sector * Sector;
        f.Read(table, 0, (int)size);
        var pending = new Stack<int>();
        var seen = new HashSet<int>();
        pending.Push(0);
        while (pending.Count > 0) {
            int off = pending.Pop();
            if (off + 14 > table.Length || !seen.Add(off)) continue;
            ushort left = BitConverter.ToUInt16(table, off);
            ushort right = BitConverter.ToUInt16(table, off + 2);
            if (left == 0xFFFF) continue; // padding
            uint start = BitConverter.ToUInt32(table, off + 4);
            uint len = BitConverter.ToUInt32(table, off + 8);
            byte attr = table[off + 12];
            int nameLen = table[off + 13];
            string name = Encoding.ASCII.GetString(table, off + 14, nameLen);
            if (left != 0) pending.Push(left * 4);
            if (right != 0) pending.Push(right * 4);
            var e = new Entry { Path = prefix + name, Sector = start, Size = len, Dir = (attr & 0x10) != 0 };
            list.Add(e);
            if (e.Dir) Walk(f, partition, start, len, e.Path + "\\", list);
        }
    }

    public static void Extract(string iso, long partition, List<Entry> entries, string outDir) {
        var buf = new byte[1 << 20];
        using (var f = File.OpenRead(iso)) {
            foreach (var e in entries) {
                string dest = Path.Combine(outDir, e.Path);
                if (e.Dir) { Directory.CreateDirectory(dest); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                f.Position = partition + (long)e.Sector * Sector;
                using (var o = File.Create(dest)) {
                    long left = e.Size;
                    while (left > 0) {
                        int n = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                        if (n <= 0) throw new EndOfStreamException(e.Path);
                        o.Write(buf, 0, n);
                        left -= n;
                    }
                }
            }
        }
    }
}
'@

$isoPath = (Resolve-Path $Iso).Path
$partition = 0L
$entries = [Xdvdfs]::Read($isoPath, [ref]$partition)
$entries = $entries | Sort-Object Path

if ($Out) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    [Xdvdfs]::Extract($isoPath, $partition, $entries, (Resolve-Path $Out).Path)
    "{0} entries extracted to {1}" -f $entries.Count, $Out
} else {
    $entries | ForEach-Object { "{0,12} {1}{2}" -f $_.Size, $_.Path, $(if ($_.Dir) { '\' } else { '' }) }
}
