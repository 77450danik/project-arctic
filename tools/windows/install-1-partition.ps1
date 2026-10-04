# Arctic installed next to Windows (docs/install.md), step 1, as
# administrator: D: shrinks by $ArcticSize, and in the space a GPT partition
# is made for Arctic's C:, raw, without a letter yet. Its GUID goes to
# $Work\install-1.json for the boot files (tools/wsl/make-install-set.sh).
# It checks that D: is on the Crucial SSD of the laptop it was written for.
param([long]$ArcticSize = 100GB, [string]$Work = "$env:TEMP\arctic-install")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Work | Out-Null
$Log = "$Work\install.log"
$Json = "$Work\install-1.json"
function Say($t) { "$(Get-Date -Format HH:mm:ss) $t" | Out-File -Append -Encoding utf8 $Log; Write-Host $t }
try {
    $d = Get-Partition -DriveLetter D
    $disk = Get-Disk -Number $d.DiskNumber
    Say "D: is disk $($disk.Number) partition $($d.PartitionNumber): $($disk.FriendlyName), $($disk.PartitionStyle), $([math]::Round($d.Size/1GB,1)) GB"
    if ($disk.FriendlyName -notmatch 'CT1000MX500' -or $disk.PartitionStyle -ne 'GPT' -or $disk.IsBoot -or $disk.IsSystem) {
        throw "D: is not on the Crucial SSD as expected: nothing changed"
    }
    $arctic = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.PartitionNumber -gt $d.PartitionNumber }
    if ($arctic) {
        Say "a partition after D: is there already (number $($arctic.PartitionNumber)): used as it is"
        $arctic = $arctic | Select-Object -First 1
    } else {
        $supported = Get-PartitionSupportedSize -DriveLetter D
        $target = $d.Size - $ArcticSize
        Say "D: can shrink down to $([math]::Round($supported.SizeMin/1GB,1)) GB; it goes to $([math]::Round($target/1GB,1)) GB"
        if ($target -lt $supported.SizeMin) { throw "D: cannot shrink that far: nothing changed" }
        Resize-Partition -DriveLetter D -Size $target
        Say "D: is $([math]::Round((Get-Partition -DriveLetter D).Size/1GB,1)) GB"
        $arctic = New-Partition -DiskNumber $disk.Number -UseMaximumSize -GptType '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
    }
    $info = [ordered]@{
        disk = $disk.Number; partition = $arctic.PartitionNumber; guid = $arctic.Guid.Trim('{}').ToLower()
        offset = $arctic.Offset; size = $arctic.Size
    }
    $info | ConvertTo-Json | Out-File -Encoding ascii $Json
    Say "Arctic's partition: disk $($info.disk) partition $($info.partition), $([math]::Round($info.size/1GB,1)) GB, GUID $($info.guid)"
    Say "STEP1 DONE"
} catch {
    Say "FAILED: $_"
}
