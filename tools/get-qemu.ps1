# Downloads QEMU for Windows into tools\qemu without running its installer
# (the installer wants admin rights; 7-Zip unpacks it just as well).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $PSScriptRoot 'qemu'
$tmp = Join-Path $root 'out'
New-Item -ItemType Directory -Force $tmp | Out-Null

$index = (Invoke-WebRequest -UseBasicParsing 'https://qemu.weilnetz.de/w64/').Content
$name = [regex]::Matches($index, 'qemu-w64-setup-\d+\.exe') | ForEach-Object Value | Sort-Object -Unique | Select-Object -Last 1
$setup = Join-Path $tmp $name
if (-not (Test-Path $setup)) {
    Write-Host "downloading $name"
    Invoke-WebRequest -UseBasicParsing "https://qemu.weilnetz.de/w64/$name" -OutFile $setup
}

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
& 'C:\Program Files\7-Zip\7z.exe' x -y -bso0 -bsp0 "-o$dest" $setup
if ($LASTEXITCODE) { throw "7-Zip failed: $LASTEXITCODE" }
Remove-Item -Recurse -Force (Join-Path $dest '$PLUGINSDIR') -ErrorAction SilentlyContinue
Remove-Item $setup
& (Join-Path $dest 'qemu-system-x86_64.exe') --version | Select-Object -First 1
