# PopTab native messaging host installer. No admin rights or ID lookup required.
# Auto mode finds PopTab extensions in browser profile Preferences files.
[CmdletBinding()]
param(
    [string]$ExtensionId = '',

    [ValidateSet('Auto', 'Chrome', 'Edge', 'Brave', 'All')]
    [string]$Browser = 'Auto',

    [string]$ExePath = ''
)

$ErrorActionPreference = 'Stop'

$hostName = 'com.poptab.titlepin'
$browserData = @{
    Chrome = (Join-Path $env:LOCALAPPDATA 'Google\Chrome\User Data')
    Edge   = (Join-Path $env:LOCALAPPDATA 'Microsoft\Edge\User Data')
    Brave  = (Join-Path $env:LOCALAPPDATA 'BraveSoftware\Brave-Browser\User Data')
}
$registryRoots = @{
    Chrome = 'Software\Google\Chrome\NativeMessagingHosts'
    Edge   = 'Software\Microsoft\Edge\NativeMessagingHosts'
    Brave  = 'Software\BraveSoftware\Brave-Browser\NativeMessagingHosts'
}

function Find-PopTabIds([string]$dataDir, [string]$browserName) {
    if (-not (Test-Path -LiteralPath $dataDir -PathType Container)) {
        return
    }

    $seen = @{}
    # Each profile stores installed extensions in its own Preferences file.
    # Do not recurse through browser caches, history or user web content.
    foreach ($profile in @(Get-ChildItem -LiteralPath $dataDir -Directory -ErrorAction SilentlyContinue)) {
        $preferencesPath = Join-Path $profile.FullName 'Preferences'
        if (-not (Test-Path -LiteralPath $preferencesPath -PathType Leaf)) {
            continue
        }

        try {
            # Chrome may have Preferences open: permit concurrent reads/writes.
            $stream = [System.IO.File]::Open(
                $preferencesPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                ([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
            )
            try {
                $reader = New-Object System.IO.StreamReader($stream)
                try {
                    $preferences = $reader.ReadToEnd() | ConvertFrom-Json
                } finally {
                    $reader.Dispose()
                }
            } finally {
                $stream.Dispose()
            }
        } catch {
            Write-Warning "Could not read $($browserName)/$($profile.Name) Preferences. Close the browser and run setup again."
            continue
        }

        $settings = $preferences.extensions.settings
        if ($null -eq $settings) {
            continue
        }

        foreach ($item in $settings.PSObject.Properties) {
            if ($item.Name -cnotmatch '^[a-p]{32}$') {
                continue
            }
            $manifest = $item.Value.manifest
            if ($null -eq $manifest) {
                continue
            }

            # Match two features that belong to this exact extension, not
            # an arbitrary extension with a similar display name.
            if ($manifest.name -cne 'PopTab' -or
                $null -eq $manifest.commands -or
                $null -eq $manifest.commands.'toggle-clean-window') {
                continue
            }

            $id = $item.Name
            if (-not $seen.ContainsKey($id)) {
                $seen[$id] = $true
                Write-Host "Found PopTab in $($browserName)/$($profile.Name): $id"
                if (@($manifest.permissions) -notcontains 'nativeMessaging') {
                    Write-Warning "This PopTab installation does not yet have nativeMessaging permission. Update/reload the extension from PR #5 before using the pin."
                }
            }
        }
    }

    foreach ($id in $seen.Keys) {
        Write-Output $id
    }
}

if ($ExtensionId -ne '' -and $ExtensionId -cnotmatch '^[a-p]{32}$') {
    throw 'ExtensionId must be exactly 32 lowercase letters from a to p.'
}

$selectedBrowsers = if ($Browser -eq 'Auto' -or $Browser -eq 'All') {
    @('Chrome', 'Edge', 'Brave')
} else {
    @($Browser)
}

$extensions = [ordered]@{}
foreach ($browserName in $selectedBrowsers) {
    $ids = if ($ExtensionId -ne '') {
        @($ExtensionId)
    } else {
        @(Find-PopTabIds $browserData[$browserName] $browserName)
    }
    if ($ids.Count -gt 0) {
        $extensions[$browserName] = $ids
    }
}

# No mutation on discovery failure. Users may simply need to load the
# packaged PopTab extension in chrome://extensions first.
if ($extensions.Count -eq 0) {
    throw @'
PopTab extension not found in Chrome, Edge or Brave profiles.
Load/enable the PopTab extension in your browser first, then double-click Setup.cmd.
If your browser uses a custom profile directory, use:
  install.ps1 -ExtensionId <32-letter-id> -Browser Chrome
'@
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $portableExe = Join-Path $PSScriptRoot 'PopTabCompanion.exe'
    $builtExe = Join-Path $PSScriptRoot 'build\Release\PopTabCompanion.exe'
    $ExePath = if (Test-Path -LiteralPath $portableExe) {
        $portableExe
    } else {
        $builtExe
    }
}
$absoluteExe = (Resolve-Path -LiteralPath $ExePath -ErrorAction Stop).Path

$destination = Join-Path $env:LOCALAPPDATA 'PopTabCompanion'
$installedExe = Join-Path $destination 'PopTabCompanion.exe'
$manifestPath = Join-Path $destination 'native-host.json'

$origins = @()
foreach ($browserName in $extensions.Keys) {
    foreach ($id in $extensions[$browserName]) {
        $origins += "chrome-extension://$id/"
    }
}

# Retain already-authorized PopTab IDs for additional browser profiles.
if (Test-Path -LiteralPath $manifestPath) {
    try {
        $old = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        foreach ($value in @($old.allowed_origins)) {
            if ($value -cmatch '^chrome-extension://[a-p]{32}/$') {
                $origins += $value
            }
        }
    } catch {
        Write-Warning 'Existing host manifest was unreadable; replacing it.'
    }
}

New-Item -ItemType Directory -Path $destination -Force | Out-Null
if (-not [string]::Equals($absoluteExe, $installedExe,
                          [System.StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $absoluteExe -Destination $installedExe -Force
}

$manifest = @{
    name = $hostName
    description = 'PopTab Windows titlebar pin'
    path = $installedExe
    type = 'stdio'
    allowed_origins = @($origins | Sort-Object -Unique)
} | ConvertTo-Json -Depth 5
# Windows PowerShell 5.1 writes a BOM by default. Chromium expects UTF-8 JSON.
[System.IO.File]::WriteAllText(
    $manifestPath, $manifest, (New-Object System.Text.UTF8Encoding($false))
)

foreach ($browserName in $extensions.Keys) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
        "$($registryRoots[$browserName])\$hostName"
    )
    if ($null -eq $key) {
        throw "Could not register Native Messaging host for $browserName"
    }
    try {
        $key.SetValue('', $manifestPath, [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        $key.Close()
    }
    Write-Host "Registered PopTab Companion for $browserName"
}

Write-Host ''
Write-Host 'Setup complete. No extension ID entry required.'
Write-Host "Executable: $installedExe"
Write-Host 'Reload PopTab in the extensions page if you just updated it, then open a popup.'
