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
# M8.51: the same mod adds a "VR draw distance" UI app and radial menu entry. They
# write <UserFolder>\settings\beamng_vrs_vr_draw_distance.txt ("distance=N", "ini=N"),
# which the OpenXR layer applies live and the next launch keeps unless the INI changed.
$script:VRDrawDistanceApp = @'
angular.module('beamng.apps')
.directive('beamngVrsDrawDistance', [function () {
  return {
    template:
      '<div class="bngApp" style="padding: 6px 10px; color: white; box-sizing: border-box; width: 100%; height: 100%">' +
        '<div style="font-weight: bold">VR draw distance: {{ value }} m</div>' +
        '<input type="range" min="100" max="10000" step="100" ng-model="slider" ng-change="dragged()" style="width: 100%">' +
        '<div style="display: flex">' +
          '<button ng-repeat="p in presets" ng-click="pick(p.m)" style="flex: 1; margin: 1px; padding: 2px 0">{{ p.label }}</button>' +
        '</div>' +
      '</div>',
    replace: true,
    restrict: 'EA',
    scope: true,
    link: function (scope) {
      var timer = null
      scope.value = 0
      scope.slider = 0
      scope.presets = [500, 1000, 2000, 4000, 6000].map(function (m) { return { label: String(m), m: m } })
      scope.presets.push({ label: 'Full', m: 10000 })
      function send(m) { bngApi.engineLua('extensions.beamngVrs.setVrDrawDistance(' + Math.round(m) + ')') }
      function cancel() { if (timer) { clearTimeout(timer); timer = null } }
      // The slider only sends once it has been still for 300 ms.
      scope.dragged = function () {
        scope.value = Number(scope.slider)
        cancel()
        timer = setTimeout(function () { timer = null; send(scope.value) }, 300)
      }
      scope.pick = function (m) { cancel(); scope.slider = m; scope.value = m; send(m) }
      scope.$on('BeamngVrsDrawDistance', function (event, m) {
        scope.$evalAsync(function () { if (!timer) { scope.value = m; scope.slider = m } })
      })
      scope.$on('$destroy', cancel)
      bngApi.engineLua('extensions.beamngVrs.requestVrDrawDistance()')
    }
  }
}])
'@
$script:VRDrawDistanceAppJson = @'
{
  "domElement": "<beamng-vrs-draw-distance></beamng-vrs-draw-distance>",
  "name": "VR draw distance",
  "types": [
    "ui.apps.categories.info"
  ],
  "description": "BeamNG-VRS: VR far distance in metres, applied live",
  "css": {
    "left": "200px",
    "top": "200px",
    "width": "320px",
    "height": "90px"
  },
  "author": "BeamNG-VRS",
  "version": "0.1",
  "directive": "beamngVrsDrawDistance",
  "category": "General",
  "isAuxiliary": false,
  "interactive": "yes"
}
'@
$script:VRDrawDistanceAppPng = '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'
# Returns this launch's starting distance: the in-game choice saved in the live
# file when it was made with the same INI value, else the INI value (and a stale
# file is deleted). Null when VRDrawDistance=off.
function Get-VRDrawDistanceStart {
    param([string]$File, $IniDistance)
    if ($null -eq $IniDistance) { return $null }
    if (Test-Path -LiteralPath $File -PathType Leaf) {
        $saved = @{}
        foreach ($line in [IO.File]::ReadAllLines($File)) {
            if ($line -match '^\s*(distance|ini)\s*=\s*(\S+)\s*$') { $saved[$Matches[1]] = $Matches[2] }
        }
        try {
            $ini = Assert-IniNumber -Value $saved['ini'] -Low 100 -High 10000 -Integer $false
            $distance = Assert-IniNumber -Value $saved['distance'] -Low 100 -High 10000 -Integer $false
            if ($ini -eq [double]$IniDistance) { return $distance }
        } catch {}
        Remove-Item -LiteralPath $File -Force
    }
    return [double]$IniDistance
}
function Update-VRMod {
    param([string]$UserFolder, $Scale, $Distance, $IniDistance)
    $mod = Join-Path $UserFolder 'mods\unpacked\beamng_vrs'
    $marker = Join-Path $mod 'beamng_vrs.txt'
    if ($null -eq $Scale -and $null -eq $Distance) {
        # Only a folder this launcher wrote is removed.
        if (Test-Path -LiteralPath $marker -PathType Leaf) { Remove-Item -LiteralPath $mod -Recurse -Force }
        return
    }
    if (-not (Test-Path -LiteralPath $UserFolder -PathType Container)) {
        Write-Warning "BeamNG user folder not found at $UserFolder; [Renderer] RenderScale and the VR draw distance app are not applied."
        return
    }
    if ((Test-Path -LiteralPath $mod) -and -not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        Write-Warning "$mod exists but was not written by BeamNG-VRS; [Renderer] RenderScale and the VR draw distance app are not applied."
        return
    }
    # Rewritten from scratch, so a feature turned off in the INI leaves no file behind.
    if (Test-Path -LiteralPath $mod) { Remove-Item -LiteralPath $mod -Recurse -Force }
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    $utf8 = New-Object Text.UTF8Encoding($false)
    $lua = "-- Written by Launch-BeamNG-VRS from BeamNG-VRS.ini ([Renderer] RenderScale and VRDrawDistance).`r`nlocal M = {}`r`n"
    if ($null -ne $Scale) {
        $scaleText = ([double]$Scale).ToString('0.###', $invariant)
        $lua += @"
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

"@
    }
    if ($null -ne $Distance) {
        $iniText = ([double]$IniDistance).ToString('0', $invariant)
        $distanceText = ([double]$Distance).ToString('0', $invariant)
        $lua += @"
-- M8.51: VR draw distance. The OpenXR layer applies the file below live.
local iniDistance = $iniText
local distance = $distanceText
local distanceFile = '/settings/beamng_vrs_vr_draw_distance.txt'
local presets = {500, 1000, 2000, 4000, 6000, 10000}
local function setVrDrawDistance(m)
  m = tonumber(m)
  if not m then return end
  m = math.floor(math.min(10000, math.max(100, m)) / 100 + 0.5) * 100
  distance = m
  writeFile(distanceFile, 'distance=' .. m .. '\nini=' .. iniDistance .. '\n')
  log('I', 'beamngVrs', 'BeamNG-VRS VR draw distance ' .. m .. ' m')
  guihooks.trigger('BeamngVrsDrawDistance', m)
end
local function requestVrDrawDistance() guihooks.trigger('BeamngVrsDrawDistance', distance) end
-- Radial (quick access) menu: Freeroam actions > VR draw distance.
local function registerMenus()
  if not core_quickAccess then return end
  core_quickAccess.addEntry({ level = '/root/sandbox/', uniqueID = 'beamngVrsDrawDistance', title = 'VR draw distance',
    icon = 'road', categoryOrder = 40, ['goto'] = '/root/sandbox/beamngVrsDrawDistance/' })
  core_quickAccess.addEntry({ level = '/root/sandbox/beamngVrsDrawDistance/', uniqueID = 'beamngVrsDrawDistancePresets',
    generator = function(entries)
      for i, m in ipairs(presets) do
        local item = { title = m == 10000 and 'Full (10000 m)' or (m .. ' m'), icon = 'road', priority = i,
          uniqueID = 'beamngVrsDrawDistance' .. m, onSelect = function() setVrDrawDistance(m) return {'reload'} end }
        if m == distance then item.color = '#ff6600' end
        table.insert(entries, item)
      end
    end })
end
M.setVrDrawDistance = setVrDrawDistance
M.requestVrDrawDistance = requestVrDrawDistance
M.onRegisterQuickAccessMenus = registerMenus
M.onExtensionLoaded = registerMenus

"@
    }
    $lua += "return M`r`n"
    $files = @{
        'beamng_vrs.txt' = "Written by Launch-BeamNG-VRS at every start from BeamNG-VRS.ini. Remove RenderScale and set VRDrawDistance=off there to delete it.`r`n"
        'scripts\beamng_vrs\modScript.lua' = "load('beamngVrs')`r`nsetExtensionUnloadMode('beamngVrs', 'manual')`r`n"
        'lua\ge\extensions\beamngVrs.lua' = $lua
    }
    if ($null -ne $Distance) {
        $files['ui\modules\apps\BeamngVrsDrawDistance\app.js'] = $script:VRDrawDistanceApp
        $files['ui\modules\apps\BeamngVrsDrawDistance\app.json'] = $script:VRDrawDistanceAppJson
    }
    foreach ($relative in $files.Keys) {
        $path = Join-Path $mod $relative
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
        [IO.File]::WriteAllText($path, $files[$relative], $utf8)
    }
    if ($null -ne $Distance) {
        [IO.File]::WriteAllBytes((Join-Path $mod 'ui\modules\apps\BeamngVrsDrawDistance\app.png'),
            [Convert]::FromBase64String($script:VRDrawDistanceAppPng))
        Write-Output "VR draw distance $distanceText m; its in-game app and radial menu are added through $mod"
    }
    if ($null -ne $Scale) { Write-Output "VR render scale $scaleText applied through $mod" }
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
            } elseif ($id -eq 'renderer.vrdrawdistance') {
                # off leaves the game's code untouched (an escape hatch after a game update).
                $script:VRDrawDistance = if ($value -ieq 'off') { $null } else { Assert-IniNumber -Value $value -Low 100 -High 10000 -Integer $false }
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
    $script:VRDrawDistance = [double]4000
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
    # M8.51: the in-game VR draw distance persists unless the INI value changed.
    $distanceFile = Join-Path $UserFolder 'settings\beamng_vrs_vr_draw_distance.txt'
    $startDistance = Get-VRDrawDistanceStart -File $distanceFile -IniDistance $script:VRDrawDistance
    if ($null -ne $startDistance) {
        if ($startDistance -ne $script:VRDrawDistance) {
            # Overrides the INI value in the OpenXR layer (child environment only).
            $start.EnvironmentVariables['BEAMNG_VRS_VR_DRAW_DISTANCE'] = $startDistance.ToString('0', [Globalization.CultureInfo]::InvariantCulture)
        }
        $start.EnvironmentVariables['BEAMNG_VRS_VR_DRAW_DISTANCE_FILE'] = $distanceFile
    }
    Update-VRMod -UserFolder $UserFolder -Scale $(if ($script:Renderer -eq 'd3d12') { $script:RenderScale } else { $null }) `
        -Distance $startDistance -IniDistance $script:VRDrawDistance
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
