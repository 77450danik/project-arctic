# Boots out\arctic.iso in QEMU. Uses the Windows Hypervisor Platform when it
# is available and falls back to plain emulation otherwise. Serial output goes
# to out\serial.log (that is where the ARCTIC: lines and the dev shell are).
param(
    [switch]$Uefi,
    [int]$MemoryMB = 4096,
    [int]$Cpus = 4,
    [switch]$Tcg
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$qemuDir = Join-Path $PSScriptRoot 'qemu'
$qemu = Join-Path $qemuDir 'qemu-system-x86_64.exe'
$iso = Join-Path $root 'out\arctic.iso'
$serial = Join-Path $root 'out\serial.log'

if (-not (Test-Path $qemu)) { throw "QEMU is missing: run tools\get-qemu.ps1" }
if (-not (Test-Path $iso)) { throw "ISO is missing: run tools\get-iso.ps1" }

$accel = if ($Tcg) { @('-accel', 'tcg') } else { @('-accel', 'whpx,kernel-irqchip=off', '-accel', 'tcg') }
$qargs = $accel + @(
    '-m', $MemoryMB, '-smp', $Cpus,
    '-cdrom', $iso,
    '-vga', 'std',
    '-serial', "file:$serial",
    '-display', 'gtk'
)
if ($Uefi) { $qargs += @('-bios', (Join-Path $qemuDir 'share\edk2-x86_64-code.fd')) }

Write-Host "qemu $($qargs -join ' ')"
& $qemu @qargs
