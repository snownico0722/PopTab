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
    $folder = Join-Path $testRoot 'PopTabCompanion'
    $extension = Join-Path $folder 'PopTab-extension'
    $manifest = Get-Content (Join-Path $extension 'manifest.json') -Raw | ConvertFrom-Json
    Assert ($manifest.permissions -contains 'nativeMessaging') 'Missing bundled permission'
    Assert ($manifest.permissions -contains 'alarms') 'Missing reliable reconnect permission'
    $chromeId = 'abcdefghijklmnopabcdefghijklmnop'
    $edgeId = 'ponmlkjihgfedcbaponmlkjihgfedcba'
    $disabledId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $decoyId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    $secure = @{}
    $secure[$chromeId] = @{ state = 1; manifest = $manifest }
    $secure[$disabledId] = @{ state = 0; manifest = @{ name = 'PopTab'; commands = @{ 'toggle-clean-window' = @{} } } }
    $secure[$decoyId] = @{ state = 1; manifest = @{ name = 'Other extension' } }
    $profile = Join-Path $testRoot 'Google\Chrome\User Data\Default'
    Write-Json (Join-Path $profile 'Secure Preferences') @{ extensions = @{ settings = $secure } }
    $stale = @{}; $stale[$disabledId] = @{ state = 1; manifest = $manifest }
    Write-Json (Join-Path $profile 'Preferences') @{ extensions = @{ settings = $stale } }
    $unpacked = Join-Path $testRoot ('unpacked-' + [char]0x4E2D)
    Write-Json (Join-Path $unpacked 'manifest.json') $manifest
    $edge = @{}; $edge[$edgeId] = @{ path = $unpacked; disable_reasons = @() }
    Write-Json (Join-Path $testRoot 'Microsoft\Edge\User Data\Profile 1\Preferences') @{ extensions = @{ settings = $edge } }
    Run-Installer '--setup-silent' 0
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
    Run-Installer '--uninstall-silent' 0
    Assert (-not (Test-Path (Join-Path $folder 'native-host.json'))) 'Manifest remains'
    Assert (-not (Test-Path (Join-Path $folder 'PopTabCompanion.exe'))) 'Installed executable remains'
    Assert (Test-Path (Join-Path $extension 'manifest.json')) 'Uninstall removed the browser extension'
    Write-Host 'Installer discovery, Unicode path, disabled copy, live repair and uninstall passed.'
} finally {
    if ($hostProcess -and -not $hostProcess.HasExited) { $hostProcess.Kill() }
    foreach ($root in $roots) {
        $path = "$root\NativeMessagingHosts\com.poptab.titlepin"
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey($path, $false)
        if ($backup.ContainsKey($path)) {
            $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($path)
            $key.SetValue('', $backup[$path]); $key.Close()
        }
    }
    $env:LOCALAPPDATA = $originalLocal
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
