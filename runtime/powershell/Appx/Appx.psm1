# Appx in Arctic: the packages a program adds with an external location
# (a sparse package: a manifest, the files where the program is installed),
# as installers made for Windows 11 add one for their File Explorer menu
# ("Open with ..."). The menu the package declares
# (windows.fileExplorerContextMenus) becomes the user's verbs of those item
# types, which run the package's program on the item: the Explorer command
# the package names would do the same, and Arctic's Explorer has the classic
# menu. The record is in HKCU\Software\Arctic\Appx, for Get-AppxPackage and
# Remove-AppxPackage.

Set-StrictMode -Version Latest
$script:RecordKey = 'Software\Arctic\Appx'
$script:Namespaces = @{
    f  = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'
    d4 = 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4'
    d5 = 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5'
}

function Read-Manifest([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Container) { $Path = Join-Path $Path 'AppxManifest.xml' }
    if ([IO.Path]::GetFileName($Path) -ieq 'AppxManifest.xml') { return [xml](Get-Content -LiteralPath $Path -Raw) }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.GetEntry('AppxManifest.xml')
        if (-not $entry) { throw "$Path has no AppxManifest.xml" }
        $reader = New-Object IO.StreamReader($entry.Open())
        try { return [xml]$reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $zip.Dispose() }
}

# Windows' publisher ID: the first 64 bits of SHA-256 of the publisher in
# UTF-16, in 13 letters of Crockford's base 32
function Get-PublisherId([string]$Publisher) {
    $hash = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::Unicode.GetBytes($Publisher))
    $bits = -join ($hash[0..7] | ForEach-Object { [Convert]::ToString($_, 2).PadLeft(8, '0') })
    $bits += '0'
    $alphabet = '0123456789abcdefghjkmnpqrstvwxyz'
    -join (0..12 | ForEach-Object { $alphabet[[Convert]::ToInt32($bits.Substring($_ * 5, 5), 2)] })
}

# the program of the package: in the external location or a folder or two
# above it, where installers keep the program and the package beside it
function Find-Program([string]$Root, [string]$Executable) {
    $dir = $Root
    for ($i = 0; $i -lt 3 -and $dir; $i++) {
        $candidate = Join-Path $dir $Executable
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        $dir = Split-Path $dir -Parent
    }
    return Join-Path $Root $Executable
}

function Get-VerbTitle([string]$Name) {
    if ((Get-UICulture).TwoLetterISOLanguageName -eq 'uk') { return "Відкрити за допомогою $Name" }
    return "Open with $Name"
}

function Remove-Record([string]$Name) {
    $records = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($script:RecordKey, $true)
    if (-not $records) { return }
    $record = $records.OpenSubKey($Name)
    if ($record) {
        $classes = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes', $true)
        foreach ($verb in @($record.GetValue('Verbs', @()))) {
            if ($classes -and $verb) { $classes.DeleteSubKeyTree($verb, $false) }
        }
        $record.Dispose()
        $records.DeleteSubKeyTree($Name, $false)
    }
    $records.Dispose()
}

