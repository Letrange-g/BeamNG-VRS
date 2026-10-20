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
    foreach ($binding in @(@('BEAMNG_VRS_CONFIG', (Join-Path (Split-Path -Parent $Layers) 'BeamNG-VRS.ini')),
                           @($vulkanPathName, ($Layers + ';' + $existingPath)), @('XR_API_LAYER_PATH', $Layers),
                           @('working_directory', $temp))) {
        if (-not $content.Contains($binding[0] + '=' + $binding[1])) { throw "Incorrect child path: $($binding[0])." }
    }
    if (-not $env:VK_LAYER_PATH -and $content -notmatch '(?m)^VK_LAYER_PATH=\r?$') { throw 'Launcher replaced normal Vulkan layer discovery.' }
    if ($env:VK_ADD_LAYER_PATH -ne 'existing-add-path') { throw 'Launcher changed the parent Vulkan search path.' }
    if ($Diagnostics) {
        if ($content -notmatch 'VK_LOADER_DEBUG=error,warn,layer' -or $content -notmatch 'BEAMNG_VRS_LOG_LEVEL=2' -or
            $content -notmatch 'XR_LOADER_DEBUG=info') {
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
    foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll',
            'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json')) {
        Copy-Item -LiteralPath (Join-Path $BuildDirectory $file) -Destination $layers
    }
    $ini = Join-Path $temp 'BeamNG-VRS.ini'
    Copy-Item -LiteralPath (Join-Path $source 'packaging/BeamNG-VRS.ini') -Destination $ini
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
            Move-Item -LiteralPath $ini -Destination $bin
            $ini = Join-Path $bin 'BeamNG-VRS.ini'
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
    # Zero-sized quality regions must pass validation and reach Process.Start.
    Set-Content -LiteralPath $ini -Value "[Foveation]`nSize1x1=0`nSize2x2=0`nShape=ellipse`n[Debug]`nDrawOutline=false`n[Scissor]`nEnabled=false" -Encoding UTF8
    Test-LaunchLayout -Layers $layers
    $launcher = Join-Path $layers 'Launch-BeamNG-VRS.ps1'
    Set-Content -LiteralPath $ini -Value "[Scissor]`nEnabled=true`nTop=0.30`nBottom=0.30`nLeftEyeLeft=0.05`nLeftEyeRight=0`nRightEyeLeft=0`nRightEyeRight=0.10" -Encoding UTF8
    Test-LaunchLayout -Layers $layers
    Set-Content -LiteralPath $ini -Value "[Foveation]`nEnabled=false`n[Scissor]`nEnabled=true`n[Performance]`nTurboMode=true`n[Logging]`nLevel=off" -Encoding UTF8
    Test-LaunchLayout -Layers $layers
    # Listed implicit OpenXR layers are disabled in the child only, through their
    # own disable_environment; layers already off in the registry are left alone.
    $registry = 'HKCU:\Software\BeamNG-VRS-Test-' + [Guid]::NewGuid() + '\Implicit'
    try {
        [void](New-Item -Path $registry -Force)
        foreach ($layer in @(@('overlay', 0), @('off', 1))) {
            $manifest = Join-Path $temp ('XR_APILAYER_TEST_' + $layer[0] + '.json')
            Set-Content -LiteralPath $manifest -Encoding UTF8 -Value ('{"file_format_version":"1.0.0","api_layer":{"name":"XR_APILAYER_TEST_' + $layer[0] +
                '","library_path":"test.dll","api_version":"1.0","implementation_version":"1","description":"test","disable_environment":"DISABLE_XR_APILAYER_TEST_' + $layer[0] + '"}}')
            [void](New-ItemProperty -Path $registry -Name $manifest -PropertyType DWord -Value $layer[1])
        }
        Set-Content -LiteralPath $ini -Value "[OpenXR]`nDisableLayers=XR_APILAYER_TEST_overlay, XR_APILAYER_TEST_off" -Encoding UTF8
        $messages = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly -OpenXRLayerRegistry $registry
        if ($LASTEXITCODE -ne 0 -or ($messages -join "`n") -notmatch 'XR_APILAYER_TEST_overlay \[disabled for BeamNG' -or
            ($messages -join "`n") -notmatch 'XR_APILAYER_TEST_off \[off in registry\]') { throw 'CheckOnly did not report implicit OpenXR layers.' }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -OpenXRLayerRegistry $registry
        if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with implicit OpenXR layers.' }
        $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
        while ([DateTime]::UtcNow -lt $deadline) {
            try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'DISABLE_XR_APILAYER_TEST_off=') { break } } catch {}
            Start-Sleep -Milliseconds 100
        }
        if ($content -notmatch '(?m)^DISABLE_XR_APILAYER_TEST_overlay=1\r?$' -or $content -notmatch '(?m)^DISABLE_XR_APILAYER_TEST_off=\r?$') {
            throw 'Implicit OpenXR layer was not disabled for the child only.'
        }
        if ($env:DISABLE_XR_APILAYER_TEST_overlay) { throw 'Launcher changed the parent environment.' }
        Remove-Item -LiteralPath $result
    } finally { Remove-Item -LiteralPath (Split-Path -Parent $registry) -Recurse -Force -ErrorAction SilentlyContinue }
    # [Renderer] API=d3d12 starts BeamNG 0.40's Direct3D 12 renderer instead of Vulkan.
    Set-Content -LiteralPath $ini -Value "[Renderer]`nAPI=d3d12" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with the D3D12 renderer.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    if ($content -notmatch '(?m)^arguments=-gfx d3d12\r?$' -or $content -notmatch 'XR_ENABLE_API_LAYERS=.*XR_APILAYER_BEAMNG_eye_probe') {
        throw 'The child did not receive D3D12 arguments and OpenXR layer activation.'
    }
    Remove-Item -LiteralPath $result
    # The public launcher must not silently edit/use an obsolete nested INI.
    Move-Item -LiteralPath $ini -Destination $layers
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Missing root INI was not rejected.' }
    Move-Item -LiteralPath (Join-Path $layers 'BeamNG-VRS.ini') -Destination $ini
    Remove-Item -LiteralPath (Join-Path $bin 'BeamNG.drive.x64.exe')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Missing executable was not rejected.' }
    Copy-Item -LiteralPath $GameFixture -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    Set-Content -LiteralPath $ini -Value "[Debug]`nDrawOutline=typo" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Invalid INI started the game.' }
    Write-Output 'Launcher integration passed: root/Bin64 layouts, root INI, both executable names, working directory, Vulkan discovery, diagnostic capture, arguments, child environment, CheckOnly, implicit OpenXR layer disabling, D3D12 renderer, missing INI/executable and invalid INI.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
