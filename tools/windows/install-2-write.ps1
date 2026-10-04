# Arctic installed next to Windows (docs/install.md), step 2, as
# administrator: C: from the stick image onto the partition of step 1, the
# boot files ($Work\install-set, from tools/wsl/make-install-set.sh) into
# \EFI\Arctic on Windows' EFI system partition, a firmware boot entry
# "ARCTIC" (the boot menu, F11), and a copy of Claude's memory onto Arctic's C:.
param([string]$Work = "$env:TEMP\arctic-install",
      [string]$Image = 'E:\WORK_YT\project arctic\out\arctic-usb.img',
      [string]$BootSet = "$Work\install-set",
      [string]$Memory = 'C:\Users\mateusz\.claude\projects\e--WORK-YT-project-arctic\memory')
$ErrorActionPreference = 'Stop'
$Log = "$Work\install.log"
function Say($t) { "$(Get-Date -Format HH:mm:ss) $t" | Out-File -Append -Encoding utf8 $Log; Write-Host $t }
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawDev {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);
}
'@
try {
    $info = Get-Content "$Work\install-1.json" | ConvertFrom-Json
    $part = Get-Partition -DiskNumber $info.disk -PartitionNumber $info.partition
    if ($part.Guid.Trim('{}').ToLower() -ne $info.guid) { throw "partition $($info.partition) is not the one of step 1" }
    if (-not (Test-Path "$BootSet\EFI\Arctic\shimx64.efi")) { throw "no boot files in $BootSet" }

    # C: in the image: the second entry of its MBR
    $img = [IO.File]::OpenRead($Image)
    $mbr = New-Object byte[] 512
    [void]$img.Read($mbr, 0, 512)
    $start = [long][BitConverter]::ToUInt32($mbr, 446 + 16 + 8) * 512
    $length = [long][BitConverter]::ToUInt32($mbr, 446 + 16 + 12) * 512
    Say "C: in the image: $([math]::Round($length/1MB)) MB at $start; the partition: $([math]::Round($info.size/1GB,1)) GB"
    if ($length -gt $info.size) { throw "C: does not fit" }

    $vol = Get-Volume -Partition $part -ErrorAction SilentlyContinue
    if ($vol -and $vol.FileSystem) {
        Say "the partition holds $($vol.FileSystem) already: C: is not written again"
    } else {
        $path = ($part.AccessPaths | Where-Object { $_ -like '\\?\Volume*' } | Select-Object -First 1).TrimEnd('\') -replace '^\\\\\?\\', '\\.\'
        $h = [RawDev]::CreateFile($path, [uint32]3221225472, 3, [IntPtr]::Zero, 3, [uint32]2147483648, [IntPtr]::Zero)
        if ($h.IsInvalid) { throw "cannot open $path : $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
        $dev = New-Object IO.FileStream($h, [IO.FileAccess]::ReadWrite, 4096)
        $buf = New-Object byte[] (4MB)
        $first = New-Object byte[] 512
        $img.Position = $start; $done = 0; $last = -1
        while ($done -lt $length) {
            $n = $img.Read($buf, 0, [int][math]::Min([long]$buf.Length, [long]($length - $done)))
            if ($n -le 0) { throw "the image ended early" }
            if ($done -eq 0) {
                # the boot sector last: once it is there Windows mounts the volume
                [Array]::Copy($buf, 0, $first, 0, 512)
                $dev.Position = 512; $dev.Write($buf, 512, $n - 512)
            } else { $dev.Write($buf, 0, $n) }
            $done += $n
            $pct = [int](100 * $done / $length)
            if ($pct -ne $last -and $pct % 10 -eq 0) { Say "C: written $pct%"; $last = $pct }
        }
        # where the partition starts ("hidden sectors"), in the boot sector
        $hidden = [BitConverter]::GetBytes([uint32]($info.offset / 512))
        [Array]::Copy($hidden, 0, $first, 28, 4)
        $dev.Flush(); $dev.Position = 0; $dev.Write($first, 0, 512); $dev.Flush(); $dev.Close()
        Say "C: written"
    }
    $img.Close()
    Update-Disk -Number $info.disk
    Start-Sleep 3
    $part = Get-Partition -DiskNumber $info.disk -PartitionNumber $info.partition
    if (-not $part.DriveLetter) { Add-PartitionAccessPath -DiskNumber $info.disk -PartitionNumber $info.partition -AssignDriveLetter; Start-Sleep 2 }
    $letter = (Get-Partition -DiskNumber $info.disk -PartitionNumber $info.partition).DriveLetter
    $v = Get-Volume -DriveLetter $letter
    Say "Arctic's C: is $letter`: in Windows ($($v.FileSystem), $($v.FileSystemLabel))"

    # C:\Windows\Boot\Arctic holds the boot files of the version on C:, as
    # C:\Windows\Boot does in Windows: the image brings the stick's, an
    # installed Arctic has \EFI\Arctic. Updates and their undo write the EFI
    # system partition from there.
    $set = "$letter`:\Windows\Boot\Arctic"
    if (Test-Path $set) { Remove-Item -Recurse -Force $set }
    New-Item -ItemType Directory -Force "$set\EFI\Arctic" | Out-Null
    Copy-Item "$BootSet\EFI\Arctic\*" "$set\EFI\Arctic\" -Force
    Say "$set`: $((Get-ChildItem "$set\EFI\Arctic").Name -join ', ')"

    # the boot files, next to Windows Boot Manager
    $esp = Get-Partition | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' -and (Get-Disk -Number $_.DiskNumber).IsBoot }
    if (-not $esp) { $esp = Get-Partition | Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } | Select-Object -First 1 }
    & mountvol S: /s
    try {
        $free = (Get-PSDrive S).Free
        $need = (Get-ChildItem "$BootSet\EFI\Arctic" | Measure-Object Length -Sum).Sum
        Say "EFI system partition (disk $($esp.DiskNumber)): $([math]::Round($free/1MB,1)) MB free, the boot files take $([math]::Round($need/1MB,1)) MB"
        if (Test-Path 'S:\EFI\Arctic') { $need -= (Get-ChildItem 'S:\EFI\Arctic' | Measure-Object Length -Sum).Sum }
        if ($need + 2MB -gt $free) { throw "not enough room on the EFI system partition" }
        New-Item -ItemType Directory -Force 'S:\EFI\Arctic' | Out-Null
        Copy-Item "$BootSet\EFI\Arctic\*" 'S:\EFI\Arctic\' -Force
        Say "S:\EFI\Arctic: $((Get-ChildItem 'S:\EFI\Arctic').Name -join ', ')"
    } finally { & mountvol S: /d }

    # the firmware's boot entry "ARCTIC", as Windows Boot Manager's own
    $existing = (& bcdedit /enum firmware) -join "`n"
    $id = $null
    foreach ($block in ($existing -split "`n`n")) {
        if ($block -match 'description\s+ARCTIC' -and $block -match '(\{[0-9a-fA-F-]{36}\})') { $id = $Matches[1] }
    }
    if (-not $id) {
        $out = & bcdedit /copy '{bootmgr}' /d 'ARCTIC'
        if ($out -join ' ' -notmatch '(\{[0-9a-fA-F-]{36}\})') { throw "bcdedit /copy: $out" }
        $id = $Matches[1]
    }
    & bcdedit /set $id path '\EFI\Arctic\shimx64.efi' | Out-Null
    & bcdedit /set '{fwbootmgr}' displayorder $id /addlast | Out-Null
    Say "firmware entry ARCTIC: $id"
    Say ((& bcdedit /enum $id) -join ' | ')

    # Claude's memory, for the next session on Arctic
    $users = Get-ChildItem "$letter`:\Users" -Directory | Where-Object { $_.Name -notin 'Public', 'Default' }
    foreach ($u in $users) {
        $dest = "$($u.FullName)\.claude\projects\e--WORK-YT-project-arctic\memory"
        New-Item -ItemType Directory -Force $dest | Out-Null
        Copy-Item "$Memory\*" $dest -Force
        Say "memory copied to $dest ($((Get-ChildItem $dest).Count) files)"
    }
    Say "STEP2 DONE"
} catch {
    Say "FAILED: $_"
}
