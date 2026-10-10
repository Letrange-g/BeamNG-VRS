param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$GameFixture)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$temp = Join-Path ([IO.Path]::GetTempPath()) ('BeamNG launcher test ' + [Guid]::NewGuid())
$names = @('VK_INSTANCE_LAYERS', 'XR_ENABLE_API_LAYERS', 'BEAMNG_LAUNCHER_TEST_OUTPUT', 'BEAMNG_VRS_CENTER_RADIUS',
    'VK_LAYER_PATH', 'VK_ADD_LAYER_PATH')
$before = @{}
foreach ($name in $names) { $before[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Test-LaunchLayout {
    param([string]$Layers, [switch]$Diagnostics)
    $launcher = Join-Path $Layers 'Launch-BeamNG-VRS.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $result)) { throw 'CheckOnly failed or started the game.' }
    $diagnosticLog = $null
    if ($Diagnostics) {
        $messages = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -Diagnostics
        foreach ($message in $messages) {
            if ($message -match '^Vulkan loader diagnostics: (.+)$') { $diagnosticLog = $Matches[1] }
        }
    } else { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher }
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    # A closed file is required: the child might have just created it.
    $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    if ($content -notmatch 'arguments=-gfx vk' -or
        $content -notmatch 'VK_INSTANCE_LAYERS=VK_LAYER_test_existing;VK_LAYER_BEAMNG_vrs' -or
        $content -notmatch 'XR_ENABLE_API_LAYERS=XR_APILAYER_test_existing;XR_APILAYER_BEAMNG_eye_probe') {
        throw 'The child did not receive Vulkan arguments and layer activation.'
    }
    $vulkanPathName = if ($env:VK_LAYER_PATH) { 'VK_LAYER_PATH' } else { 'VK_ADD_LAYER_PATH' }
    $existingPath = [Environment]::GetEnvironmentVariable($vulkanPathName, 'Process')
    foreach ($binding in @(@('BEAMNG_VRS_CONFIG', (Join-Path $Layers 'BeamNG-VRS.ini')),
                           @($vulkanPathName, ($Layers + ';' + $existingPath)), @('XR_API_LAYER_PATH', $Layers),
                           @('working_directory', $temp))) {
        if (-not $content.Contains($binding[0] + '=' + $binding[1])) { throw "Incorrect child path: $($binding[0])." }
    }
    if (-not $env:VK_LAYER_PATH -and $content -notmatch '(?m)^VK_LAYER_PATH=\r?$') { throw 'Launcher replaced normal Vulkan layer discovery.' }
    if ($env:VK_ADD_LAYER_PATH -ne 'existing-add-path') { throw 'Launcher changed the parent Vulkan search path.' }
    if ($Diagnostics) {
        if ($content -notmatch 'VK_LOADER_DEBUG=error,warn,layer' -or $content -notmatch 'BEAMNG_VRS_LOG_LEVEL=2') {
            throw 'Diagnostic settings were not applied.'
        }
        if (-not $diagnosticLog -or (Get-Content -LiteralPath $diagnosticLog -Raw) -notmatch 'Vulkan loader diagnostic fixture') {
            throw 'Diagnostic stderr was not captured.'
        }
        Remove-Item -LiteralPath $diagnosticLog
    }
    if ($content -notmatch '(?m)^BEAMNG_VRS_CENTER_RADIUS=\r?$') { throw 'A legacy setting overrode the INI in the child.' }
    if ($env:VK_INSTANCE_LAYERS -ne 'VK_LAYER_test_existing' -or $env:XR_ENABLE_API_LAYERS -ne 'XR_APILAYER_test_existing' -or $env:BEAMNG_VRS_CENTER_RADIUS -ne '0.99') {
        throw 'Launcher changed the parent environment.'
    }
    Remove-Item -LiteralPath $result
}
try {
    $layers = Join-Path $temp 'BeamNG-VRS'
    $bin = Join-Path $temp 'Bin64'
    [void](New-Item -ItemType Directory -Path $layers, $bin)
    Copy-Item -LiteralPath $GameFixture -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll', 'BeamNG-VRS-Check.exe',
            'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json')) {
        Copy-Item -LiteralPath (Join-Path $BuildDirectory $file) -Destination $layers
    }
    Copy-Item -LiteralPath (Join-Path $source 'packaging/BeamNG-VRS.ini') -Destination $layers
    Copy-Item -LiteralPath (Join-Path $source 'packaging/Launch-BeamNG-VRS.ps1') -Destination $layers
    $result = Join-Path $temp 'result.txt'
    $env:BEAMNG_LAUNCHER_TEST_OUTPUT = $result
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_test_existing'
    $env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_test_existing'
    $env:BEAMNG_VRS_CENTER_RADIUS = '0.99'
    $env:VK_ADD_LAYER_PATH = 'existing-add-path'
    foreach ($layout in @('root', 'Bin64')) {
        if ($layout -eq 'Bin64') {
            Move-Item -LiteralPath $layers -Destination $bin
            $layers = Join-Path $bin 'BeamNG-VRS'
        }
        foreach ($executable in @('BeamNG.drive.x64.exe', 'BeamNG.drive.exe')) {
            $env:VK_LAYER_PATH = if ($executable -eq 'BeamNG.drive.exe') { 'existing-override-path' } else { $null }
            $game = Join-Path $bin $executable
            if ($executable -eq 'BeamNG.drive.exe') {
                Move-Item -LiteralPath (Join-Path $bin 'BeamNG.drive.x64.exe') -Destination $game
            }
            Test-LaunchLayout -Layers $layers
        }
        Move-Item -LiteralPath $game -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    }
    Test-LaunchLayout -Layers $layers -Diagnostics
    $launcher = Join-Path $layers 'Launch-BeamNG-VRS.ps1'
    Remove-Item -LiteralPath (Join-Path $bin 'BeamNG.drive.x64.exe')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Missing executable was not rejected.' }
    Copy-Item -LiteralPath $GameFixture -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    Set-Content -LiteralPath (Join-Path $layers 'BeamNG-VRS.ini') -Value "[Debug]`nDrawOutline=typo" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Invalid INI started the game.' }
    Write-Output 'Launcher integration passed: root/Bin64 layouts, both executable names, working directory, Vulkan discovery, diagnostic capture, arguments, child environment, CheckOnly, missing executable and invalid INI.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
