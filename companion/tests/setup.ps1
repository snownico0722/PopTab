# Windows PowerShell 5.1 regression test for automatic PopTab ID discovery.
$ErrorActionPreference = 'Stop'

$chromeId = 'abcdefghijklmnopabcdefghijklmnop'
$edgeId = 'ponmlkjihgfedcbaponmlkjihgfedcba'
$decoyId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
$wrongId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
$hostName = 'com.poptab.titlepin'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$testDirectories = @()

function New-Preferences([string]$root, [string]$profile, [hashtable]$settings) {
    $directory = Join-Path $root $profile
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $script:testDirectories += $directory
    $file = Join-Path $directory 'Preferences'
    $json = @{
        extensions = @{
            settings = $settings
        }
    } | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText($file, $json, $script:utf8)
}

function New-PopTabEntry {
    return @{
        manifest = @{
            name = 'PopTab'
            commands = @{ 'toggle-clean-window' = @{ description = 'Open PopTab' } }
            permissions = @('storage', 'nativeMessaging')
        }
        state = 1
    }
}

function Assert($ok, [string]$message) {
    if (-not $ok) { throw $message }
}

$chromeRoot = Join-Path $env:LOCALAPPDATA 'Google\Chrome\User Data'
$edgeRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\Edge\User Data'
$chromeSettings = @{}
$chromeSettings[$chromeId] = New-PopTabEntry
$chromeSettings[$decoyId] = @{
    manifest = @{ name = 'Unrelated'; commands = @{ 'toggle-clean-window' = @{} } }
}
$chromeSettings[$wrongId] = @{
    manifest = @{ name = 'PopTab'; commands = @{ 'not-the-poptab-command' = @{} } }
}
$edgeSettings = @{}
$edgeSettings[$edgeId] = New-PopTabEntry

try {
    # Multiple installed Chromium browsers, plus lookalikes to reject.
    New-Preferences $chromeRoot 'Profile PopTab Auto CI' $chromeSettings
    New-Preferences $edgeRoot 'Profile PopTab Auto CI' $edgeSettings

    $installer = Join-Path $PSScriptRoot '..\dist\install.ps1'
    $uninstaller = Join-Path $PSScriptRoot '..\dist\uninstall.ps1'
    & $installer
    if (-not $?) { throw 'Automatic installer returned failure' }

    $manifestPath = Join-Path $env:LOCALAPPDATA 'PopTabCompanion\native-host.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert ($manifest.name -eq $hostName) 'Host name mismatch'
    Assert ($manifest.type -eq 'stdio') 'Host type mismatch'
    Assert (@($manifest.allowed_origins).Count -eq 2) 'Wrong number of auto-discovered extension IDs'
    Assert ($manifest.allowed_origins -contains "chrome-extension://$chromeId/") 'Chrome extension missing'
    Assert ($manifest.allowed_origins -contains "chrome-extension://$edgeId/") 'Edge extension missing'
    Assert ($manifest.allowed_origins -notcontains "chrome-extension://$decoyId/") 'Lookalike extension was mistakenly authorized'
    Assert ($manifest.allowed_origins -notcontains "chrome-extension://$wrongId/") 'Non-PopTab ID was mistakenly authorized'
    Assert (Test-Path -LiteralPath $manifest.path -PathType Leaf) 'Executable was not installed'

    foreach ($browser in @('Google\Chrome', 'Microsoft\Edge')) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
            "Software\$browser\NativeMessagingHosts\$hostName"
        )
        Assert ($null -ne $key) "$browser registration missing"
        Assert ($key.GetValue('') -eq $manifestPath) "$browser registration path incorrect"
        $key.Close()
    }

    # The old explicit-ID command still works and is idempotent.
    & $installer -ExtensionId $chromeId -Browser Chrome
    if (-not $?) { throw 'Manual compatibility path failed' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert (@($manifest.allowed_origins).Count -eq 2) 'Reinstall duplicated or discarded extension ID'

    Write-Host 'Automatic profile discovery and registration smoke test passed.'
} finally {
    & (Join-Path $PSScriptRoot '..\dist\uninstall.ps1') -Browser All
    foreach ($directory in $testDirectories) {
        if (Test-Path -LiteralPath $directory) {
            Remove-Item -LiteralPath $directory -Recurse -Force
        }
    }
}
