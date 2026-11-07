# Isolated current-user install/repair/uninstall test, including real Chromium preference shapes.
$ErrorActionPreference = 'Stop'
$originalLocal = $env:LOCALAPPDATA
$testRoot = Join-Path $env:TEMP ("PopTab-setup-" + $PID)
$env:LOCALAPPDATA = $testRoot
$exe = (Resolve-Path (Join-Path $PSScriptRoot '..\build\Release\PopTabCompanion.exe')).Path
$roots = @('Software\Google\Chrome', 'Software\Microsoft\Edge', 'Software\BraveSoftware\Brave-Browser')
$backup = @{}
foreach ($root in $roots) {
    $path = "$root\NativeMessagingHosts\com.poptab.titlepin"
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path)
    if ($key) { $backup[$path] = $key.GetValue(''); $key.Close() }
}
function Assert($ok, [string]$message) { if (-not $ok) { throw $message } }
Add-Type -Namespace PopTabTest -Name Window -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindow(string className, string title);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr SendMessage(System.IntPtr window, uint message, System.IntPtr wParam, System.IntPtr lParam);
'@
function Tray-Window {
    return [PopTabTest.Window]::FindWindow('PopTabCompanionTrayWindow', 'PopTab Companion')
}
function Wait-Tray {
    for ($try = 0; $try -lt 60; $try++) {
        $window = Tray-Window
        if ($window -ne [System.IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 100
    }
    throw 'Tray process did not start'
}
function Startup-Entry {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    if ($null -eq $key) { return $null }
    try { return $key.GetValue('PopTabCompanion', $null) } finally { $key.Close() }
}
function Run-Installer([string]$argument, [int]$expected) {
    Write-Host "Starting $argument; expecting $expected"
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = $argument
    $start.UseShellExecute = $false
    $process = [System.Diagnostics.Process]::Start($start)
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        throw "Installer timed out: $argument"
    }
    Assert ($process.ExitCode -eq $expected) "Expected $expected; got $($process.ExitCode)"
    Write-Host "Finished $argument with $($process.ExitCode)"
}
function Write-Json([string]$path, $value) {
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    $json = ($value | ConvertTo-Json -Depth 20).Replace([string][char]0x4E2D, '\u4e2d')
    [IO.File]::WriteAllText($path, $json, (New-Object System.Text.UTF8Encoding($false)))
}
try {
    Run-Installer '--setup-silent' 7
    Assert ((Tray-Window) -eq [IntPtr]::Zero) 'Incomplete setup must not start tray'
    Assert ((Startup-Entry) -eq $null) 'Incomplete setup must not register auto-start'
    $folder = Join-Path $testRoot 'PopTabCompanion'
    $extension = Join-Path $folder 'PopTab-extension'
    $manifest = Get-Content (Join-Path $extension 'manifest.json') -Raw | ConvertFrom-Json
    Assert ($manifest.permissions -contains 'nativeMessaging') 'Missing bundled permission'
    Assert ($manifest.permissions -contains 'alarms') 'Missing reliable reconnect permission'
    $chromeId = 'abcdefghijklmnopabcdefghijklmnop'
    $edgeId = 'ponmlkjihgfedcbaponmlkjihgfedcba'
    $disabledId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $decoyId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    $legacyId = 'cccccccccccccccccccccccccccccccc'
    $secure = @{}
    $secure[$chromeId] = @{ state = 1; manifest = $manifest }
    $secure[$disabledId] = @{ state = 0; manifest = @{ name = 'PopTab'; commands = @{ 'toggle-clean-window' = @{} } } }
    $secure[$decoyId] = @{ state = 1; manifest = @{ name = 'Other extension' } }
    $secure[$legacyId] = @{ state = 1; manifest = @{ name = 'PopTab'; permissions = @('storage', 'nativeMessaging'); commands = @{ 'toggle-clean-window' = @{} } } }
    $profile = Join-Path $testRoot 'Google\Chrome\User Data\Default'
    Write-Json (Join-Path $profile 'Secure Preferences') @{ extensions = @{ settings = $secure } }
    $stale = @{}; $stale[$disabledId] = @{ state = 1; manifest = $manifest }
    Write-Json (Join-Path $profile 'Preferences') @{ extensions = @{ settings = $stale } }
    $unpacked = Join-Path $testRoot ('unpacked-' + [char]0x4E2D)
    Write-Json (Join-Path $unpacked 'manifest.json') $manifest
    $edge = @{}; $edge[$edgeId] = @{ path = $unpacked; disable_reasons = @() }
    Write-Json (Join-Path $testRoot 'Microsoft\Edge\User Data\Profile 1\Preferences') @{ extensions = @{ settings = $edge } }
    Run-Installer '--setup-silent' 0
    $tray = Wait-Tray
    Assert ((Startup-Entry) -match '\-\-tray') 'Default auto-start is not enabled'
    $nativeManifest = Get-Content (Join-Path $folder 'native-host.json') -Raw | ConvertFrom-Json
    Assert ($nativeManifest.allowed_origins.Count -eq 2) 'Wrong authorized extension count'
    foreach ($id in @($chromeId, $edgeId)) {
        Assert ($nativeManifest.allowed_origins -contains "chrome-extension://$id/") "Missing extension $id"
    }
    Assert ((Get-FileHash $exe).Hash -eq (Get-FileHash $nativeManifest.path).Hash) 'Installed executable differs'
    Write-Host 'Starting the installed native host for live repair'
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $nativeManifest.path
    $start.Arguments = '--host-test'
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $hostProcess = [System.Diagnostics.Process]::Start($start)
    $header = New-Object byte[] 4
    $read = $hostProcess.StandardOutput.BaseStream.ReadAsync($header, 0, 4)
    Assert ($read.Wait(5000)) 'Host did not send ready within five seconds'
    Assert ($read.Result -eq 4) 'Host did not start'
    Run-Installer '--setup-silent' 0
    Assert ($hostProcess.WaitForExit(5000)) 'Repair did not stop the old host'
    Assert ($hostProcess.ExitCode -eq 0) 'Old host did not exit cleanly'
    $tray = Wait-Tray
    # The user can disable startup without disabling the running service.
    [PopTabTest.Window]::SendMessage($tray, 0x0111, [IntPtr]401, [IntPtr]::Zero) | Out-Null
    Assert ((Startup-Entry) -eq $null) 'Unchecking auto-start did not remove Run entry'
    Run-Installer '--setup-silent' 0
    Assert ((Startup-Entry) -eq $null) 'Repair must preserve disabled auto-start'
    $tray = Wait-Tray
    [PopTabTest.Window]::SendMessage($tray, 0x0111, [IntPtr]401, [IntPtr]::Zero) | Out-Null
    Assert ((Startup-Entry) -match '\-\-tray') 'Re-enabling auto-start failed'
    # Exit must stop the tray without changing the chosen startup preference.
    [PopTabTest.Window]::SendMessage($tray, 0x0111, [IntPtr]403, [IntPtr]::Zero) | Out-Null
    for ($try = 0; $try -lt 30 -and (Tray-Window) -ne [IntPtr]::Zero; $try++) {
        Start-Sleep -Milliseconds 100
    }
    Assert ((Tray-Window) -eq [IntPtr]::Zero) 'Tray Exit did not stop the service'
    Assert ((Startup-Entry) -match '\-\-tray') 'Tray Exit changed auto-start unexpectedly'
    # Launching the installed EXE in tray mode must work without reinstalling.
    Start-Process -FilePath $nativeManifest.path -ArgumentList '--tray' | Out-Null
    $tray = Wait-Tray
    Assert ($tray -ne [IntPtr]::Zero) 'Failed to reopen tray after Exit'
    Assert ((Startup-Entry) -match '\-\-tray') 'Restarting tray changed auto-start unexpectedly'
    Run-Installer '--uninstall-silent' 0
    Assert ((Startup-Entry) -eq $null) 'Uninstall did not remove auto-start'
    Assert (-not (Test-Path (Join-Path $folder 'native-host.json'))) 'Manifest remains'
    Assert (-not (Test-Path (Join-Path $folder 'PopTabCompanion.exe'))) 'Installed executable remains'
    Assert (Test-Path (Join-Path $extension 'manifest.json')) 'Uninstall removed the browser extension'
    Write-Host 'Installer discovery, Unicode path, disabled copy, live repair and uninstall passed.'
} finally {
    $remainingTray = Tray-Window
    if ($remainingTray -ne [IntPtr]::Zero) {
        [PopTabTest.Window]::SendMessage($remainingTray, 0x0111, [IntPtr]403, [IntPtr]::Zero) | Out-Null
    }
    if ($hostProcess -and -not $hostProcess.HasExited) { $hostProcess.Kill() }
    foreach ($root in $roots) {
        $path = "$root\NativeMessagingHosts\com.poptab.titlepin"
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($path, $false)
        if ($backup.ContainsKey($path)) {
            $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($path)
            $key.SetValue('', $backup[$path]); $key.Close()
        }
    }
    $run = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run', $true)
    if ($run) { $run.DeleteValue('PopTabCompanion', $false); $run.Close() }
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey('Software\PopTabCompanion', $false)
    $env:LOCALAPPDATA = $originalLocal
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
