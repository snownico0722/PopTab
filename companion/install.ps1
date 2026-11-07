# Native Messaging registration for the current Windows user; no administrator needed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-p]{32}$')]
    [string]$ExtensionId,

    [ValidateSet('Chrome', 'Edge', 'Brave', 'All')]
    [string]$Browser = 'Chrome',

    [string]$ExePath = (Join-Path $PSScriptRoot 'build\Release\PopTabCompanion.exe')
)

$ErrorActionPreference = 'Stop'
$hostName = 'com.poptab.titlepin'
$absoluteExe = (Resolve-Path -LiteralPath $ExePath -ErrorAction Stop).Path
$destination = Join-Path $env:LOCALAPPDATA 'PopTabCompanion'
New-Item -ItemType Directory -Path $destination -Force | Out-Null

$installedExe = Join-Path $destination 'PopTabCompanion.exe'
$manifestPath = Join-Path $destination 'native-host.json'
$origin = "chrome-extension://$ExtensionId/"

# Keep previously registered extension IDs when enabling another browser.
$origins = @($origin)
if (Test-Path -LiteralPath $manifestPath) {
    try {
        $old = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        foreach ($value in @($old.allowed_origins)) {
            if ($value -match '^chrome-extension://[a-p]{32}/$') {
                $origins += $value
            }
        }
    } catch {
        Write-Warning 'Existing host manifest was unreadable; replacing it.'
    }
}

Copy-Item -LiteralPath $absoluteExe -Destination $installedExe -Force
$manifest = @{
    name = $hostName
    description = 'PopTab Windows titlebar pin'
    path = $installedExe
    type = 'stdio'
    allowed_origins = @($origins | Select-Object -Unique)
} | ConvertTo-Json -Depth 5

# Windows PowerShell 5.1 normally adds a BOM. Keep Chromium JSON BOM-free.
[System.IO.File]::WriteAllText(
    $manifestPath, $manifest, (New-Object System.Text.UTF8Encoding($false))
)

$registryRoots = @{
    Chrome = 'Software\Google\Chrome\NativeMessagingHosts'
    Edge = 'Software\Microsoft\Edge\NativeMessagingHosts'
    Brave = 'Software\BraveSoftware\Brave-Browser\NativeMessagingHosts'
}
$selected = if ($Browser -eq 'All') { @('Chrome', 'Edge', 'Brave') } else { @($Browser) }
foreach ($name in $selected) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
        "$($registryRoots[$name])\$hostName"
    )
    if ($null -eq $key) { throw "Unable to register host for $name" }
    try {
        $key.SetValue('', $manifestPath, [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        $key.Close()
    }
    Write-Host "Registered PopTab Companion for $name"
}
Write-Host "Companion location: $installedExe"
Write-Host "Extension origin: $origin"
Write-Host 'Reload PopTab from the extensions page to establish the new permission.'
