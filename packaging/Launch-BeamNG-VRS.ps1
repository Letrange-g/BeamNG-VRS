[CmdletBinding()]
param(
    [string]$GameExecutable,
    [switch]$CheckOnly,
    [switch]$Diagnostics,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
function Assert-IniNumber {
    param([string]$Value, [double]$Low, [double]$High, [bool]$Integer)
    [double]$number = 0
    if (-not [double]::TryParse($Value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number) -or
        $number -lt $Low -or $number -gt $High -or ($Integer -and $Value -cnotmatch '^[0-9]+$')) {
        throw 'value is outside the allowed range'
    }
    return $number
}
function Test-BeamNGIni {
    param([string]$Path)
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    $text = $encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $whitespace = [char[]]" `t`r`n"
    $section = ''; $lineNumber = 0; $seen = @{}
    $sizes = @{'foveation.size1x1' = 0.25; 'foveation.size2x2' = 0.60}
    $ranges = @{
        'foveation.size1x1' = @(0, 1, $false)
        'foveation.size2x2' = @(0, 1, $false)
        'scissor.top' = @(0, 0.49, $false)
        'scissor.bottom' = @(0, 0.49, $false)
        'scissor.left' = @(0, 0.49, $false)
        'scissor.right' = @(0, 0.49, $false)
        'scissor.lefteyeleft' = @(0, 0.49, $false)
        'scissor.lefteyeright' = @(0, 0.49, $false)
        'scissor.righteyeleft' = @(0, 0.49, $false)
        'scissor.righteyeright' = @(0, 0.49, $false)
        'tracking.graceperiodms' = @(0, 1000, $true)
        'tracking.gazerefreshms' = @(2, 50, $true)
    }
    foreach ($line in ($text -split "`n")) {
        ++$lineNumber
        $line = $line.Trim($whitespace)
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        try {
            if ($line.StartsWith('[')) {
                if (-not $line.EndsWith(']')) { throw 'invalid section header' }
                $section = $line.Substring(1, $line.Length - 2).Trim($whitespace).ToLowerInvariant()
                if (@('foveation', 'debug', 'scissor', 'tracking', 'performance', 'logging', 'experimental') -cnotcontains $section) { throw 'unknown section' }
                continue
            }
            $equals = $line.IndexOf('=')
            if ($equals -lt 0) { throw 'expected key=value' }
            $key = $line.Substring(0, $equals).Trim($whitespace).ToLowerInvariant()
            $value = $line.Substring($equals + 1).Trim($whitespace)
            $id = $section + '.' + $key
            if ($seen.ContainsKey($id)) { throw 'duplicate setting' }
            $seen[$id] = $true
            if ($ranges.ContainsKey($id)) {
                $range = $ranges[$id]
                $number = Assert-IniNumber -Value $value -Low $range[0] -High $range[1] -Integer $range[2]
                if ($sizes.ContainsKey($id)) { $sizes[$id] = $number }
            } elseif ($id -eq 'foveation.shape') {
                if (@('circle', 'square', 'ellipse') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
            } elseif ($id -eq 'experimental.earlypasses') {
                if (@('false', 'true', '0', '1', 'off', 'trace', 'on') -cnotcontains $value.ToLowerInvariant()) { throw 'expected true or false (advanced: trace; legacy: on/off)' }
            } elseif ($id -eq 'logging.level') {
                if (@('off', 'errors', 'info', 'debug') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
            } elseif ($id -eq 'debug.drawoutline' -or $id -eq 'scissor.enabled' -or $id -eq 'foveation.enabled' -or $id -eq 'performance.turbomode') {
                if (@('0', '1', 'false', 'true') -cnotcontains $value.ToLowerInvariant()) { throw 'expected 0, 1, false or true' }
            } else { throw 'unknown section or setting' }
        } catch { throw "INI line ${lineNumber}: $($_.Exception.Message)" }
    }
    if ($seen.Count -eq 0) { throw 'INI contains no settings' }
    if ($sizes['foveation.size1x1'] -gt $sizes['foveation.size2x2']) { throw 'Size1x1 must not exceed Size2x2' }
    Write-Output 'BeamNG-VRS.ini is valid.'
}
try {
    $layerDirectory = $PSScriptRoot
    $installDirectory = Split-Path -Parent $layerDirectory
    $iniPath = Join-Path $installDirectory 'BeamNG-VRS.ini'
    if (-not (Test-Path -LiteralPath $iniPath -PathType Leaf)) {
        throw 'BeamNG-VRS.ini not found beside Launch-BeamNG-VRS.cmd. Extract the entire ZIP; when upgrading, move your saved INI beside the CMD.'
    }
    Test-BeamNGIni -Path $iniPath
    if ($ValidateOnly) { return }
    # Accept extraction at the game root or directly inside its Bin64 folder.
    $binaryDirectory = if ((Split-Path -Leaf $installDirectory) -ieq 'Bin64') {
        $installDirectory
    } else {
        Join-Path $installDirectory 'Bin64'
    }
    foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll',
            'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $layerDirectory $file) -PathType Leaf)) {
            throw "Missing $file. Extract the entire release ZIP into the BeamNG root folder."
        }
    }
    if (-not $GameExecutable) {
        foreach ($candidate in @('BeamNG.drive.x64.exe', 'BeamNG.drive.exe')) {
            $path = Join-Path $binaryDirectory $candidate
            if (Test-Path -LiteralPath $path -PathType Leaf) { $GameExecutable = $path; break }
        }
    }
    if (-not $GameExecutable -or -not (Test-Path -LiteralPath $GameExecutable -PathType Leaf)) {
        throw 'Game executable not found. Extract the ZIP into the BeamNG installation root or its Bin64 folder, or run this script with -GameExecutable followed by the full executable path.'
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
    $start.EnvironmentVariables['BEAMNG_VRS_CONFIG'] = $iniPath
    # VK_LAYER_PATH overrides registry discovery. Add our folder instead when
    # no explicit override exists; preserve an existing developer override.
    $vulkanPathName = if ($start.EnvironmentVariables['VK_LAYER_PATH']) { 'VK_LAYER_PATH' } else { 'VK_ADD_LAYER_PATH' }
    foreach ($name in @($vulkanPathName, 'XR_API_LAYER_PATH')) {
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
    if ($Diagnostics) {
        $start.EnvironmentVariables['VK_LOADER_DEBUG'] = 'error,warn,layer'
        $start.EnvironmentVariables['BEAMNG_VRS_LOG_LEVEL'] = '2'
        $start.EnvironmentVariables['BEAMNG_VRS_M4_DIAGNOSTICS'] = '1'
        $start.RedirectStandardError = $true
        $logPath = Join-Path ([IO.Path]::GetTempPath()) ('BeamNG_VRS_loader_' + [Guid]::NewGuid() + '.log')
        Write-Output "Vulkan loader diagnostics: $logPath"
        Write-Output 'Close BeamNG (and any error dialog), then share this log if launch fails.'
        $writer = New-Object IO.StreamWriter($logPath, $false, [Text.Encoding]::UTF8)
        try {
            $process = [System.Diagnostics.Process]::Start($start)
            while ($null -ne ($line = $process.StandardError.ReadLine())) {
                $writer.WriteLine($line)
                $writer.Flush()
            }
            $process.WaitForExit()
            Write-Output "BeamNG exited with code $($process.ExitCode). Diagnostics saved to $logPath"
        } finally { $writer.Dispose() }
        return
    }
    [void][System.Diagnostics.Process]::Start($start)
    Write-Output 'BeamNG launched in Vulkan mode. Change BeamNG-VRS.ini and restart to apply settings.'
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
