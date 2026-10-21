[CmdletBinding()]
param(
    [string]$GameExecutable,
    [switch]$CheckOnly,
    [switch]$Diagnostics,
    [switch]$ValidateOnly,
    # BeamNG user folder for the RenderScale mod; tests use a scratch folder.
    [string]$UserFolder = (Join-Path $env:LOCALAPPDATA 'BeamNG\BeamNG.drive\current'),
    # Implicit OpenXR layer registry keys; tests replace them with a scratch key.
    [string[]]$OpenXRLayerRegistry = @('HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit',
                                       'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit')
)
$ErrorActionPreference = 'Stop'
function Assert-IniNumber {
    param([string]$Value, [double]$Low, [double]$High, [bool]$Integer)
    [double]$number = 0
    if (-not [double]::TryParse($Value, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number) -or
        $number -lt $Low -or $number -gt $High -or ($Integer -and $Value -cnotmatch '^[0-9]+$')) {
        $inv = [Globalization.CultureInfo]::InvariantCulture
        throw "value '$Value' is outside the allowed range ($($Low.ToString($inv)) to $($High.ToString($inv)))"
    }
    return $number
}
# Comma-separated implicit OpenXR layer names, or * for all of them. Empty keeps every layer.
function ConvertTo-OpenXRLayerList {
    param([string]$Value)
    if (-not $Value) { return }
    if ($Value -eq '*') { return '*' }
    foreach ($name in ($Value -split ',')) {
        $name = $name.Trim([char[]]" `t")
        if ($name -cnotmatch '^[A-Za-z0-9_.-]+$') { throw 'expected OpenXR layer names separated by commas, or *' }
        $name
    }
}
# Implicit layers load into every OpenXR game before explicit ones, so this
# project's layer cannot shield BeamNG from them. Their manifests must name a
# disable_environment variable, which turns a layer off for one process only.
function Get-OpenXRImplicitLayers {
    param([string[]]$RegistryKeys)
    foreach ($key in $RegistryKeys) {
        if (-not (Test-Path -LiteralPath $key)) { continue }
        $item = Get-Item -LiteralPath $key
        foreach ($manifest in $item.GetValueNames()) {
            $layer = [pscustomobject]@{ Name = $null; Manifest = $manifest; Enabled = ($item.GetValue($manifest) -eq 0)
                                        DisableEnvironment = $null; EnableEnvironment = $null }
            try {
                $json = [IO.File]::ReadAllText($manifest) | ConvertFrom-Json
                $layer.Name = [string]$json.api_layer.name
                $layer.DisableEnvironment = [string]$json.api_layer.disable_environment
                $layer.EnableEnvironment = [string]$json.api_layer.enable_environment
            } catch {}
            $layer
        }
    }
}
# M8.48: BeamNG 0.40 forces FSR at 50% in VR. Each eye view ('oxrView0'/'oxrView1')
# exposes upscaleRenderScale to Lua, so a small unpacked mod sets it whenever the
# VR views appear (entering VR recreates them) and every 2 s puts it back if the
# game changed it. Setting it recreates FSR, so it is only set when it differs.
# (M8.49 tried a VR far clip the same way: BeamNG ignores frusta set from Lua.)
function Update-VRMod {
    param([string]$UserFolder, $Scale)
    $mod = Join-Path $UserFolder 'mods\unpacked\beamng_vrs'
    $marker = Join-Path $mod 'beamng_vrs.txt'
    if ($null -eq $Scale) {
        # Only a folder this launcher wrote is removed.
        if (Test-Path -LiteralPath $marker -PathType Leaf) { Remove-Item -LiteralPath $mod -Recurse -Force }
        return
    }
    if (-not (Test-Path -LiteralPath $UserFolder -PathType Container)) {
        Write-Warning "BeamNG user folder not found at $UserFolder; [Renderer] RenderScale is not applied."
        return
    }
    if ((Test-Path -LiteralPath $mod) -and -not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        Write-Warning "$mod exists but was not written by BeamNG-VRS; [Renderer] RenderScale is not applied."
        return
    }
    $scaleText = ([double]$Scale).ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
    $utf8 = New-Object Text.UTF8Encoding($false)
    $files = @{
        'beamng_vrs.txt' = "Written by Launch-BeamNG-VRS at every start from BeamNG-VRS.ini. Remove RenderScale there to delete it.`r`n"
        'scripts\beamng_vrs\modScript.lua' = "load('beamngVrs')`r`nsetExtensionUnloadMode('beamngVrs', 'manual')`r`n"
        'lua\ge\extensions\beamngVrs.lua' = @"
-- Written by Launch-BeamNG-VRS from BeamNG-VRS.ini ([Renderer] RenderScale).
local M = {}
local scale = $scaleText
local present = {}
local sinceCheck = 0
local function apply(name, view, why)
  view.upscaleRenderScale = scale
  log('I', 'beamngVrs', 'BeamNG-VRS render scale ' .. scale .. ' on ' .. name .. ' (' .. why .. ')')
end
local function onUpdate(dtReal)
  local manager = RenderViewManagerInstance
  if not manager then return end
  sinceCheck = sinceCheck + (dtReal or 0)
  local check = sinceCheck >= 2
  if check then sinceCheck = 0 end
  for _, name in ipairs({'oxrView0', 'oxrView1'}) do
    local ok, view = pcall(function() return manager:getView(name) end)
    if ok and view then
      if not present[name] then
        present[name] = true
        apply(name, view, 'VR started')
      elseif check then
        local current = view.upscaleRenderScale
        if type(current) == 'number' and math.abs(current - scale) > 0.001 then
          apply(name, view, 'game had set ' .. string.format('%.2f', current))
        end
      end
    else present[name] = nil end
  end
end
local function onClientPostStartMission() present = {} end
M.onUpdate = onUpdate
M.onClientPostStartMission = onClientPostStartMission
return M
"@
    }
    foreach ($relative in $files.Keys) {
        $path = Join-Path $mod $relative
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
        [IO.File]::WriteAllText($path, $files[$relative], $utf8)
    }
    Write-Output "VR render scale $scaleText applied through $mod"
}
function Test-BeamNGIni {
    param([string]$Path)
    $encoding = New-Object Text.UTF8Encoding($false, $true)
    $text = $encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $whitespace = [char[]]" `t`r`n"
    $section = ''; $lineNumber = 0; $seen = @{}
    $sizes = @{'foveation.size1x1' = 0.25; 'foveation.size2x2' = 0.55}
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
        'lensmask.size' = @(0.5, 1.5, $false)
        'renderer.renderscale' = @(0.25, 1.0, $false)
        'renderer.vrdrawdistance' = @(100, 10000, $false)
    }
    foreach ($line in ($text -split "`n")) {
        ++$lineNumber
        $line = $line.Trim($whitespace)
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        try {
            if ($line.StartsWith('[')) {
                if (-not $line.EndsWith(']')) { throw 'invalid section header' }
                $section = $line.Substring(1, $line.Length - 2).Trim($whitespace).ToLowerInvariant()
                if (@('foveation', 'debug', 'scissor', 'lensmask', 'tracking', 'performance', 'logging', 'openxr', 'renderer') -cnotcontains $section) { throw 'unknown section' }
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
                if ($id -eq 'renderer.renderscale') { $script:RenderScale = $number }
            } elseif ($id -eq 'foveation.shape') {
                if (@('circle', 'square', 'ellipse') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
            } elseif ($id -eq 'logging.level') {
                if (@('off', 'errors', 'info', 'debug') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
            } elseif ($id -eq 'debug.drawoutline' -or $id -eq 'scissor.enabled' -or $id -eq 'foveation.enabled' -or $id -eq 'foveation.allpasses' -or $id -eq 'lensmask.enabled' -or $id -eq 'performance.turbomode') {
                if (@('0', '1', 'false', 'true') -cnotcontains $value.ToLowerInvariant()) { throw 'expected 0, 1, false or true' }
            } elseif ($id -eq 'renderer.api') {
                if (@('vulkan', 'd3d12') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
                $script:Renderer = $value.ToLowerInvariant()
            } elseif ($id -eq 'openxr.disablelayers') {
                $script:DisabledOpenXRLayers = @(ConvertTo-OpenXRLayerList -Value $value)
            } elseif ($id -eq 'renderer.farclip') {
                Write-Warning "INI line ${lineNumber} [Renderer]: FarClip was removed (BeamNG ignores it in VR); this line is ignored and can be deleted."
            } else { throw 'unknown section or setting' }
        } catch {
            $where = if ($section) { " [$section]" } else { '' }
            throw "INI line ${lineNumber}${where}: $line : $($_.Exception.Message)"
        }
    }
    if ($seen.Count -eq 0) { throw 'INI contains no settings' }
    if ($sizes['foveation.size1x1'] -gt $sizes['foveation.size2x2']) { throw 'Size1x1 must not exceed Size2x2' }
    Write-Output 'BeamNG-VRS.ini is valid.'
}
try {
    $layerDirectory = $PSScriptRoot
    $installDirectory = Split-Path -Parent $layerDirectory
    $iniPath = Join-Path $installDirectory 'BeamNG-VRS.ini'
    # An omitted DisableLayers turns off every implicit layer for BeamNG; an empty value keeps them all.
    $script:DisabledOpenXRLayers = @('*')
    $script:Renderer = 'd3d12'
    $script:RenderScale = $null
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
    # Direct3D 12 is BeamNG 0.40's VR default and the launcher's. The OpenXR layer
    # applies the features on d3d12; the Vulkan layer applies them on vk.
    $start.Arguments = if ($script:Renderer -eq 'vulkan') { '-gfx vk' } else { '-gfx d3d12' }
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
    $implicitLayers = @(Get-OpenXRImplicitLayers -RegistryKeys $OpenXRLayerRegistry)
    $layerReport = @()
    foreach ($layer in $implicitLayers) {
        $label = if ($layer.Name) { $layer.Name } else { '(unreadable manifest) ' + [IO.Path]::GetFileName($layer.Manifest) }
        $state = if (-not $layer.Enabled) { 'off in registry' }
                 elseif ($layer.EnableEnvironment -and -not $start.EnvironmentVariables[$layer.EnableEnvironment]) { 'inactive (needs ' + $layer.EnableEnvironment + ')' }
                 else { 'active' }
        $listed = $layer.Name -and ($script:DisabledOpenXRLayers -contains '*' -or $script:DisabledOpenXRLayers -contains $layer.Name)
        if ($listed -and $layer.Enabled) {
            if ($layer.DisableEnvironment) {
                $start.EnvironmentVariables[$layer.DisableEnvironment] = '1'
                $state = 'disabled for BeamNG by BeamNG-VRS.ini'
            } else { Write-Warning "OpenXR layer $($layer.Name) has no disable_environment and cannot be disabled for BeamNG only." }
        }
        $layerReport += "OpenXR implicit layer: $label [$state]"
    }
    foreach ($name in $script:DisabledOpenXRLayers) {
        if ($name -ne '*' -and -not ($implicitLayers | Where-Object { $_.Name -eq $name })) {
            Write-Warning "[OpenXR] DisableLayers names $name, but no implicit OpenXR layer has that name."
        }
    }
    if ($CheckOnly) {
        $layerReport | Write-Output
        if ($null -ne $script:RenderScale) { Write-Output 'VR render scale will be applied through the BeamNG user folder.' }
        Write-Output 'Release files and game executable found. No game started.'; return
    }
    Update-VRMod -UserFolder $UserFolder -Scale $(if ($script:Renderer -eq 'd3d12') { $script:RenderScale } else { $null })
    if ($Diagnostics) {
        $start.EnvironmentVariables['VK_LOADER_DEBUG'] = 'error,warn,layer'
        # The OpenXR loader then reports the layer chain and which layer failed xrCreateInstance.
        $start.EnvironmentVariables['XR_LOADER_DEBUG'] = 'info'
        $start.EnvironmentVariables['BEAMNG_VRS_LOG_LEVEL'] = '2'
        $start.EnvironmentVariables['BEAMNG_VRS_M4_DIAGNOSTICS'] = '1'
        $start.RedirectStandardError = $true
        $logPath = Join-Path ([IO.Path]::GetTempPath()) ('BeamNG_VRS_loader_' + [Guid]::NewGuid() + '.log')
        Write-Output "Vulkan loader diagnostics: $logPath"
        Write-Output 'Close BeamNG (and any error dialog), then share this log if launch fails.'
        $writer = New-Object IO.StreamWriter($logPath, $false, [Text.Encoding]::UTF8)
        try {
            if (-not $layerReport) { $layerReport = @('OpenXR implicit layers: none registered') }
            foreach ($line in $layerReport) { Write-Output $line; $writer.WriteLine($line) }
            $writer.Flush()
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
    $layerReport | Where-Object { $_ -like '*disabled for BeamNG*' } | Write-Output
    [void][System.Diagnostics.Process]::Start($start)
    if ($script:Renderer -eq 'd3d12') {
        Write-Output 'BeamNG launched in Direct3D 12 mode. Change BeamNG-VRS.ini and restart to apply settings.'
    } else { Write-Output 'BeamNG launched in Vulkan mode. Change BeamNG-VRS.ini and restart to apply settings.' }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
