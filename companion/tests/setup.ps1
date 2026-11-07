# Windows PowerShell 5.1 end-to-end check of the single self-installing EXE.
$ErrorActionPreference = 'Stop'
$originalLocalAppData = $env:LOCALAPPDATA
$env:LOCALAPPDATA = Join-Path $env:TEMP ("PopTab-OneExe-CI-" + $PID)
New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force | Out-Null

$exe = Join-Path $PSScriptRoot '..\build\Release\PopTabCompanion.exe'
$chromeId = 'abcdefghijklmnopabcdefghijklmnop'
$edgeId = 'ponmlkjihgfedcbaponmlkjihgfedcba'
$decoyId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
$wrongId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$hostName = 'com.poptab.titlepin'

function Assert($test, [string]$message) {
    if (-not $test) { throw $message }
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

function Write-Preferences([string]$browser, [string]$profile, [hashtable]$settings) {
    $directory = Join-Path $env:LOCALAPPDATA "$browser\User Data\$profile"
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $json = @{
        extensions = @{ settings = $settings }
    } | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText(
        (Join-Path $directory 'Preferences'), $json, $utf8
    )
}

try {
    # No extension: installer should extract the bundled extension and tell
    # the user to load it (exit 7 in CI, no GUI message box).
    $first = Start-Process -FilePath $exe -ArgumentList '--setup-silent' -Wait -PassThru
    Assert ($first.ExitCode -eq 7) ("Expected onboarding status 7; got $($first.ExitCode)")
    $install = Join-Path $env:LOCALAPPDATA 'PopTabCompanion'
    $extension = Join-Path $install 'PopTab-extension'
    foreach ($filename in @(
        'manifest.json', 'background.js', 'native-companion.js',
        'video-float.js', 'assets\icons\icon-16.png',
        'assets\icons\icon-32.png', 'assets\icons\icon-48.png',
        'assets\icons\icon-128.png'
    )) {
        Assert (Test-Path -LiteralPath (Join-Path $extension $filename) -PathType Leaf) "Missing embedded extension asset: $filename"
    }
    $extensionManifest = Get-Content (Join-Path $extension 'manifest.json') -Raw | ConvertFrom-Json
    Assert ($extensionManifest.permissions -contains 'nativeMessaging') 'The embedded extension lacks Native Messaging permission'

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
    Write-Preferences 'Google\Chrome' 'Default' $chromeSettings
    Write-Preferences 'Microsoft\Edge' 'Profile 2' $edgeSettings

    $installed = Start-Process -FilePath $exe -ArgumentList '--setup-silent' -Wait -PassThru
    Assert ($installed.ExitCode -eq 0) ("Automated EXE setup failed: $($installed.ExitCode)")
    $manifestPath = Join-Path $install 'native-host.json'
    $host = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert ($host.name -eq $hostName) 'Native host name mismatch'
    Assert ($host.type -eq 'stdio') 'Native host type mismatch'
    Assert (@($host.allowed_origins).Count -eq 2) 'Wrong auto-discovered extension count'
    Assert ($host.allowed_origins -contains "chrome-extension://$chromeId/") 'Missing Chrome PopTab ID'
    Assert ($host.allowed_origins -contains "chrome-extension://$edgeId/") 'Missing Edge PopTab ID'
    Assert ($host.allowed_origins -notcontains "chrome-extension://$decoyId/") 'Decoy authorized'
    Assert ($host.allowed_origins -notcontains "chrome-extension://$wrongId/") 'Wrong command authorized'
    Assert (Test-Path -LiteralPath $host.path -PathType Leaf) 'Native host executable missing'

    $installedHash = (Get-FileHash -Algorithm SHA256 $host.path).Hash
    $sourceHash = (Get-FileHash -Algorithm SHA256 $exe).Hash
    Assert ($installedHash -eq $sourceHash) 'Installed EXE differs from the downloaded one'

    foreach ($browser in @('Google\Chrome', 'Microsoft\Edge')) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
            "Software\$browser\NativeMessagingHosts\$hostName"
        )
        Assert ($null -ne $key) "Missing registry key for $browser"
        Assert ($key.GetValue('') -eq $manifestPath) 'Registry manifest path mismatch'
        $key.Close()
    }

    # A second double click must not duplicate IDs or require reinstallation.
    $again = Start-Process -FilePath $exe -ArgumentList '--setup-silent' -Wait -PassThru
    Assert ($again.ExitCode -eq 0) ("Repeat setup failed: $($again.ExitCode)")
    $host = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert (@($host.allowed_origins).Count -eq 2) 'Repeat setup changed origin count'

    Write-Host 'One-file EXE extraction, discovery, registry and repeat-install tests passed.'
} finally {
    foreach ($root in @(
        'Software\Google\Chrome\NativeMessagingHosts',
        'Software\Microsoft\Edge\NativeMessagingHosts',
        'Software\BraveSoftware\Brave-Browser\NativeMessagingHosts'
    )) {
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey(
            "$root\$hostName", $false
        )
    }
    $env:LOCALAPPDATA = $originalLocalAppData
    Remove-Item -LiteralPath (Join-Path $env:TEMP ("PopTab-OneExe-CI-" + $PID)) -Recurse -Force -ErrorAction SilentlyContinue
}
