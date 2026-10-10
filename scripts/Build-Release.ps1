[CmdletBinding()]
param(
    [string]$Version = '0.1.0-beta.7',
    [string]$BuildDirectory,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?$') { throw 'Use a version such as 0.1.0-beta.7.' }
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build/windows-release' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'out/releases' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
function Run-Native {
    param([string]$Command, [string[]]$CommandArguments)
    & $Command @CommandArguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}
Run-Native -Command cmake -CommandArguments @('-S', $root, '-B', $BuildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DBEAMNG_FETCH_HEADERS=ON', '-DBUILD_TESTING=ON', '-DBEAMNG_BUILD_BENCHMARKS=ON')
Run-Native -Command cmake -CommandArguments @('--build', $BuildDirectory, '--config', 'Release', '--parallel', '2')
Run-Native -Command ctest -CommandArguments @('--test-dir', $BuildDirectory, '-C', 'Release', '--output-on-failure', '--no-tests=error')
$release = Join-Path $BuildDirectory 'Release'
$packageName = "BeamNG-VRS-$Version-windows-x64"
$stage = Join-Path $OutputDirectory ($packageName + '-staging')
if (Test-Path -LiteralPath $stage) { throw "Staging directory already exists: $stage. Choose a clean output directory." }
$zip = Join-Path $OutputDirectory ($packageName + '.zip')
if (Test-Path -LiteralPath $zip) { throw "Release ZIP already exists: $zip. Choose a new version or output directory." }
$layers = Join-Path $stage 'BeamNG-VRS'
[void](New-Item -ItemType Directory -Path $layers -Force)
foreach ($file in @('BeamNG_VRS.dll', 'BeamNG_EyeProbe.dll',
        'VK_LAYER_BEAMNG_vrs.json', 'XR_APILAYER_BEAMNG_eye_probe.json')) {
    Copy-Item -LiteralPath (Join-Path $release $file) -Destination $layers
}
foreach ($file in @('BeamNG-VRS.ini', 'Launch-BeamNG-VRS.ps1')) {
    Copy-Item -LiteralPath (Join-Path $root "packaging/$file") -Destination $layers
}
foreach ($file in @('Launch-BeamNG-VRS.cmd', 'Diagnose-BeamNG-VRS.cmd', 'READ-ME-FIRST.txt')) {
    Copy-Item -LiteralPath (Join-Path $root "packaging/$file") -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $layers
Copy-Item -LiteralPath (Join-Path $root 'docs/images/vrs-settings.png') -Destination (Join-Path $layers 'VRS-SETTINGS.png')
Run-Native -Command powershell.exe -CommandArguments @('-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', (Join-Path $layers 'Launch-BeamNG-VRS.ps1'), '-ValidateOnly')
if (@(Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.exe').Count) {
    throw 'The portable package must not contain EXE files.'
}
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Could not record source commit.' }
$dirty = @(& git -C $root status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Could not record source status.' }
@("Version: $Version", "Commit: $commit", "Uncommitted changes: $($dirty.Count -gt 0)",
  'Windows x64 / Visual Studio 2022 / Release / static MSVC runtime',
  'Vulkan headers: 952f776f6573aafbb62ea717d871cd1d6816c387',
  'OpenXR SDK headers: 7f9285bce1ce8b69bb75554bf788666579d0c35e',
  'DLL build and automated tests passed; BeamNG/headset validation is a separate release check.') |
    Set-Content -LiteralPath (Join-Path $layers 'BUILD-INFO.txt') -Encoding UTF8
$sums = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $relative
}
$sums | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ASCII
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$archiveSum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
($archiveSum + '  ' + [IO.Path]::GetFileName($zip)) | Set-Content -LiteralPath ($zip + '.sha256') -Encoding ASCII
Write-Output "Created $zip"
