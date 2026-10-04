param([string]$Drive = 'F:')
# How fast the stick reads and writes: sequentially, and in random 4 KB blocks
# (what a registry save, a browser profile, a log line is). Reads host.sqfs;
# writes a temp file that it deletes afterwards.
$ErrorActionPreference = 'Stop'
$read = [IO.File]::Open("$Drive\Windows\System32\Host\host.sqfs", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$buf = New-Object byte[] (4MB)
$sw = [Diagnostics.Stopwatch]::StartNew()
$read.Position = 700MB; $total = 0
while ($total -lt 200MB) { $n = $read.Read($buf, 0, $buf.Length); if ($n -le 0) { break }; $total += $n }
"sequential read : {0,7:N1} MB/s" -f ($total / 1MB / $sw.Elapsed.TotalSeconds)
$small = New-Object byte[] 4096; $rnd = New-Object Random 7
$sw.Restart()
for ($i = 0; $i -lt 300; $i++) { $read.Position = [long]($rnd.Next(0, 140000)) * 4096; [void]$read.Read($small, 0, 4096) }
"random 4K read  : {0,7:N0} per s ({1:N1} ms each)" -f (300 / $sw.Elapsed.TotalSeconds), ($sw.Elapsed.TotalMilliseconds / 300)
$read.Close()

$tmp = "$Drive\arctic-bench.tmp"
$w = New-Object IO.FileStream($tmp, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 4096, [IO.FileOptions]::WriteThrough)
try {
    $rnd.NextBytes($buf)
    $sw.Restart()
    for ($i = 0; $i -lt 16; $i++) { $w.Write($buf, 0, $buf.Length) }
    $w.Flush($true)
    "sequential write: {0,7:N1} MB/s" -f (64 / $sw.Elapsed.TotalSeconds)
    $sw.Restart(); $count = 0
    while ($sw.Elapsed.TotalSeconds -lt 15 -and $count -lt 300) {
        $w.Position = [long]($rnd.Next(0, 16384)) * 4096
        $w.Write($small, 0, 4096); $w.Flush($true); $count++
    }
    "random 4K write : {0,7:N1} per s ({1:N0} ms each, synced)" -f ($count / $sw.Elapsed.TotalSeconds), ($sw.Elapsed.TotalMilliseconds / $count)
} finally {
    $w.Close()
    Remove-Item $tmp
}
Get-CimInstance Win32_DiskDrive | Where-Object Model -match 'ADATA' | ForEach-Object { "drive: $($_.Model), $($_.PNPDeviceID)" }
