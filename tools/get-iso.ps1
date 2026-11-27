# Downloads the latest ISO built by CI (release "dev") into out\arctic.iso,
# replacing the previous one so only a single ISO is ever kept here.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$gh = 'C:\Users\admin\tools\gh\bin\gh.exe'
New-Item -ItemType Directory -Force $out | Out-Null

& $gh release view dev -R 77450danik/project-arctic --json name --jq .name
$assets = (& $gh release view dev -R 77450danik/project-arctic --json assets --jq '.assets[].name')
$iso = Join-Path $out 'arctic.iso'
if ($assets -contains 'arctic.iso') {
    & $gh release download dev -R 77450danik/project-arctic -p 'arctic.iso' -D $out --clobber
    if ($LASTEXITCODE) { throw "download failed" }
} else {
    $parts = $assets | Where-Object { $_ -match '^arctic\.iso\.\d{3}$' } | Sort-Object
    if (-not $parts) { throw "no arctic.iso or arctic.iso.* assets in release dev" }
    Remove-Item (Join-Path $out 'arctic.iso.*') -Force -ErrorAction SilentlyContinue
    & $gh release download dev -R 77450danik/project-arctic -p 'arctic.iso.*' -D $out --clobber
    if ($LASTEXITCODE) { throw "download failed" }
    $dst = [System.IO.File]::Create($iso)
    try {
        foreach ($p in $parts) {
            $srcPath = Join-Path $out $p
            $src = [System.IO.File]::OpenRead($srcPath)
            try { $src.CopyTo($dst) } finally { $src.Dispose() }
            Remove-Item $srcPath -Force
        }
    } finally { $dst.Dispose() }
}
Get-Item $iso | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB) } }, LastWriteTime
