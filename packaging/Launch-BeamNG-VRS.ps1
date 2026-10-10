[CmdletBinding()]
param(
    [string]$GameExecutable,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
try {
    $layerDirectory = $PSScriptRoot
    $gameRoot = Split-Path -Parent $layerDirectory
    foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll', 'BeamNG-VRS-Check.exe',
            'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json', 'BeamNG-VRS.ini')) {
        if (-not (Test-Path -LiteralPath (Join-Path $layerDirectory $file) -PathType Leaf)) {
            throw "Missing $file. Extract the entire release ZIP into the BeamNG root folder."
        }
    }
    & (Join-Path $layerDirectory 'BeamNG-VRS-Check.exe') (Join-Path $layerDirectory 'BeamNG-VRS.ini')
    if ($LASTEXITCODE -ne 0) { throw 'Invalid settings. Fix BeamNG-VRS.ini using the error above, then try again.' }
    if (-not $GameExecutable) {
        foreach ($candidate in @('Bin64\BeamNG.drive.x64.exe', 'Bin64\BeamNG.drive.exe')) {
            $path = Join-Path $gameRoot $candidate
            if (Test-Path -LiteralPath $path -PathType Leaf) { $GameExecutable = $path; break }
        }
    }
    if (-not $GameExecutable -or -not (Test-Path -LiteralPath $GameExecutable -PathType Leaf)) {
        throw 'Game executable not found. Extract the ZIP beside the BeamNG Bin64 folder, or run this script with -GameExecutable followed by the full executable path.'
    }
    if ([IO.Path]::GetExtension($GameExecutable) -ne '.exe') { throw 'Select the BeamNG Windows executable (.exe).' }
    $GameExecutable = (Resolve-Path -LiteralPath $GameExecutable).Path
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $GameExecutable
    $start.WorkingDirectory = Split-Path -Parent (Split-Path -Parent $GameExecutable)
    $start.Arguments = '-gfx vk'
    $start.UseShellExecute = $false
    # The child receives its own environment. The shell and Windows settings are untouched.
    # Old testing scripts must not silently override the user's INI settings.
    foreach ($name in @($start.EnvironmentVariables.Keys)) {
        if ($name -like 'BEAMNG_VRS_*' -or $name -like 'BEAMNG_EYE_*') { $start.EnvironmentVariables.Remove($name) }
    }
    $start.EnvironmentVariables['BEAMNG_VRS_CONFIG'] = Join-Path $layerDirectory 'BeamNG-VRS.ini'
    foreach ($name in @('VK_LAYER_PATH', 'XR_API_LAYER_PATH')) {
        $existing = $start.EnvironmentVariables[$name]
        $start.EnvironmentVariables[$name] = if ($existing) { $layerDirectory + ';' + $existing } else { $layerDirectory }
    }
    foreach ($binding in @(@('VK_INSTANCE_LAYERS', 'VK_LAYER_BEAMNG_vrs'),
                           @('XR_ENABLE_API_LAYERS', 'XR_APILAYER_BEAMNG_eye_probe'))) {
        $existing = $start.EnvironmentVariables[$binding[0]]
        $layers = @($existing -split ';' | Where-Object { $_ })
        if ($layers -notcontains $binding[1]) { $layers += $binding[1] }
        $start.EnvironmentVariables[$binding[0]] = $layers -join ';'
    }
    if ($CheckOnly) { Write-Output 'Release files and game executable found. No game started.'; return }
    [void][System.Diagnostics.Process]::Start($start)
    Write-Output 'BeamNG launched in Vulkan mode. Change BeamNG-VRS.ini and restart to apply settings.'
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
