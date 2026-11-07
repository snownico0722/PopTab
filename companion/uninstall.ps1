# Remove the current user's native host registration and installed files.
[CmdletBinding()]
param(
    [ValidateSet('Chrome', 'Edge', 'Brave', 'All')]
    [string]$Browser = 'All'
)

$ErrorActionPreference = 'Stop'
$hostName = 'com.poptab.titlepin'
$roots = @{
    Chrome = 'Software\Google\Chrome\NativeMessagingHosts'
    Edge = 'Software\Microsoft\Edge\NativeMessagingHosts'
    Brave = 'Software\BraveSoftware\Brave-Browser\NativeMessagingHosts'
}
$selected = if ($Browser -eq 'All') { @('Chrome', 'Edge', 'Brave') } else { @($Browser) }
foreach ($name in $selected) {
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey(
        "$($roots[$name])\$hostName", $false
    )
    Write-Host "Unregistered $name"
}
if ($Browser -eq 'All') {
    $folder = Join-Path $env:LOCALAPPDATA 'PopTabCompanion'
    if (Test-Path -LiteralPath $folder) {
        Remove-Item -LiteralPath $folder -Force -Recurse
    }
}
Write-Host 'Reload PopTab to finish.'
