param([Parameter(Mandatory)][string]$ParserFixture)
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('BeamNG INI validation ' + [Guid]::NewGuid())
try {
    $layers = Join-Path $temporary 'BeamNG-VRS'
    [void](New-Item -ItemType Directory -Path $layers -Force)
    $launcher = Join-Path $layers 'Launch-BeamNG-VRS.ps1'
    Copy-Item -LiteralPath (Join-Path $source 'packaging/Launch-BeamNG-VRS.ps1') -Destination $launcher
    $ini = Join-Path $temporary 'BeamNG-VRS.ini'
    $cases = @(
        @{ Text = [IO.File]::ReadAllText((Join-Path $source 'packaging/BeamNG-VRS.ini')); Valid = $true },
        @{ Text = "[Debug]`nDrawOutline=false"; Valid = $true },
        @{ Text = "[Renderer]`nFarClip=2000"; Valid = $true },
        @{ Text = "[Foveation]`nSize1x1=.25`nSize2x2=6e-1`nShape=ELLIPSE"; Valid = $true },
        @{ Text = "[Foveation]`nSize1x1=0`nSize2x2=0`nShape=ellipse"; Valid = $true },
        @{ Text = "[Foveation]`nSize1x1=0.0`nSize2x2=0.0`nShape=ellipse"; Valid = $true },
        @{ Text = "[Foveation]`nSize1x1=0.1`nSize2x2=0.1`nShape=ellipse"; Valid = $true },
        @{ Text = "[Foveation]`nShape=circle`n[Debug]`nDrawOutline=1"; Valid = $true },
        @{ Text = "[Foveation]`nShape=square`n[Scissor]`nEnabled=TRUE`nTop=0.49`nBottom=0"; Valid = $true },
        @{ Text = "[Scissor]`nLeft=0`nRight=0"; Valid = $true },
        @{ Text = "[Scissor]`nLeft=0.49`nRight=0.49"; Valid = $true },
        @{ Text = "[Scissor]`nTop=0.30`nBottom=0.30`nLeft=0.05`nRight=0.10"; Valid = $true },
        @{ Text = "[Scissor]`nLeftEyeLeft=0.05`nLeftEyeRight=0`nRightEyeLeft=0`nRightEyeRight=0.49"; Valid = $true },
        @{ Text = "[Scissor]`nLeftEyeLeft=0`nLeftEyeRight=0.49`nRightEyeLeft=0.49`nRightEyeRight=0"; Valid = $true },
        @{ Text = "[Scissor]`nLeft=0.1`nRightEyeLeft=0"; Valid = $true },
        @{ Text = "[Foveation]`nEnabled=false`n[Scissor]`nEnabled=true`n[Performance]`nTurboMode=true"; Valid = $true },
        @{ Text = "[Foveation]`nEnabled=TRUE`n[Scissor]`nEnabled=0`n[Performance]`nTurboMode=0"; Valid = $true },
        @{ Text = "[Performance]`nTurboMode=1"; Valid = $true },
        @{ Text = "[Foveation]`nAllPasses=false"; Valid = $true },
        @{ Text = "[LensMask]`nEnabled=true`nSize=1.05"; Valid = $true },
        @{ Text = "[Logging]`nLevel=off"; Valid = $true },
        @{ Text = "[OpenXR]`nDisableLayers="; Valid = $true },
        @{ Text = "[Renderer]`nAPI=vulkan"; Valid = $true },
        @{ Text = "[renderer]`napi = D3D12"; Valid = $true },
        @{ Text = "[Renderer]`nRenderScale=1"; Valid = $true },
        @{ Text = "[Renderer]`nRenderScale=0.25"; Valid = $true },
        @{ Text = "[Renderer]`nVRDrawDistance=800"; Valid = $true },
        @{ Text = "[Renderer]`nVRDrawDistance=off"; Valid = $true },
        @{ Text = "[renderer]`nvrdrawdistance = Off"; Valid = $true },
        @{ Text = "[OpenXR]`nDisableLayers=*"; Valid = $true },
        @{ Text = "[openxr]`ndisablelayers = XR_APILAYER_A_overlay , XR_APILAYER_b.2-x"; Valid = $true },
        @{ Text = "[Logging]`nLevel=errors"; Valid = $true },
        @{ Text = "[Logging]`nLevel=INFO"; Valid = $true },
        @{ Text = "[Logging]`nLevel=debug"; Valid = $true },
        @{ Text = "[Tracking]`nGracePeriodMs=0`nGazeRefreshMs=50"; Valid = $true },
        @{ Text = "[Tracking]`nGracePeriodMs=1000`nGazeRefreshMs=2"; Valid = $true },
        @{ Text = ([string][char]0xFEFF) + "; comment`r`n [DeBuG]`r`nDrawOutline = TRUE`r`n[Scissor]`r`nBottom=+0.30`r`n"; Valid = $true }
    )
    foreach ($bad in @(
        "[Logging]`nLevel=verbose", "[Logging]`nLevel=off`nlevel=info", "[Performance]`nTurboMode=yes", "[Performance]`nTurboMode=true`nturbomode=false",
        "[Foveation]`nAllPasses=yes", "[LensMask]`nSize=1.6", "[LensMask]`nSize=0.4", "[LensMask]`nEnabled=on", "[Foveation]`nEnabled=2", "[Foveation]`nEnabled=true`nenabled=false", "[Foveation]`nTypo=1", "[other]`nEnabled=1", "[Debug]`nDrawOutline=1`ndrawoutline=0",
        "[Debug]`nDrawOutline=1`n[DEBUG]`nDrawOutline=0", "[Foveation]`nSize1x1=nan",
        "[Foveation]`nSize1x1=Infinity", "[Foveation]`nSize1x1=1.2", "[Foveation]`nSize1x1=-1",
        "[Foveation]`nSize1x1=0.7`nSize2x2=0.5", "[Foveation]`nSize1x1=0.7",
        "[Foveation]`nSize2x2=0.1", "[Foveation]`nSize1x1=0,25", "[Scissor]`nTop=0.5",
        "[Scissor]`nBottom=-0.1", "[Scissor]`nEnabled=yes", "[Tracking]`nGracePeriodMs=1001",
        "[Scissor]`nLeft=-0.1", "[Scissor]`nLeft=0.5", "[Scissor]`nRight=0.5",
        "[Scissor]`nLeftEyeLeft=-0.1", "[Scissor]`nLeftEyeRight=0.5",
        "[Scissor]`nRightEyeLeft=NaN", "[Scissor]`nRightEyeRight=0.50",
        "[Scissor]`nLeftEyeLeft=0`nlefteyeleft=0.1", "[Scissor]`nRight=NaN", "[Scissor]`nLeft=0`nleft=0.1",
        "[Tracking]`nGazeRefreshMs=1", "[Tracking]`nGazeRefreshMs=8.5",
        "[Tracking]`nGazeRefreshMs=8.0", "[Tracking]`nGazeRefreshMs=+8",
        "[Tracking]`nGazeRefreshMs=8e0", "[Foveation]`nShape=triangle",
        "[Debug]`nDrawOutline=1 ; inline comment",
        "[OpenXR]`nDisableLayers=XR layer", "[OpenXR]`nDisableLayers=A,,B", "[OpenXR]`nDisableLayers=A,", "[OpenXR]`nDisableLayers=A,*",
        "[OpenXR]`nDisableLayers=A`ndisablelayers=B", "[OpenXR]`nTypo=1", "[Renderer]`nAPI=dx12", "[Renderer]`nRenderScale=1.5", "[Renderer]`nRenderScale=0.2", "[Renderer]`nRenderScale=auto", "[Renderer]`nVRDrawDistance=50", "[Renderer]`nVRDrawDistance=20000", "[Renderer]`nVRDrawDistance=on", "[Renderer]`nVRDrawDistance=","[Renderer]`nAPI=", "[Debug", "DrawOutline=1", "# empty`n",
        '', "[Debug]`nDrawOutline="
    )) { $cases += @{ Text = $bad; Valid = $false } }
    $caseNumber = 0
    foreach ($case in $cases) {
        ++$caseNumber
        [IO.File]::WriteAllText($ini, $case.Text, (New-Object Text.UTF8Encoding($false)))
        # Windows PowerShell 5.1 turns redirected native stderr into error
        # records. Expected parser rejections must not stop the test itself.
        $preference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $ParserFixture $ini > $null 2> (Join-Path $temporary 'cpp-error.txt')
            $cppValid = $LASTEXITCODE -eq 0
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $launcher -ValidateOnly > $null 2> (Join-Path $temporary 'ps-error.txt')
            $psValid = $LASTEXITCODE -eq 0
        } finally { $ErrorActionPreference = $preference }
        if ($cppValid -ne $case.Valid -or $psValid -ne $cppValid) {
            throw "INI validation case $caseNumber disagreed: expected=$($case.Valid), C++=$cppValid, PowerShell=$psValid."
        }
    }
    Write-Output "Launcher and DLL INI validation agree on $caseNumber cases without a packaged EXE."
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