function Add-AppxPackage {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)][Alias('PSPath')][string]$Path,
        [string]$ExternalLocation,
        [switch]$Register,
        [Parameter(ValueFromRemainingArguments = $true)]$Rest
    )
    $manifest = Read-Manifest $Path
    $ns = New-Object Xml.XmlNamespaceManager($manifest.NameTable)
    foreach ($prefix in $script:Namespaces.Keys) { $ns.AddNamespace($prefix, $script:Namespaces[$prefix]) }
    $identity = $manifest.SelectSingleNode('/f:Package/f:Identity', $ns)
    $name = $identity.GetAttribute('Name')
    $version = $identity.GetAttribute('Version')
    $publisher = $identity.GetAttribute('Publisher')
    $arch = $identity.GetAttribute('ProcessorArchitecture')
    if (-not $arch) { $arch = 'neutral' }
    if (-not $ExternalLocation) {
        if (-not $Register) { throw "Add-AppxPackage: $name is not a package with an external location; Arctic adds those only" }
        $ExternalLocation = Split-Path $Path -Parent
    }
    $root = (Resolve-Path -LiteralPath $ExternalLocation).ProviderPath
    $displayNode = $manifest.SelectSingleNode('/f:Package/f:Properties/f:DisplayName', $ns)
    $display = if ($displayNode) { $displayNode.InnerText } else { $name }
    $id = Get-PublisherId $publisher

    Remove-Record $name
    $classes = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Classes')
    $verbs = New-Object Collections.Generic.List[string]
    foreach ($app in $manifest.SelectNodes('/f:Package/f:Applications/f:Application', $ns)) {
        $program = Find-Program $root $app.GetAttribute('Executable')
        foreach ($type in $app.SelectNodes('.//d4:FileExplorerContextMenus/d5:ItemType', $ns)) {
            $itemType = $type.GetAttribute('Type')
            $arg = if ($itemType -ieq 'Directory\Background') { '%V' } else { '%1' }
            foreach ($verb in $type.SelectNodes('d5:Verb', $ns)) {
                $sub = "$itemType\shell\$name.$($verb.GetAttribute('Id'))"
                $key = $classes.CreateSubKey($sub)
                $key.SetValue('', (Get-VerbTitle $display))
                $key.SetValue('Icon', "`"$program`",0")
                $command = $key.CreateSubKey('command')
                $command.SetValue('', "`"$program`" `"$arg`"")
                $command.Dispose()
                $key.Dispose()
                $verbs.Add($sub)
            }
        }
    }
    $classes.Dispose()

    $record = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("$script:RecordKey\$name")
    $record.SetValue('PackageFullName', "${name}_${version}_${arch}__$id")
    $record.SetValue('PackageFamilyName', "${name}_$id")
    $record.SetValue('Version', $version)
    $record.SetValue('Publisher', $publisher)
    $record.SetValue('DisplayName', $display)
    $record.SetValue('InstallLocation', $root)
    $record.SetValue('Verbs', [string[]]$verbs.ToArray(), [Microsoft.Win32.RegistryValueKind]::MultiString)
    $record.Dispose()
}

function Get-AppxPackage {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)][string]$Name = '*',
        [Parameter(Position = 1)][string]$Publisher = '*',
        [switch]$AllUsers,
        [string]$User,
        [Parameter(ValueFromRemainingArguments = $true)]$Rest
    )
    $records = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($script:RecordKey)
    if (-not $records) { return }
    foreach ($n in $records.GetSubKeyNames()) {
        if ($n -notlike $Name) { continue }
        $r = $records.OpenSubKey($n)
        if ($r.GetValue('Publisher') -like $Publisher) {
            [pscustomobject]@{
                Name              = $n
                Publisher         = $r.GetValue('Publisher')
                Architecture      = ($r.GetValue('PackageFullName') -split '_')[2]
                Version           = $r.GetValue('Version')
                PackageFullName   = $r.GetValue('PackageFullName')
                InstallLocation   = $r.GetValue('InstallLocation')
                IsFramework       = $false
                PackageFamilyName = $r.GetValue('PackageFamilyName')
                PublisherId       = ($r.GetValue('PackageFamilyName') -split '_')[-1]
                SignatureKind     = 'Developer'
                Status            = 'Ok'
            }
        }
        $r.Dispose()
    }
    $records.Dispose()
}

function Remove-AppxPackage {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromPipelineByPropertyName = $true)][Alias('PackageFullName')][string]$Package,
        [switch]$AllUsers,
        [Parameter(ValueFromRemainingArguments = $true)]$Rest
    )
    process {
        foreach ($p in @(Get-AppxPackage)) {
            if ($p.PackageFullName -eq $Package -or $p.Name -eq $Package) { Remove-Record $p.Name }
        }
    }
}
