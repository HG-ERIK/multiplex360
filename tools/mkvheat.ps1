# Finds the heaviest (usually highest-motion) stretches of an MKV on the Plex server by
# reading only its Cues index over HTTP: bytes between keyframes / time between them.
#   .\tools\mkvheat.ps1 -Url 'http://server:32400/library/parts/8118/123/file.mkv?X-Plex-Token=...' -Top 10
param([Parameter(Mandatory)] [string]$Url, [int]$Top = 10, [double]$Window = 30)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Net; using System.Collections.Generic;
public static class Mkv {
    public static byte[] Get(string url, long from, long count) {
        var rq = (HttpWebRequest)WebRequest.Create(url);
        rq.AddRange(from, from + count - 1);
        using (var rs = rq.GetResponse()) using (var s = rs.GetResponseStream()) using (var ms = new MemoryStream()) {
            s.CopyTo(ms); return ms.ToArray();
        }
    }
    // EBML variable-length integer; id=true keeps the length marker bits.
    public static long Vint(byte[] b, ref int p, bool id) {
        int first = b[p], len = 1, mask = 0x80;
        while (len <= 8 && (first & mask) == 0) { mask >>= 1; ++len; }
        long v = id ? first : (first & (mask - 1));
        for (int i = 1; i < len; ++i) v = (v << 8) | b[p + i];
        p += len;
        if (!id && v == (1L << (7 * len)) - 1) v = -1;   // unknown size
        return v;
    }
    public static long Uint(byte[] b, int p, long size) { long v = 0; for (int i = 0; i < size; ++i) v = (v << 8) | b[p + i]; return v; }

    public static List<long[]> Cues(string url, out double scaleNs) {
        byte[] h = Get(url, 0, 65536);
        int p = 0;
        long id = Vint(h, ref p, true), size = Vint(h, ref p, false); p += (int)size;   // EBML header
        id = Vint(h, ref p, true); size = Vint(h, ref p, false);                         // Segment
        if (id != 0x18538067) throw new Exception("not a Matroska segment");
        long segStart = p, cuesPos = -1; scaleNs = 1000000;
        while (p < h.Length - 12) {
            id = Vint(h, ref p, true); size = Vint(h, ref p, false);
            if (id == 0x114D9B74) {                       // SeekHead
                int e = p + (int)size;
                while (p < e) {
                    long sid = Vint(h, ref p, true), ssz = Vint(h, ref p, false); int se = p + (int)ssz;
                    long target = 0, pos = -1;
                    while (p < se) {
                        long cid = Vint(h, ref p, true), csz = Vint(h, ref p, false);
                        if (cid == 0x53AB) target = Uint(h, p, csz);
                        if (cid == 0x53AC) pos = Uint(h, p, csz);
                        p += (int)csz;
                    }
                    if (target == 0x1C53BB6B) cuesPos = pos;
                }
            } else if (id == 0x1549A966) {                // Info
                int e = p + (int)size;
                while (p < e) {
                    long cid = Vint(h, ref p, true), csz = Vint(h, ref p, false);
                    if (cid == 0x2AD7B1) scaleNs = Uint(h, p, csz);
                    p += (int)csz;
                }
            } else if (id == 0x1F43B675) break;           // first Cluster
            else p += (int)size;
        }
        if (cuesPos < 0) throw new Exception("no Cues in SeekHead");
        byte[] ch = Get(url, segStart + cuesPos, 16);
        int q = 0; Vint(ch, ref q, true); long cuesSize = Vint(ch, ref q, false);
        byte[] c = Get(url, segStart + cuesPos + q, cuesSize);
        var list = new List<long[]>();
        int r = 0;
        while (r < c.Length) {
            long cid = Vint(c, ref r, true), csz = Vint(c, ref r, false); int ce = r + (int)csz;
            if (cid != 0xBB) { r = ce; continue; }        // CuePoint
            long time = -1, track = 0, cluster = -1;
            while (r < ce) {
                long eid = Vint(c, ref r, true), esz = Vint(c, ref r, false);
                if (eid == 0xB3) { time = Uint(c, r, esz); r += (int)esz; }
                else if (eid == 0xB7) {
                    int te = r + (int)esz;
                    while (r < te) {
                        long tid = Vint(c, ref r, true), tsz = Vint(c, ref r, false);
                        if (tid == 0xF7) track = Uint(c, r, tsz);
                        if (tid == 0xF1) cluster = Uint(c, r, tsz);
                        r += (int)tsz;
                    }
                } else r += (int)esz;
            }
            if (time >= 0 && cluster >= 0 && track == 1) list.Add(new long[] { time, cluster });
        }
        return list;
    }
}
"@

$scale = 0.0
$cues = [Mkv]::Cues($Url, [ref]$scale)
$sec = $scale / 1e9
Write-Host ("{0} keyframes indexed" -f $cues.Count)

# Average Mbit/s over a sliding window starting at each keyframe.
$rows = @()
for ($i = 0; $i -lt $cues.Count; ++$i) {
    $t0 = $cues[$i][0] * $sec
    $j = $i
    while ($j + 1 -lt $cues.Count -and $cues[$j + 1][0] * $sec - $t0 -lt $Window) { ++$j }
    if ($j -eq $i) { continue }
    $dt = $cues[$j][0] * $sec - $t0
    $mbps = ($cues[$j][1] - $cues[$i][1]) * 8 / $dt / 1e6
    $rows += [pscustomobject]@{ Start = $t0; Mbps = $mbps }
}
$avg = ($rows | Measure-Object Mbps -Average).Average
Write-Host ("average over {0:N0}s windows: {1:N1} Mbit/s" -f $Window, $avg)

# Top windows that don't overlap each other.
$picked = @()
foreach ($r in ($rows | Sort-Object Mbps -Descending)) {
    if ($picked | Where-Object { [math]::Abs($_.Start - $r.Start) -lt $Window }) { continue }
    $picked += $r
    if ($picked.Count -ge $Top) { break }
}
$picked | ForEach-Object {
    [pscustomobject]@{ At = [TimeSpan]::FromSeconds([int]$_.Start).ToString(); Seconds = [int]$_.Start; Mbps = [math]::Round($_.Mbps, 1) }
} | Format-Table -AutoSize
