# Downloads the latest ISO built by CI (release "dev") into out\arctic.iso,
# replacing the previous one so only a single ISO is ever kept here.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$gh = 'C:\Users\admin\tools\gh\bin\gh.exe'
New-Item -ItemType Directory -Force $out | Out-Null

& $gh release view dev -R 77450danik/project-arctic --json name, publishedAt --jq '.name + "  " + .publishedAt'
& $gh release download dev -R 77450danik/project-arctic -p 'arctic.iso' -D $out --clobber
if ($LASTEXITCODE) { throw "download failed" }
Get-Item (Join-Path $out 'arctic.iso') | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB) } }, LastWriteTime
