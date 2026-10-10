param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$GameFixture)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$temp = Join-Path ([IO.Path]::GetTempPath()) ('BeamNG launcher test ' + [Guid]::NewGuid())
$names = @('VK_INSTANCE_LAYERS', 'XR_ENABLE_API_LAYERS', 'BEAMNG_LAUNCHER_TEST_OUTPUT', 'BEAMNG_VRS_CENTER_RADIUS')
$before = @{}
foreach ($name in $names) { $before[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
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
    $launcher = Join-Path $layers 'Launch-BeamNG-VRS.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -CheckOnly
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $result)) { throw 'CheckOnly failed or started the game.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $result) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
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
    if (-not $content.Contains('BEAMNG_VRS_CONFIG=' + (Join-Path $layers 'BeamNG-VRS.ini'))) { throw 'INI path was lost.' }
    if ($content -notmatch '(?m)^BEAMNG_VRS_CENTER_RADIUS=\r?$') { throw 'A legacy setting overrode the INI in the child.' }
    if ($env:VK_INSTANCE_LAYERS -ne 'VK_LAYER_test_existing' -or $env:XR_ENABLE_API_LAYERS -ne 'XR_APILAYER_test_existing' -or $env:BEAMNG_VRS_CENTER_RADIUS -ne '0.99') {
        throw 'Launcher changed the parent environment.'
    }
    Remove-Item -LiteralPath $result
    Set-Content -LiteralPath (Join-Path $layers 'BeamNG-VRS.ini') -Value "[Debug]`nDrawOutline=typo" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Invalid INI started the game.' }
    Write-Output 'Launcher integration passed: discovery, arguments, child environment, CheckOnly and invalid INI.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
