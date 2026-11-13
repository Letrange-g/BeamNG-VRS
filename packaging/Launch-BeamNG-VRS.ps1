[CmdletBinding()]
param(
    [string]$GameExecutable,
    [switch]$CheckOnly,
    [switch]$Diagnostics,
    [switch]$ValidateOnly,
    # Start BeamNG through BeamMP-Launcher.exe (multiplayer) instead of directly.
    [switch]$BeamMP,
    # BeamMP-Launcher.exe when it is not in its default folder, %APPDATA%\BeamMP-Launcher.
    [string]$BeamMPLauncher,
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
# exposes upscaleRenderScale to Lua, so a small in-game script (loose user-folder files) sets it whenever the
# VR views appear (entering VR recreates them) and every 2 s puts it back if the
# game changed it. Setting it recreates FSR, so it is only set when it differs.
# (M8.49 tried a VR far clip the same way: BeamNG ignores frusta set from Lua.)
# M8.51: the same mod adds a "VR draw distance" UI app and radial menu entry. They
# write <UserFolder>\settings\beamng_vrs_vr_draw_distance.txt ("distance=N", "ini=N"),
# which the OpenXR layer applies live and the next launch keeps unless the INI changed.
# UI app "VR draw distance" (Angular directive, the format of the game's own apps):
# a slider plus preset buttons. It calls the Lua extension below through
# bngApi.engineLua and follows the 'BeamngVrsDrawDistance' event, so it shows the
# same value as the radial menu.
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
      // A change from Lua (radial menu or another app) moves the slider, unless
      // the user is still dragging it.
      scope.$on('BeamngVrsDrawDistance', function (event, m) {
        scope.$evalAsync(function () { if (!timer) { scope.value = m; scope.slider = m } })
      })
      scope.$on('$destroy', cancel)
      // Ask Lua for the current value when the app opens.
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
# The in-game script is written as loose files in the BeamNG user folder, not as a mod:
# BeamMP turns off every mod a server does not list (ours included, which left the quad
# views' surround black), but it only manages mods. BeamNG runs every
# scripts\*\modScript.lua it finds, loose files included, as it does for mods.
# settings\beamng_vrs_files.txt lists the files written, so only those are removed.
$script:VRScriptFiles = @('lua\ge\extensions\beamngVrs.lua', 'scripts\beamng_vrs\modScript.lua',
    'ui\modules\apps\BeamngVrsDrawDistance\app.js', 'ui\modules\apps\BeamngVrsDrawDistance\app.json',
    'ui\modules\apps\BeamngVrsDrawDistance\app.png')
function Remove-VRScript {
    param([string]$UserFolder)
    # Up to 0.4.2 the script was the mod mods\unpacked\beamng_vrs; only a folder this
    # launcher wrote (it has beamng_vrs.txt) is removed.
    $oldMod = Join-Path $UserFolder 'mods\unpacked\beamng_vrs'
    if (Test-Path -LiteralPath (Join-Path $oldMod 'beamng_vrs.txt') -PathType Leaf) { Remove-Item -LiteralPath $oldMod -Recurse -Force }
    $list = Join-Path $UserFolder 'settings\beamng_vrs_files.txt'
    if (-not (Test-Path -LiteralPath $list -PathType Leaf)) { return }
    foreach ($relative in [IO.File]::ReadAllLines($list)) {
        # Only names this launcher writes, so a damaged list cannot delete anything else.
        if ($script:VRScriptFiles -notcontains $relative.Trim()) { continue }
        $path = Join-Path $UserFolder $relative.Trim()
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
        # Remove the folders it leaves empty, up to (not including) the user folder.
        $folder = Split-Path -Parent $path
        while ($folder.Length -gt $UserFolder.TrimEnd('\').Length -and (Test-Path -LiteralPath $folder -PathType Container) -and
            -not (Get-ChildItem -LiteralPath $folder -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $folder -Force
            $folder = Split-Path -Parent $folder
        }
    }
    Remove-Item -LiteralPath $list -Force
}
function Update-VRMod {
    param([string]$UserFolder, $Scale, $Distance, $IniDistance, $Middle)
    if (Test-Path -LiteralPath $UserFolder -PathType Container) { Remove-VRScript -UserFolder $UserFolder }
    if ($null -eq $Scale -and $null -eq $Distance -and $null -eq $Middle) { return }
    if (-not (Test-Path -LiteralPath $UserFolder -PathType Container)) {
        Write-Warning "BeamNG user folder not found at $UserFolder; [Renderer] RenderScale, the VR draw distance app and the quad views' wide views are not applied."
        return
    }
    foreach ($relative in $script:VRScriptFiles) {
        $path = Join-Path $UserFolder $relative
        if (Test-Path -LiteralPath $path) {
            Write-Warning "$path exists but was not written by BeamNG-VRS; [Renderer] RenderScale, the VR draw distance app and the quad views' wide views are not applied."
            return
        }
    }
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    $utf8 = New-Object Text.UTF8Encoding($false)
    $lua = "-- Written by Launch-BeamNG-VRS from BeamNG-VRS.ini ([Renderer] RenderScale and VRDrawDistance, [VR] Mode).`r`nlocal M = {}`r`n"
    # Extension hooks are collected per name, since several features may use the same one.
    $hooks = @{}
    # The driver's head: BeamNG hides it in player 0's first-person render views by a
    # per-view bit (gameplay/seatOccupants.lua, setMeshHiddenInRenderView), but the VR
    # eye views do not carry that bit, so the head and helmet showed from the inside.
    $lua += @"
local headSince = 2
local function headUpdate(dtReal)
  headSince = headSince + (dtReal or 0)
  if headSince < 2 then return end
  headSince = 0
  local manager = RenderViewManagerInstance
  if not manager then return end
  for _, name in ipairs({'oxrView0', 'oxrView1'}) do
    pcall(function()
      local view = manager:getView(name)
      if view then view:maskSet(0) end
    end)
  end
end

"@
    $hooks['onUpdate'] = @('headUpdate')
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

"@
        $hooks['onUpdate'] = @($hooks['onUpdate']) + 'onUpdate'
        $hooks['onClientPostStartMission'] = @('onClientPostStartMission')
    }
    if ($null -ne $Middle) {
        # M8.55 quad views: one wide, low-resolution view per eye. The OpenXR layer finds
        # their render targets by size (the right one is two rows taller) and composes each
        # around its eye's sharp focus view; without them the surround stays black.
        # nil = auto: wait for the OpenXR layer's values in the settings folder. The size is
        # fixed only when WideWidth and WideHeight are both set; otherwise the layer sizes
        # the views from WideResolution and the headset.
        $fixedSize = $null -ne $Middle.Width -and $null -ne $Middle.Height
        $middleW = if ($fixedSize) { [string][int]$Middle.Width } else { 'nil' }
        $middleH = if ($fixedSize) { [string][int]$Middle.Height } else { 'nil' }
        $middleFov = if ($null -eq $Middle.Fov) { 'nil' } else { ([double]$Middle.Fov).ToString('0.##', $invariant) }
        $wideDetail = if ($Middle.Detail) { [string]$Middle.Detail } else { 'medium' }
        $lua += @"
-- M8.55 quad views: wide views.
local wideW, wideH, wideFov = $middleW, $middleH, $middleFov
local wideDetail = '$wideDetail'
-- core_camera first: its onPreRender sets this frame's head pose that the wide views use.
M.dependencies = {'core_camera'}
local wideCount = 2
local wideFovFile = '/settings/beamng_vrs_middle_fov.txt'
local wide = {}       -- [1] left eye, [2] right eye
local wideWait = 0
local wideFollowFailed = false
-- Projection of the wide views: horizontal FOV and near/far planes. far follows the left
-- eye's far plane every frame, so a live VR draw distance change (the in-game app or the
-- radial menu, which the OpenXR layer applies to the eyes) reaches the wide views too.
local wideFovUsed, wideNear, wideFar = nil, 0.1, 4000
-- Wide view pose prediction state (see wideFollow).
local heads = {}        -- head poses: [1] this frame, [2] one frame ago, [3] two ago
-- Learned eye offsets and the wide-vs-eye angle check (see wideFollow); declared here so
-- the diagnostics above wideFollow see them.
local lagUsed, lagCounts = 1, {0, 0, 0}
local lastWide, mismatchSum, mismatchMax, mismatchCount = nil, 0, 0, 0
local lag, lagSamples, lagOffset = 1, 0, {0, 0, 0}
local predict = core_camera ~= nil -- off if the check in wideFollow fails: plain copies then
local predictChecked = false
-- The OpenXR layer writes "fov=N width=W height=H" once it knows the headset: the wide
-- views' field of view (WideFov=auto) and size (WideResolution x the headset's eye size).
local function wideAuto()
  local ok, text = pcall(readFile, wideFovFile)
  if not ok or type(text) ~= 'string' then return {} end
  local info = {fov = tonumber(text:match('fov=([%d%.]+)')), width = tonumber(text:match('width=(%d+)')),
    height = tonumber(text:match('height=(%d+)'))}
  if not (info.fov and info.fov >= 30 and info.fov <= 150) then info.fov = nil end
  if not (info.width and info.width >= 256 and info.width <= 4096) then info.width = nil end
  if not (info.height and info.height >= 256 and info.height <= 4096) then info.height = nil end
  return info
end
local function wideSay(text) log('I', 'beamngVrs', 'BeamNG-VRS quad views: ' .. text) end
local function try(fn) local ok, r = pcall(fn) if ok then return r end return nil end
-- Diagnostics (beamng.log): camera position, axes and frustum of each eye and wide view,
-- every 3 s for the first 30 s and every 30 s after.
local diagTime, diagNext = 0, 0
local function fmt3(v) if not v then return '?' end return string.format('(%.3f,%.3f,%.3f)', v.x or 0, v.y or 0, v.z or 0) end
local function describe(name, view)
  if not view then return name .. '=nil' end
  local m = try(function() return view.cameraMatrix end)
  local text = name .. ' pos=' .. fmt3(m and try(function() return m:getPosition() end))
  for i = 0, 2 do text = text .. ' c' .. i .. '=' .. fmt3(m and try(function() return m:getColumn(i) end)) end
  text = text .. ' fov=' .. tostring(try(function() return view.fov end))
  local f = try(function() return view.frustum end)
  if f then
    local parts = {}
    for _, k in ipairs({'getNearLeft', 'getNearRight', 'getNearTop', 'getNearBottom', 'getNearDist', 'getFarDist'}) do
      local v = try(function() return f[k](f) end)
      if v ~= nil then parts[#parts + 1] = k:sub(4) .. '=' .. string.format('%.4f', v) end
    end
    text = text .. ' frustum ' .. table.concat(parts, ' ')
  end
  text = text .. ' cameraObject=' .. tostring(try(function() return view:getCameraObject() end))
  return text
end
local function wideDiag(manager, dt)
  diagTime = diagTime + (dt or 0)
  if diagTime < diagNext then return end
  diagNext = diagTime + (diagTime < 30 and 3 or 30)
  wideSay(string.format('diag t=%.0f wide vs eye angle deg mean %.2f max %.2f over %d frames | pose lag=%d predict=%s eye offset mm at lag 0/1/2=%.1f/%.1f/%.1f samples above 8 m/s=%d frames at lag 0/1/2=%d/%d/%d | %s | %s | %s | %s',
    diagTime, mismatchCount > 0 and mismatchSum / mismatchCount or 0, mismatchMax, mismatchCount,
    lag, tostring(predict), lagOffset[1] * 1000, lagOffset[2] * 1000, lagOffset[3] * 1000, lagSamples, lagCounts[1], lagCounts[2], lagCounts[3],
    describe('eye0', try(function() return manager:getView('oxrView0') end)),
    describe('eye1', try(function() return manager:getView('oxrView1') end)),
    describe('wide0', wide[1]), describe('wide1', wide[2])))
  mismatchSum, mismatchMax, mismatchCount = 0, 0, 0
  lagCounts = {0, 0, 0}
end
-- Wide view poses. The engine writes the eyes' matrices (oxrView0/1) while it renders,
-- after Lua's onPreRender, so here they still hold the previous frame: copied as they
-- are, the wide views trailed the eyes by a frame (about 30 cm at 100 km/h). This
-- frame's head pose is known though: core_camera's final pose already holds the
-- headset's predicted pose (core/cameraModes/gameengine.lua), and core_camera runs its
-- onPreRender before ours (it is a dependency). Each wide view takes that pose plus its
-- eye's offset from the head, measured against the head pose the eye was built from.
-- How many frames separate the two is measured while moving (the offset stays still
-- for the right count), 1 until then.
local function v3(v) return {v.x, v.y, v.z} end
local function dot(a, b) return a[1] * b[1] + a[2] * b[2] + a[3] * b[3] end
local function toLocal(h, v) return {dot(v, h.r), dot(v, h.f), dot(v, h.u)} end
local function toWorld(h, l)
  return {h.r[1] * l[1] + h.f[1] * l[2] + h.u[1] * l[3],
          h.r[2] * l[1] + h.f[2] * l[2] + h.u[2] * l[3],
          h.r[3] * l[1] + h.f[3] * l[2] + h.u[3] * l[3]}
end
local function headNow()
  local ok, h = pcall(function()
    return {p = v3(core_camera.getPosition()), r = v3(core_camera.getRight()),
            f = v3(core_camera.getForward()), u = v3(core_camera.getUp())}
  end)
  if ok then return h end
end
-- An eye matrix in head coordinates: position and the three axes.
local function eyeLocal(h, m)
  local p = m:getPosition()
  local e = {pos = toLocal(h, {p.x - h.p[1], p.y - h.p[2], p.z - h.p[3]})}
  for i = 0, 2 do e[i] = toLocal(h, v3(m:getColumn(i))) end
  return e
end
local function poseMatrix(h, e)
  local m = MatrixF(true)
  for i = 0, 2 do local w = toWorld(h, e[i]) m:setColumn(i, vec3(w[1], w[2], w[3])) end
  local p = toWorld(h, e.pos)
  m:setColumn(3, vec3(h.p[1] + p[1], h.p[2] + p[2], h.p[3] + p[3]))
  return m
end
local function matrixGap(a, b)
  local gap = 0
  for i = 0, 3 do
    local x, y = a:getColumn(i), b:getColumn(i)
    gap = math.max(gap, math.abs(x.x - y.x), math.abs(x.y - y.y), math.abs(x.z - y.z))
  end
  return gap
end
-- Lag: for 0, 1 and 2 frames, how far the left eye sits from the head. With the right
-- count it is the eye's own offset (a few cm); with a wrong one the distance the car
-- travelled in a frame adds to it, so this is measured above 8 m/s (9 cm a frame).
local function measureLag(m, dt)
  if not heads[3] or not dt or dt <= 0 then return end
  local a, b = heads[1].p, heads[2].p
  local speed = math.sqrt((a[1] - b[1]) ^ 2 + (a[2] - b[2]) ^ 2 + (a[3] - b[3]) ^ 2) / dt
  if speed < 8 then return end
  for k = 1, 3 do
    local pos = eyeLocal(heads[k], m).pos
    local d = math.sqrt(pos[1] ^ 2 + pos[2] ^ 2 + pos[3] ^ 2)
    lagOffset[k] = lagSamples == 0 and d or lagOffset[k] * 0.95 + d * 0.05
  end
  lagSamples = lagSamples + 1
  if lagSamples >= 30 then
    local best = 1
    for k = 1, 3 do if lagOffset[k] < lagOffset[best + 1] then best = k - 1 end end
    if best ~= lag then wideSay('pose lag now ' .. best .. ' frame(s)') end
    lag = best
  end
end
-- Exposure: each render view adapts its own auto exposure, and a wide view sees more sky
-- and interior than its eye's focus window, so it settled brighter or darker than the
-- eye and the focus window stood out. Each frame the wide view takes its eye's exposure
-- (manual EV or the current auto value), as render/renderViews.lua does for captures.
local exposureFailed = false
local function wideExposure(view, eye)
  if exposureFailed or not eye then return end
  local ok, err = pcall(function()
    view.useManualEV = eye.useManualEV
    view.manualEV = eye.manualEV
    local current = eye.currentExposure
    if type(current) == 'number' and current > 0 then view.currentExposure = current end
  end)
  if not ok then exposureFailed = true wideSay('exposure not copied: ' .. tostring(err)) end
end
-- How far each wide view's direction was from its eye's, measured a frame later when the
-- eye's matrix becomes readable (beamng.log diag: the angle the two views disagree by).
local function columnAngle(a, b)
  local worst = 0
  for c = 0, 2 do
    local x, y = a:getColumn(c), b:getColumn(c)
    local d = math.max(-1, math.min(1, x.x * y.x + x.y * y.y + x.z * y.z))
    worst = math.max(worst, math.deg(math.acos(d)))
  end
  return worst
end
local function wideFollow(manager, dt)
  local m0 = manager:getView('oxrView0').cameraMatrix
  if lastWide and dt then
    local ok, a = pcall(columnAngle, m0, lastWide)
    if ok and a == a then mismatchSum = mismatchSum + a mismatchMax = math.max(mismatchMax, a) mismatchCount = mismatchCount + 1 end
  end
  local m1 = manager:getView('oxrView1').cameraMatrix
  local source = {m0, m1}
  local h = predict and headNow()
  if h then
    table.insert(heads, 1, h)
    heads[4] = nil
    measureLag(manager:getView('oxrView0').cameraMatrix, dt)
    local then_ = heads[lag + 1] or h
    if not predictChecked then
      -- The round trip through head coordinates must give the eye matrix back.
      predictChecked = true
      local ok, gap = pcall(function() return matrixGap(poseMatrix(h, eyeLocal(h, m0)), m0) end)
      if not ok or gap > 0.001 then
        predict = false
        wideSay('pose prediction off (check failed: ' .. tostring(gap) .. '), wide views copy the eyes')
      else wideSay('pose prediction on') end
    end
    if predict then
      -- Each frame, the head pose the eye matrix was built from: of this frame's and the
      -- two before, the one the eye sits closest to and faces the same way as. A wrong
      -- one is off by a frame of car travel or head turn; the lag jumps when frames come
      -- unevenly, so a fixed count put the wide views a frame off now and then.
      local best, bestScore = then_, math.huge
      for k = 1, 3 do
        if heads[k] then
          local e = eyeLocal(heads[k], m0)
          local turn = math.acos(math.max(-1, math.min(1, e[1][2])))
          local score = math.sqrt(dot(e.pos, e.pos)) + 0.5 * turn
          if score < bestScore then best, bestScore = heads[k], score lagUsed = k - 1 end
        end
      end
      lagCounts[lagUsed + 1] = lagCounts[lagUsed + 1] + 1
      -- The eye's own offset from that head, measured anew every frame: BeamNG moves the
      -- eyes by less than the camera when the head moves sideways, so a fixed, learned
      -- offset made the wide views move more than the eyes (Matt, build #142).
      for i = 1, #source do source[i] = poseMatrix(h, eyeLocal(best, source[i])) end
    end
  end
  lastWide = source[1]
  for i = 1, wideCount do
    local view = wide[i]
    if view then
      local ok = pcall(function() view.cameraMatrix = source[i] end)
      if not ok then wide[i] = nil else wideExposure(view, manager:getView('oxrView' .. (i - 1))) end
    end
  end
end
-- WideDetail: the wide views draw the game's own settings scaled down (medium, low) or as
-- they are (full), the way the game's split screen does it (render/splitScreen.lua). Each
-- view repeats the game's work, so less model detail, grass, small meshes, shadow map
-- resolution and no live reflections save time; the blurry periphery hides the difference.
-- Mirrors are off there too (the focus view still shows them when you look).
local wideDetailScale = {
  full = nil,
  medium = {mesh = 0.5, grass = 0.5, cull = 2, shadow = 0.5, mirrors = false},
  low = {mesh = 0.25, grass = 0, cull = 4, shadow = 0.25, mirrors = false},
}
local function wideQuality(view, name)
  local scale = wideDetailScale[wideDetail]
  if not scale then return end
  local skipped = {}
  local ok, err = pcall(function()
    local s = RenderSettings.getGlobals()
    -- Each field on its own, so one a game version lacks does not stop the others.
    local function set(field, fn)
      if not pcall(function() local v = s[field] if v == nil then error('missing') end s[field] = fn(v) end) then
        skipped[#skipped + 1] = field
      end
    end
    set('meshDetailAdjust', function(v) return v * scale.mesh end)
    set('groundCoverScale', function(v) return v * scale.grass end)
    -- Meshes smaller than this many pixels are skipped. The wide views have about a
    -- quarter of the eyes' pixels per degree, so the game's value already culls more
    -- there; this culls more again.
    set('meshSmallestPixelSize', function(v) if v > 0 then return v * scale.cull end return v end)
    set('shadowTexScalar', function(v) return v * scale.shadow end)
    set('vehicleDynamicReflectionsEnabled', function() return false end)
    if not scale.mirrors then set('vehicleMirrorsEnabled', function() return false end) end
    view.settingsOverride = s -- returned by value: the whole struct is written back
    view.settingsOverrideEnabled = true
  end)
  if ok then wideSay(name .. ' detail ' .. wideDetail .. (#skipped > 0 and (' (not available: ' .. table.concat(skipped, ', ') .. ')') or ''))
  else wideSay(name .. ' detail not set: ' .. tostring(err)) end
end
-- The projection comes from the frustum alone (fov is only a label, as in the game's own
-- render/viewDemo.lua). Frustum.construct takes the VERTICAL angle and widens it by the
-- aspect (measured: near-plane extents 0.43 x 0.295 at 0.2 m), while the FOV here is
-- horizontal, so it is converted. i: 1 left eye, 2 right eye (two rows taller).
local function wideFrustum(view, i, near, far)
  local h = wideH + (i - 1) * 2
  local vertical = 2 * math.atan(math.tan(math.rad(wideFovUsed) / 2) * h / wideW)
  view.frustum = Frustum.construct(false, vertical, wideW / h, near, far)
end
-- Keeps the wide views' far plane equal to the eyes' (the VR draw distance, changeable live).
local function wideSyncFar(manager)
  local ok, x = pcall(function() return manager:getView('oxrView0').frustum:getFarDist() end)
  if not ok or type(x) ~= 'number' or x <= wideNear or math.abs(x - wideFar) < 0.5 then return end
  wideFar = x
  for i = 1, wideCount do
    if wide[i] then
      local set, err = pcall(wideFrustum, wide[i], i, wideNear, wideFar)
      if not set then wideSay('far plane not updated: ' .. tostring(err)) return end
    end
  end
  wideSay('far ' .. math.floor(wideFar + 0.5) .. ' m, as the eyes (VR draw distance)')
end
local function wideCreate(manager)
  local info = wideAuto()
  local fov = wideFov or info.fov
  wideW, wideH = wideW or info.width, wideH or info.height
  if not (fov and wideW and wideH) then return end -- auto: not written yet, retried in a second
  -- Near and far follow the left eye when its frustum can be read (0.1 m and 4000 m in
  -- BeamNG 0.40). The driver's head is hidden by the render view mask set below, as in
  -- the eyes, so nothing needs clipping: a farther near plane cut the roof liner away.
  local near, far = 0.1, 4000
  pcall(function()
    local f = manager:getView('oxrView0').frustum
    local n, x = f:getNearDist(), f:getFarDist()
    if type(n) == 'number' and n > 0 and n < 1 then near = n end
    if type(x) == 'number' and x > near then far = x end
  end)
  wideFovUsed, wideNear, wideFar = fov, near, far
  for i = 1, wideCount do
    if not wide[i] then
      local name = 'beamngVrsWide' .. (i - 1)
      local h = wideH + (i - 1) * 2
      local view
      local ok, err = pcall(function()
        view = manager:getOrCreateView(name)
        view.luaOwned = true
        view.resolution = Point2I(wideW, h)
        view.viewPort = RectI(0, 0, wideW, h)
        view.namedTexTargetColor = name
        pcall(function() view.renderEditorIcons = false end)
        -- The driver's head is hidden per render view bit (gameplay/seatOccupants.lua:
        -- setMeshHiddenInRenderView(mesh, player, true)); the eyes carry player 0's bit.
        local masked = pcall(function() view:maskSet(0) end)
        if not masked then wideSay(name .. ': render view mask not set, the driver head may show') end
        wideQuality(view, name)
        wideFrustum(view, i, near, far)
        view.fov = fov
      end)
      if ok then
        wide[i] = view
        wideSay(name .. ' ' .. wideW .. 'x' .. h .. ', ' .. fov .. ' deg horizontal' .. (wideFov and '' or ' (auto)') ..
          ', near ' .. near .. ' m, far ' .. far .. ' m')
      else wideSay(name .. ' not created: ' .. tostring(err)) end
    end
  end
  pcall(wideFollow, manager, nil)
end
local function wideDestroy(manager)
  for i = 1, 2 do
    if wide[i] and manager then local view = wide[i] pcall(function() manager:destroyView(view) end) end
    wide[i] = nil
  end
end
local function wideUpdate(dtReal)
  local manager = RenderViewManagerInstance
  if not manager then return end
  local ok, eye = pcall(function() return manager:getView('oxrView0') end)
  if not ok or not eye then
    if wide[1] or wide[2] then wideDestroy(manager) wideSay('VR stopped, wide views removed') end
    wideWait = 0
    return
  end
  if not wide[1] or (wideCount == 2 and not wide[2]) then
    wideWait = wideWait + (dtReal or 0)
    if wideWait >= 1 then wideWait = 0 wideCreate(manager) end
    return
  end
  pcall(wideSyncFar, manager)
  wideDiag(manager, dtReal)
end
local function widePreRender(dtReal)
  local manager = RenderViewManagerInstance
  if manager and wide[1] then
    local ok, err = pcall(wideFollow, manager, dtReal)
    if not ok and not wideFollowFailed then wideFollowFailed = true wideSay('pose update failed: ' .. tostring(err)) end
  end
end
local function wideMission() wide = {} wideWait = 0 heads = {} lagCounts = {0, 0, 0} end
local function wideUnload() wideDestroy(RenderViewManagerInstance) end

"@
        foreach ($pair in @(@('onUpdate', 'wideUpdate'), @('onPreRender', 'widePreRender'),
                            @('onClientPostStartMission', 'wideMission'), @('onExtensionUnloaded', 'wideUnload'))) {
            $hooks[$pair[0]] = @($hooks[$pair[0]]) + $pair[1] | Where-Object { $_ }
        }
    }
    if ($null -ne $Distance) {
        $iniText = ([double]$IniDistance).ToString('0', $invariant)
        $distanceText = ([double]$Distance).ToString('0', $invariant)
        $lua += @"
-- M8.51: VR draw distance. The OpenXR layer applies the file below live.
-- iniDistance is saved beside the pick so the launcher can tell whether the INI
-- changed since; distance is the value this launch started with.
local iniDistance = $iniText
local distance = $distanceText
local distanceFile = '/settings/beamng_vrs_vr_draw_distance.txt'
local presets = {500, 1000, 2000, 4000, 6000, 10000}
-- Called by the UI app and the radial menu: clamps to 100-10000 m in 100 m
-- steps, saves it for the DLL and the next launch, and tells the UI.
local function setVrDrawDistance(m)
  m = tonumber(m)
  if not m then return end
  m = math.floor(math.min(10000, math.max(100, m)) / 100 + 0.5) * 100
  distance = m
  -- writeFile returns nil when the file cannot be opened, and pcall catches errors.
  local ok, written = pcall(writeFile, distanceFile, 'distance=' .. m .. '\nini=' .. iniDistance .. '\n')
  if not ok or not written then log('W', 'beamngVrs', 'BeamNG-VRS could not save the VR draw distance: ' .. tostring(written)) return end
  log('I', 'beamngVrs', 'BeamNG-VRS VR draw distance ' .. m .. ' m')
  guihooks.trigger('BeamngVrsDrawDistance', m)
end
-- Sends the current value to the UI app.
local function requestVrDrawDistance() guihooks.trigger('BeamngVrsDrawDistance', distance) end
-- Radial (quick access) menu: Freeroam actions > VR draw distance.
-- A future game version may change this menu: the entry is then left out, nothing else breaks.
local function addMenuEntry(entry)
  local ok, err = pcall(core_quickAccess.addEntry, entry)
  if not ok then log('W', 'beamngVrs', 'BeamNG-VRS radial menu entry not added: ' .. tostring(err)) end
end
-- One category on the Freeroam actions page ('/root/sandbox/') that opens a
-- submenu; its generator builds the preset items each time the menu opens, so
-- the current preset is highlighted. Returning {'reload'} redraws the menu
-- after a pick instead of closing it.
local function registerMenus()
  if not core_quickAccess or type(core_quickAccess.addEntry) ~= 'function' then return end
  addMenuEntry({ level = '/root/sandbox/', uniqueID = 'beamngVrsDrawDistance', title = 'VR draw distance',
    icon = 'road', categoryOrder = 40, ['goto'] = '/root/sandbox/beamngVrsDrawDistance/' })
  addMenuEntry({ level = '/root/sandbox/beamngVrsDrawDistance/', uniqueID = 'beamngVrsDrawDistancePresets',
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
-- The game asks extensions for entries whenever it rebuilds the radial menu;
-- registering on load too covers a menu that was built before this mod loaded.
M.onRegisterQuickAccessMenus = registerMenus
M.onExtensionLoaded = registerMenus

"@
    }
    foreach ($hook in $hooks.Keys) {
        $calls = ($hooks[$hook] | ForEach-Object { "$_(...)" }) -join ' '
        $lua += "M.$hook = function(...) $calls end`r`n"
    }
    $lua += "return M`r`n"
    $files = @{
        'scripts\beamng_vrs\modScript.lua' = "load('beamngVrs')`r`nsetExtensionUnloadMode('beamngVrs', 'manual')`r`n"
        'lua\ge\extensions\beamngVrs.lua' = $lua
    }
    if ($null -ne $Distance) {
        $files['ui\modules\apps\BeamngVrsDrawDistance\app.js'] = $script:VRDrawDistanceApp
        $files['ui\modules\apps\BeamngVrsDrawDistance\app.json'] = $script:VRDrawDistanceAppJson
    }
    $written = @($files.Keys)
    if ($null -ne $Distance) { $written += 'ui\modules\apps\BeamngVrsDrawDistance\app.png' }
    # The list first, so a launch that stops halfway still removes what it wrote.
    $settings = Join-Path $UserFolder 'settings'
    [void](New-Item -ItemType Directory -Path $settings -Force)
    [IO.File]::WriteAllText((Join-Path $settings 'beamng_vrs_files.txt'), (($written -join "`r`n") + "`r`n"), $utf8)
    foreach ($relative in $files.Keys) {
        $path = Join-Path $UserFolder $relative
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force)
        [IO.File]::WriteAllText($path, $files[$relative], $utf8)
    }
    $where = "the in-game script in $UserFolder (lua\ge\extensions\beamngVrs.lua)"
    if ($null -ne $Distance) {
        [IO.File]::WriteAllBytes((Join-Path $UserFolder 'ui\modules\apps\BeamngVrsDrawDistance\app.png'),
            [Convert]::FromBase64String($script:VRDrawDistanceAppPng))
        Write-Output "VR draw distance $distanceText m; its in-game app and radial menu are added through $where"
    }
    if ($null -ne $Scale) { Write-Output "VR render scale $scaleText applied through $where" }
    if ($null -ne $Middle) { Write-Output "Quad views: wide views $(if ($fixedSize) { "${middleW}x${middleH}" } else { 'sized from the headset' }) (FOV $(if ($null -eq $Middle.Fov) { 'auto' } else { $middleFov + ' deg' })) added through $where" }
}

# ---------------------------------------------------------------------------------------
# [Renderer] GhostingFix
#
# BeamNG 0.40 builds local exposure and bloom from one VR eye and reuses them for the
# other eye, which shows the first eye's picture as ghost shapes. GhostingFix.hlsl
# (beside this script) makes each eye read those textures lined up with its own view.
#
# BeamNG ignores shaders inside mods but loads loose files from the user folder, so the
# game's own combinePass shader is patched here and written there. The game caches
# compiled shaders by file name, so temp\shaders is cleared whenever that file changes.
# ---------------------------------------------------------------------------------------

# Returns the game's combinePass shader with GhostingFix.hlsl patched in, or $null (with a
# warning) when the game's shader no longer looks the way the patch expects.
function Get-GhostingFixShader {
    param([string]$StockFile, [string]$FixFile)

    $source = [IO.File]::ReadAllText($StockFile).Replace("`r`n", "`n")
    $fix = [IO.File]::ReadAllText($FixFile).Replace("`r`n", "`n")

    # GhostingFix.hlsl uses these stock declarations; if one was renamed, stand down.
    $declarations = @('Texture2D<float4> backbuffer', 'Texture2D<float4> bloomFinal',
                      'Texture2D<float4> localExposureBlurredLogLum', 'SamplerState samplerLinear', '#ifdef BNG_COCONUT')
    $missing = $declarations | Where-Object { -not $source.Contains($_) } | Select-Object -First 1
    if ($missing) {
        Write-Warning "[Renderer] GhostingFix: this BeamNG version's combinePass shader has no '$missing'; the ghosting fix is not applied."
        return $null
    }

    # Each edit is (stock text, replacement). "\n" marks a line break and
    # <GhostingFix.hlsl> the inserted file. The stock text is matched whitespace-tolerant
    # and must occur exactly once, so a game update that changes it stands the fix down.
    $edits = @(
        # 1. Insert the fix after getRtDimensions(), where every resource it uses is declared.
        @('uint2 getRtDimensions() {\n    uint2 dim;\n    destTex.GetDimensions(dim.x, dim.y);\n    return dim;\n}\n',
          'uint2 getRtDimensions() {\n    uint2 dim;\n    destTex.GetDimensions(dim.x, dim.y);\n    return dim;\n}\n\n<GhostingFix.hlsl>'),
        # 2. Read local exposure at this eye's measured shift.
        @('computeLocalExposure(sample.rgb, uv, exposureScale,',
          'computeLocalExposure(sample.rgb, saturate(uv + vrsLocalExposureShift), exposureScale,'),
        # 3. Read bloom at the same shift.
        @('sample.rgb += bloomScale * bloomFinal.SampleLevel(samplerLinear, uv, 0).rgb;',
          'sample.rgb += bloomScale * bloomFinal.SampleLevel(samplerLinear, saturate(uv + vrsBloomShift), 0).rgb;'),
        # 4. Optional debug bar (off unless VRS_STEREO_DEBUG is 1).
        @('destTex[storePos] = float4(sampleOutput.rgb, 1);',
          'destTex[storePos] = float4(vrsDebugOverlay(sampleOutput.rgb, uv), 1);'),
        # 5. Measure the shift in main(), before any thread of the group returns.
        @('void main(uint3 dispatchThreadID : SV_DispatchThreadID)\n{\n    if (any(dispatchThreadID.xy >= getRtDimensions())) return;',
          'void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)\n{\n    vrsEstimateEyeShift(groupIndex);\n    if (any(dispatchThreadID.xy >= getRtDimensions())) return;'))

    foreach ($edit in $edits) {
        $words = ($edit[0] -replace '\\n', "`n") -split '\s+' | Where-Object { $_ }
        $pattern = ($words | ForEach-Object { [regex]::Escape($_) }) -join '\s+'
        $found = [regex]::Matches($source, $pattern)
        if ($found.Count -ne 1) {
            $firstLine = $edit[0] -replace '\\n.*', ''
            Write-Warning "[Renderer] GhostingFix: this BeamNG version's combinePass shader changed ($firstLine...); the ghosting fix is not applied."
            return $null
        }

        $replacement = ($edit[1] -replace '\\n', "`n").Replace('<GhostingFix.hlsl>', $fix)
        $match = $found[0]
        $source = $source.Substring(0, $match.Index) + $replacement + $source.Substring($match.Index + $match.Length)
    }

    return "// Written by Launch-BeamNG-VRS ([Renderer] GhostingFix) from the game's own combinePass.cs.hlsl.`n" + $source
}

# Writes, refreshes or removes the patched shader in the user folder.
function Update-GhostingFix {
    param([string]$UserFolder, [string]$GameRoot, [string]$FixFile, [bool]$Enabled)

    $relative = 'shaders\common\postfx\hdr\combinePass.cs.hlsl'
    $target = Join-Path $UserFolder $relative
    $shaderCache = Join-Path $UserFolder 'temp\shaders'
    $marker = 'BeamNG-VRS ghosting fix' # appears in GhostingFix.hlsl's header

    # What is in the user folder now, and did this launcher write it?
    $existing = if (Test-Path -LiteralPath $target -PathType Leaf) { [IO.File]::ReadAllText($target) } else { $null }
    $ours = $null -ne $existing -and $existing.Contains($marker)

    # What should be there?
    $wanted = $null
    if ($Enabled) {
        $stock = Join-Path $GameRoot $relative
        if ((Test-Path -LiteralPath $stock -PathType Leaf) -and (Test-Path -LiteralPath $FixFile -PathType Leaf)) {
            $wanted = Get-GhostingFixShader -StockFile $stock -FixFile $FixFile
        } else {
            Write-Warning "[Renderer] GhostingFix: $stock or $FixFile not found; the ghosting fix is not applied."
        }
    }

    # Off, or not applicable: remove our file (never someone else's).
    if ($null -eq $wanted) {
        if ($ours) {
            Remove-Item -LiteralPath $target -Force
            if (Test-Path -LiteralPath $shaderCache) { Remove-Item -LiteralPath $shaderCache -Recurse -Force }
        }
        return
    }

    # Another mod's override wins; leave it alone.
    if ($null -ne $existing -and -not $ours) {
        Write-Warning "[Renderer] GhostingFix: $target exists but was not written by BeamNG-VRS (another mod?); the ghosting fix is not applied."
        return
    }

    # Unchanged: keep the file and the compiled shader cache.
    if ($ours -and $existing -eq $wanted) { return }

    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force)
    [IO.File]::WriteAllText($target, $wanted, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $shaderCache) { Remove-Item -LiteralPath $shaderCache -Recurse -Force }
    Write-Output 'Ghosting fix shader updated; BeamNG recompiles its shaders on this start (the first load is slower).'
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
        'quadviews.focuswidth' = @(0.1, 1, $false)
        'quadviews.focusheight' = @(0.1, 1, $false)
        'quadviews.feather' = @(0, 0.5, $false)
        'quadviews.gazedeadzone' = @(0, 10, $false)
        'quadviews.wideresolution' = @(0.1, 1, $false)
        'quadviews.widewidth' = @(256, 4096, $true)
        'quadviews.wideheight' = @(256, 4096, $true)
    }
    foreach ($line in ($text -split "`n")) {
        ++$lineNumber
        $line = $line.Trim($whitespace)
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        try {
            if ($line.StartsWith('[')) {
                if (-not $line.EndsWith(']')) { throw 'invalid section header' }
                $section = $line.Substring(1, $line.Length - 2).Trim($whitespace).ToLowerInvariant()
                if (@('foveation', 'debug', 'scissor', 'lensmask', 'tracking', 'performance', 'logging', 'openxr', 'renderer', 'vr', 'quadviews', 'tripleview') -cnotcontains $section) { throw 'unknown section' }
                continue
            }
            $equals = $line.IndexOf('=')
            if ($equals -lt 0) { throw 'expected key=value' }
            $key = $line.Substring(0, $equals).Trim($whitespace).ToLowerInvariant()
            $value = $line.Substring($equals + 1).Trim($whitespace)
            $id = $section + '.' + $key
            if ($seen.ContainsKey($id)) { throw 'duplicate setting' }
            $seen[$id] = $true
            # M8.55: the unreleased M8.54 [TripleView] names still work (same rule as the DLL).
            if ($section -eq 'tripleview') {
                $renamed = @{ sharpwidth = 'focuswidth'; sharpheight = 'focusheight'; middlewidth = 'widewidth'; middleheight = 'wideheight'; middlefov = 'widefov' }
                $id = 'quadviews.' + $(if ($renamed.ContainsKey($key)) { $renamed[$key] } else { $key })
            }
            if ($ranges.ContainsKey($id)) {
                $range = $ranges[$id]
                $number = Assert-IniNumber -Value $value -Low $range[0] -High $range[1] -Integer $range[2]
                if ($sizes.ContainsKey($id)) { $sizes[$id] = $number }
                if ($id -eq 'renderer.renderscale') { $script:RenderScale = $number }
                if ($id -eq 'quadviews.widewidth') { $script:Middle.Width = $number }
                if ($id -eq 'quadviews.wideheight') { $script:Middle.Height = $number }
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
            } elseif ($id -eq 'renderer.ghostingfix') {
                if (@('0', '1', 'false', 'true') -cnotcontains $value.ToLowerInvariant()) { throw 'expected 0, 1, false or true' }
                $script:GhostingFix = @('1', 'true') -ccontains $value.ToLowerInvariant()
            } elseif ($id -eq 'vr.mode') {
                # Same rule as the DLL: spaces, '-' and '_' are ignored ("quad views" works);
                # the M8.54 names triple and triple-black still work and mean quad and quad-black
                # (triple view, one wide view shared by both eyes, was removed).
                $mode = ($value.ToLowerInvariant() -replace '[\s_-]', '')
                if (@('vrs') -ccontains $mode) { $script:VRMode = 'vrs' }
                elseif (@('quad', 'quadview', 'quadviews', 'triple', 'tripleview') -ccontains $mode) { $script:VRMode = 'quad' }
                elseif (@('quadblack', 'quadviewblack', 'quadviewsblack', 'tripleblack', 'tripleviewblack') -ccontains $mode) { $script:VRMode = 'quad-black' }
                else { throw 'invalid VR mode (use vrs, quad or quad-black)' }
            } elseif ($id -eq 'quadviews.widefov') {
                # auto (default): the OpenXR layer sizes it from the headset at VR start.
                $script:Middle.Fov = if ($value -ieq 'auto') { $null } else { Assert-IniNumber -Value $value -Low 30 -High 150 -Integer $false }
            } elseif ($id -eq 'quadviews.followgaze') {
                if (@('0', '1', 'false', 'true') -cnotcontains $value.ToLowerInvariant()) { throw 'expected 0, 1, false or true' }
            } elseif ($id -eq 'quadviews.shape') {
                if (@('circle', 'rectangle') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
            } elseif ($id -eq 'quadviews.widedetail') {
                if (@('full', 'medium', 'low') -cnotcontains $value.ToLowerInvariant()) { throw 'unsupported setting value' }
                $script:Middle.Detail = $value.ToLowerInvariant()
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
    $script:GhostingFix = $false
    $script:VRMode = 'vrs'
    $script:Middle = @{ Width = $null; Height = $null; Fov = $null; Detail = 'medium' }
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
    $gameRoot = Split-Path -Parent (Split-Path -Parent $GameExecutable)
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $GameExecutable
    $start.WorkingDirectory = $gameRoot
    # Direct3D 12 is BeamNG 0.40's VR default and the launcher's. The OpenXR layer
    # applies the features on d3d12; the Vulkan layer applies them on vk.
    $start.Arguments = if ($script:Renderer -eq 'vulkan') { '-gfx vk' } else { '-gfx d3d12' }
    if ($BeamMP) {
        # BeamMP-Launcher starts BeamNG with its own environment, so the child settings
        # below reach the game. It passes the arguments after -- on to the game.
        if (-not $BeamMPLauncher) { $BeamMPLauncher = Join-Path $env:APPDATA 'BeamMP-Launcher\BeamMP-Launcher.exe' }
        if (-not (Test-Path -LiteralPath $BeamMPLauncher -PathType Leaf)) {
            throw "BeamMP-Launcher.exe not found at $BeamMPLauncher. Install BeamMP, or run this script with -BeamMP -BeamMPLauncher followed by the full path of BeamMP-Launcher.exe."
        }
        $start.FileName = (Resolve-Path -LiteralPath $BeamMPLauncher).Path
        # BeamMP renames and updates itself relative to its own folder.
        $start.WorkingDirectory = Split-Path -Parent $start.FileName
        $start.Arguments = '-- ' + $start.Arguments
    }
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
        if ($BeamMP) { Write-Output "BeamMP launcher found: $($start.FileName)" }
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
    # M8.54 MiddleFov=auto: the OpenXR layer writes the headset-sized FOV here at VR start;
    # an old value (another headset) is removed first so the mod waits for the new one.
    $middleFovFile = Join-Path $UserFolder 'settings\beamng_vrs_middle_fov.txt'
    if (Test-Path -LiteralPath $middleFovFile -PathType Leaf) { Remove-Item -LiteralPath $middleFovFile -Force }
    $start.EnvironmentVariables['BEAMNG_VRS_TRIPLE_FOV_FILE'] = $middleFovFile
    Update-VRMod -UserFolder $UserFolder -Scale $(if ($script:Renderer -eq 'd3d12') { $script:RenderScale } else { $null }) `
        -Distance $startDistance -IniDistance $script:VRDrawDistance `
        -Middle $(if ($script:Renderer -eq 'd3d12' -and $script:VRMode -eq 'quad') { $script:Middle } else { $null })
    if ($script:VRMode -ne 'vrs' -and $script:Renderer -ne 'd3d12') {
        Write-Warning "[VR] Mode=$($script:VRMode) needs [Renderer] API=d3d12; VRS is used instead."
    } elseif ($script:VRMode -ne 'vrs') {
        Write-Output "VR mode: $($script:VRMode) (experimental). [Scissor] cuts the composed eyes' edges; [Foveation] and [LensMask] are not used in this mode."
    }
    Update-GhostingFix -UserFolder $UserFolder -GameRoot $gameRoot `
        -FixFile (Join-Path $layerDirectory 'GhostingFix.hlsl') -Enabled $script:GhostingFix
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
    if ($BeamMP) {
        Write-Output 'BeamMP launcher started; it starts BeamNG with BeamNG-VRS. On servers the in-game draw distance app and radial menu are off; BeamNG-VRS.ini settings still apply.'
    } elseif ($script:Renderer -eq 'd3d12') {
        Write-Output 'BeamNG launched in Direct3D 12 mode. Change BeamNG-VRS.ini and restart to apply settings.'
    } else { Write-Output 'BeamNG launched in Vulkan mode. Change BeamNG-VRS.ini and restart to apply settings.' }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
