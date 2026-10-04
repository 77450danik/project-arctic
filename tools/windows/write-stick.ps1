# Writes the Arctic stick image onto USB disk $Number, byte for byte, then
# reads it back to compare. As administrator; the partition table is
# written last, so Windows mounts nothing half written. It checks that the
# disk is the ADATA stick it was written for.
param([int]$Number = 3, [string]$Image = 'E:\WORK_YT\project arctic\out\arctic-usb.img',
      [string]$Work = "$env:TEMP\arctic-install", [string]$Log = "$env:TEMP\arctic-install\write-stick.log")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Work | Out-Null
function Say($text) { "$(Get-Date -Format HH:mm:ss) $text" | Out-File -Append -Encoding utf8 $Log; Write-Host $text }
Remove-Item $Log -ErrorAction SilentlyContinue
try {
    $disk = Get-Disk -Number $Number
    $len = (Get-Item $Image).Length
    Say "disk $Number : $($disk.FriendlyName), $($disk.BusType), $([math]::Round($disk.Size/1GB,1)) GB; image $([math]::Round($len/1GB,2)) GB"
    if ($disk.FriendlyName -ne 'ADATA USB Flash Drive' -or $disk.BusType -ne 'USB' -or $disk.Size -gt 20GB -or $disk.IsBoot -or $disk.IsSystem) {
        throw "disk $Number is not the ADATA stick: nothing written"
    }
    if ($len -gt $disk.Size) { throw "the image is bigger than the stick" }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawDisk {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);
}
'@
    Set-Disk -Number $Number -IsReadOnly $false
    # No volumes left on it (Windows refuses raw writes into a mounted one).
    # Removable media cannot go offline, so the partition table goes, as Rufus does.
    # diskpart's clean dismounts what is mounted (Clear-Disk fails on a volume in use)
    "select disk $Number`nclean`nexit`n" | Out-File -Encoding ascii "$Work\clean.txt"
    $out = & diskpart /s "$Work\clean.txt" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "diskpart clean: $out" }
    Say "partitions removed, writing"
    # GENERIC_READ|GENERIC_WRITE, share read/write, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH
    # (PowerShell 5 reads 0xC0000000 as a negative int: the constants in decimal)
    $handle = [RawDisk]::CreateFile("\\.\PhysicalDrive$Number", [uint32]3221225472, 3, [IntPtr]::Zero, 3, [uint32]2147483648, [IntPtr]::Zero)
    if ($handle.IsInvalid) { throw "cannot open PhysicalDrive$Number : $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $dev = New-Object IO.FileStream($handle, [IO.FileAccess]::ReadWrite, 4096)
    $src = [IO.File]::OpenRead($Image)
    $chunk = 4MB
    $buf = New-Object byte[] $chunk
    $mbr = New-Object byte[] 512
    $done = 0; $lastPct = -1
    while (($n = $src.Read($buf, 0, $chunk)) -gt 0) {
        if ($n % 512) { $n += 512 - ($n % 512) }   # whole sectors
        if ($done -eq 0) {
            # the MBR last: once it is there Windows sees the partitions and mounts them
            [Array]::Copy($buf, 0, $mbr, 0, 512)
            $dev.Position = 512
            $dev.Write($buf, 512, $n - 512)
        } else {
            $dev.Write($buf, 0, $n)
        }
        $done += $n
        $pct = [int](100 * $done / $len)
        if ($pct -ne $lastPct -and $pct % 5 -eq 0) { Say "written $pct%"; $lastPct = $pct }
    }
    $dev.Flush()
    $dev.Position = 0
    $dev.Write($mbr, 0, 512)
    $dev.Flush()
    Say "written, checking"
    $dev.Position = 0; $src.Position = 0
    $back = New-Object byte[] $chunk
    $sha = [Security.Cryptography.SHA256]::Create()
    $done = 0; $lastPct = -1
    while (($n = $src.Read($buf, 0, $chunk)) -gt 0) {
        if ($n % 512) { $n += 512 - ($n % 512) }
        $m = $dev.Read($back, 0, $n)
        $a = [BitConverter]::ToString($sha.ComputeHash($buf, 0, $n))
        $b = [BitConverter]::ToString($sha.ComputeHash($back, 0, $n))
        if ($m -ne $n -or $a -ne $b) { throw "the stick differs from the image in the 4 MB at byte $done" }
        $done += $n
        $pct = [int](100 * $done / $len)
        if ($pct -ne $lastPct -and $pct % 10 -eq 0) { Say "checked $pct%"; $lastPct = $pct }
    }
    $src.Close(); $dev.Close()
    Set-Disk -Number $Number -IsOffline $false
    Update-Disk -Number $Number
    Say "DONE: the stick holds the image"
} catch {
    Say "FAILED: $_"
    try { Set-Disk -Number $Number -IsOffline $false } catch {}
}
