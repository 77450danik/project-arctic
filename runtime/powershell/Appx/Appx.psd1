@{
    RootModule           = 'Appx.psm1'
    ModuleVersion        = '1.0.0'
    GUID                 = '7d6f3c1e-2b4a-4e8f-9a51-3c2d8e6f0b17'
    Author               = 'Arctic'
    Description          = 'Packages with an external location (sparse packages): their File Explorer context menus.'
    CompatiblePSEditions = @('Core', 'Desktop')
    PowerShellVersion    = '5.1'
    FunctionsToExport    = @('Add-AppxPackage', 'Get-AppxPackage', 'Remove-AppxPackage')
    CmdletsToExport      = @()
    AliasesToExport      = @()
}
