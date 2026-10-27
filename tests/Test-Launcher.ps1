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
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -CheckOnly
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $result)) { throw 'CheckOnly failed or started the game.' }
    $diagnosticLog = $null
    if ($Diagnostics) {
        $messages = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -Diagnostics
        foreach ($message in $messages) {
            if ($message -match '^Vulkan loader diagnostics: (.+)$') { $diagnosticLog = $Matches[1] }
        }
    } else { & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder }
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    # A closed file is required: the child might have just created it.
    $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    if ($content -notmatch 'arguments=-gfx d3d12' -or
        $content -notmatch 'VK_INSTANCE_LAYERS=VK_LAYER_test_existing;VK_LAYER_BEAMNG_vrs' -or
        $content -notmatch 'XR_ENABLE_API_LAYERS=XR_APILAYER_test_existing;XR_APILAYER_BEAMNG_eye_probe') {
        throw 'The child did not receive default D3D12 arguments and layer activation.'
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
    # Every launch uses this scratch BeamNG user folder, never the real one.
    $userFolder = Join-Path $temp 'user folder'
    [void](New-Item -ItemType Directory -Path $layers, $bin, (Join-Path $userFolder 'settings'))
    Copy-Item -LiteralPath $GameFixture -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll',
            'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json')) {
        Copy-Item -LiteralPath (Join-Path $BuildDirectory $file) -Destination $layers
    }
    $ini = Join-Path $temp 'BeamNG-VRS.ini'
    Copy-Item -LiteralPath (Join-Path $source 'packaging/BeamNG-VRS.ini') -Destination $ini
    Copy-Item -LiteralPath (Join-Path $source 'packaging/Launch-BeamNG-VRS.ps1') -Destination $layers
    Copy-Item -LiteralPath (Join-Path $source 'packaging/GhostingFix.hlsl') -Destination $layers
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
        $messages = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -CheckOnly -OpenXRLayerRegistry $registry
        if ($LASTEXITCODE -ne 0 -or ($messages -join "`n") -notmatch 'XR_APILAYER_TEST_overlay \[disabled for BeamNG' -or
            ($messages -join "`n") -notmatch 'XR_APILAYER_TEST_off \[off in registry\]') { throw 'CheckOnly did not report implicit OpenXR layers.' }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -OpenXRLayerRegistry $registry
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
        # Omitted DisableLayers defaults to *; an explicit empty value keeps every layer.
        foreach ($case in @(@("[Logging]`nLevel=info", '1'), @("[OpenXR]`nDisableLayers=", ''))) {
            Set-Content -LiteralPath $ini -Value $case[0] -Encoding UTF8
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -OpenXRLayerRegistry $registry
            if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with default OpenXR layer handling.' }
            $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
            while ([DateTime]::UtcNow -lt $deadline) {
                try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'DISABLE_XR_APILAYER_TEST_off=') { break } } catch {}
                Start-Sleep -Milliseconds 100
            }
            if ($content -notmatch ('(?m)^DISABLE_XR_APILAYER_TEST_overlay=' + $case[1] + '\r?$')) { throw "Unexpected default OpenXR layer handling for: $($case[0])" }
            Remove-Item -LiteralPath $result
        }
    } finally { Remove-Item -LiteralPath (Split-Path -Parent $registry) -Recurse -Force -ErrorAction SilentlyContinue }
    # M8.48: RenderScale writes the Lua mod into the user folder; omitting it (and
    # VRDrawDistance=off) removes the mod.
    $modLua = Join-Path $userFolder 'mods\unpacked\beamng_vrs\lua\ge\extensions\beamngVrs.lua'
    Set-Content -LiteralPath $ini -Value "[Renderer]`nRenderScale=0.8" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with RenderScale.' }
    if (-not (Test-Path -LiteralPath $modLua) -or (Get-Content -LiteralPath $modLua -Raw) -notmatch 'local scale = 0\.8\r?\n' -or
        -not (Test-Path -LiteralPath (Join-Path $userFolder 'mods\unpacked\beamng_vrs\scripts\beamng_vrs\modScript.lua'))) { throw 'RenderScale mod was not written.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    Remove-Item -LiteralPath $result
    # M8.51: the default VRDrawDistance=4000 adds the live UI app and radial menu.
    $app = Join-Path $userFolder 'mods\unpacked\beamng_vrs\ui\modules\apps\BeamngVrsDrawDistance'
    $distanceFile = Join-Path $userFolder 'settings\beamng_vrs_vr_draw_distance.txt'
    $modText = Get-Content -LiteralPath $modLua -Raw
    if ($modText -notmatch 'local distance = 4000\r?\n' -or $modText -notmatch 'local iniDistance = 4000\r?\n' -or
        $modText -notmatch 'M\.setVrDrawDistance' -or $modText -notmatch 'core_quickAccess\.addEntry' -or
        -not (Test-Path -LiteralPath (Join-Path $app 'app.js')) -or -not (Test-Path -LiteralPath (Join-Path $app 'app.json'))) {
        throw 'VR draw distance app was not written.'
    }
    Add-Type -AssemblyName System.Drawing
    $png = [Drawing.Image]::FromFile((Join-Path $app 'app.png'))
    try { if ($png.Width -ne 250 -or $png.Height -ne 120) { throw 'VR draw distance app.png is not 250x120.' } } finally { $png.Dispose() }
    [void](ConvertFrom-Json (Get-Content -LiteralPath (Join-Path $app 'app.json') -Raw))
    # The in-game choice persists while the INI value is unchanged; a changed INI value wins.
    foreach ($case in @(@('3000', "distance=1500`nini=3000`n", '1500', $true), @('5000', "distance=1500`nini=3000`n", '', $false),
                        @('3000', "distance=99999`nini=3000`n", '', $false))) {
        Set-Content -LiteralPath $ini -Value "[Renderer]`nVRDrawDistance=$($case[0])" -Encoding UTF8
        [IO.File]::WriteAllText($distanceFile, $case[1])
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
        if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with VRDrawDistance.' }
        $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
        while ([DateTime]::UtcNow -lt $deadline) {
            try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
            Start-Sleep -Milliseconds 100
        }
        $expected = if ($case[2]) { $case[2] } else { $case[0] }
        if ($content -notmatch ('(?m)^BEAMNG_VRS_VR_DRAW_DISTANCE=' + $case[2] + '\r?$') -or
            -not $content.Contains('BEAMNG_VRS_VR_DRAW_DISTANCE_FILE=' + $distanceFile) -or
            (Get-Content -LiteralPath $modLua -Raw) -notmatch ('local distance = ' + $expected + '\r?\n') -or
            (Test-Path -LiteralPath $distanceFile) -ne $case[3]) {
            throw "Unexpected VR draw distance start for INI $($case[0]) and saved $($case[1])."
        }
        Remove-Item -LiteralPath $result
    }
    Set-Content -LiteralPath $ini -Value "[Logging]`nLevel=info`n[Renderer]`nVRDrawDistance=OFF" -Encoding UTF8
    [IO.File]::WriteAllText($distanceFile, "distance=1500`nini=3000`n")
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath (Join-Path $userFolder 'mods\unpacked\beamng_vrs'))) { throw 'RenderScale mod was not removed.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    if ($content -notmatch '(?m)^BEAMNG_VRS_VR_DRAW_DISTANCE_FILE=\r?$' -or -not (Test-Path -LiteralPath $distanceFile)) {
        throw 'VRDrawDistance=off still used the live file.'
    }
    Remove-Item -LiteralPath $result
    # [Renderer] GhostingFix (default false; opt-in) patches the game's combinePass shader into the
    # user folder and clears the shader cache only when that file changes. This stand-in
    # has only the stock lines the patch anchors on (the real shader is the game's own).
    $stockShader = Join-Path $temp 'shaders\common\postfx\hdr\combinePass.cs.hlsl'
    $patchedShader = Join-Path $userFolder 'shaders\common\postfx\hdr\combinePass.cs.hlsl'
    $shaderCache = Join-Path $userFolder 'temp\shaders'
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $stockShader), $shaderCache -Force)
    $stockText = "Texture2D<float4> backbuffer : register(t0);`r`nTexture2D<float4> bloomFinal : register(t2);`r`n" +
        "Texture2D<float4> localExposureBlurredLogLum : register(t4);`r`nSamplerState samplerLinear : register(s0);`r`n#ifdef BNG_COCONUT`r`n#endif`r`n" +
        "uint2 getRtDimensions() {`r`n    uint2 dim;`r`n    destTex.GetDimensions(dim.x, dim.y);`r`n    return dim;`r`n}`r`n" +
        "const float localGain = computeLocalExposure(sample.rgb, uv, exposureScale,`r`n" +
        "sample.rgb += bloomScale * bloomFinal.SampleLevel(samplerLinear, uv, 0).rgb;`r`n" +
        "destTex[storePos] = float4(sampleOutput.rgb, 1);`r`n" +
        "void main(uint3 dispatchThreadID : SV_DispatchThreadID)`r`n{`r`n    if (any(dispatchThreadID.xy >= getRtDimensions())) return;`r`n}`r`n"
    [IO.File]::WriteAllText($stockShader, $stockText)
    Set-Content -LiteralPath $ini -Value "[Renderer]`nVRDrawDistance=off" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $patchedShader) -or -not (Test-Path -LiteralPath $shaderCache)) {
        throw 'The ghosting fix was applied although it is off by default.'
    }
    Set-Content -LiteralPath $ini -Value "[Renderer]`nVRDrawDistance=off`nGhostingFix=true" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with the ghosting fix.' }
    $patched = if (Test-Path -LiteralPath $patchedShader) { Get-Content -LiteralPath $patchedShader -Raw } else { '' }
    if (-not $patched.Contains('BeamNG-VRS ghosting fix') -or -not $patched.Contains('saturate(uv + vrsLocalExposureShift)') -or
        -not $patched.Contains('saturate(uv + vrsBloomShift)') -or -not $patched.Contains('vrsEstimateEyeShift(groupIndex);') -or
        (Test-Path -LiteralPath $shaderCache)) { throw 'Ghosting fix shader was not written or the shader cache was not cleared.' }
    [void](New-Item -ItemType Directory -Path $shaderCache)
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $shaderCache)) { throw 'An unchanged ghosting fix cleared the shader cache.' }
    Set-Content -LiteralPath $ini -Value "[Renderer]`nVRDrawDistance=off`nGhostingFix=false" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $patchedShader) -or (Test-Path -LiteralPath $shaderCache)) { throw 'GhostingFix=false did not remove the shader.' }
    # Another mod's override is never touched; a changed game shader is left alone.
    [IO.File]::WriteAllText($patchedShader, 'other mod')
    Set-Content -LiteralPath $ini -Value "[Renderer]`nVRDrawDistance=off`nGhostingFix=true" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or (Get-Content -LiteralPath $patchedShader -Raw) -ne 'other mod') { throw 'Another override was replaced.' }
    Remove-Item -LiteralPath $patchedShader
    [IO.File]::WriteAllText($stockShader, $stockText.Replace('uv, exposureScale', 'uv * 2, exposureScale'))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $patchedShader)) { throw 'A changed game shader was patched.' }
    Remove-Item -LiteralPath (Join-Path $temp 'shaders') -Recurse
    Remove-Item -LiteralPath $result -ErrorAction SilentlyContinue
    # [Renderer] API=vulkan keeps the Vulkan renderer, where the VRS layer applies.
    Set-Content -LiteralPath $ini -Value "[Renderer]`nAPI=vulkan" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -ne 0) { throw 'Launcher failed with the Vulkan renderer.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $content = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $content = Get-Content -LiteralPath $result -Raw; if ($content -match 'XR_ENABLE_API_LAYERS=.*probe') { break } } catch {}
        Start-Sleep -Milliseconds 100
    }
    if ($content -notmatch '(?m)^arguments=-gfx vk\r?$' -or $content -notmatch 'VK_INSTANCE_LAYERS=.*VK_LAYER_BEAMNG_vrs') {
        throw 'The child did not receive Vulkan arguments and layer activation.'
    }
    Remove-Item -LiteralPath $result
    # The public launcher must not silently edit/use an obsolete nested INI.
    Move-Item -LiteralPath $ini -Destination $layers
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -CheckOnly
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Missing root INI was not rejected.' }
    Move-Item -LiteralPath (Join-Path $layers 'BeamNG-VRS.ini') -Destination $ini
    Remove-Item -LiteralPath (Join-Path $bin 'BeamNG.drive.x64.exe')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder -CheckOnly
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Missing executable was not rejected.' }
    Copy-Item -LiteralPath $GameFixture -Destination (Join-Path $bin 'BeamNG.drive.x64.exe')
    Set-Content -LiteralPath $ini -Value "[Debug]`nDrawOutline=typo" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -UserFolder $userFolder
    if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath $result)) { throw 'Invalid INI started the game.' }
    Write-Output 'Launcher integration passed: root/Bin64 layouts, root INI, both executable names, working directory, Vulkan discovery, diagnostic capture, arguments, child environment, CheckOnly, implicit OpenXR layer disabling, RenderScale mod, VR draw distance app and persistence, ghosting fix shader, D3D12 default and Vulkan renderer, missing INI/executable and invalid INI.'
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $before[$name], 'Process') }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
