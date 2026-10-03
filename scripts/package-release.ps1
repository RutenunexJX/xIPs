[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$QtDirectory = 'E:/QT6/6.10.2/mingw_64',
    [string]$CompilerDirectory = 'E:/QT6/Tools/mingw1310_64',
    [switch]$Formal,
    [switch]$Standalone
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$versionMatch = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt')),
    'project\(xIPs VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { throw 'Cannot read the xIPs version.' }
$version = $versionMatch.Groups[1].Value
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputRoot) { throw 'Output must be a new directory.' }
$cache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt')
if ($cache -notcontains 'CMAKE_BUILD_TYPE:STRING=Release' -or
    $cache -notcontains 'BUILD_TESTING:BOOL=OFF') { throw 'Use a Release build with test hooks disabled.' }
$sourceEntry = $cache | Where-Object { $_.StartsWith('CMAKE_HOME_DIRECTORY:INTERNAL=') }
if (-not $sourceEntry -or [IO.Path]::GetFullPath(($sourceEntry -split '=',2)[1]) -ne
    [IO.Path]::GetFullPath($sourceRoot)) { throw 'Build belongs to a different source checkout.' }
$revision = & git -C $sourceRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read Git revision.' }
$dirty = [bool](& git -C $sourceRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read Git status.' }
$tag = "v$version"
if ($Formal) {
    if ($dirty) { throw 'A formal package requires a clean source checkout.' }
    $tagRevision = & git -C $sourceRoot rev-parse "$tag^{commit}"
    if ($LASTEXITCODE -ne 0 -or $tagRevision -ne $revision) { throw 'Release tag must identify HEAD.' }
}
$suiteEntry = $cache | Where-Object { $_.StartsWith('SuiteApp_DIR:') }
$suiteSdkVersion = $null
if ($Standalone) {
    if ($cache -notcontains 'XIPS_ENABLE_SUITEAPP:BOOL=OFF') {
        throw 'Standalone packaging requires XIPS_ENABLE_SUITEAPP=OFF.'
    }
} else {
    if ($cache -notcontains 'XIPS_ENABLE_SUITEAPP:BOOL=ON' -or -not $suiteEntry) {
        throw 'The AppSuite package requires the SuiteApp SDK, or select -Standalone explicitly.'
    }
    $suiteVersionFile = Join-Path (($suiteEntry -split '=',2)[1]) 'SuiteAppConfigVersion.cmake'
    $suiteVersionMatch = [regex]::Match((Get-Content -Raw -LiteralPath $suiteVersionFile),
        'set\(PACKAGE_VERSION "([^"]+)"\)')
    if (-not $suiteVersionMatch.Success) { throw 'Cannot read the SuiteApp SDK version.' }
    $suiteSdkVersion = $suiteVersionMatch.Groups[1].Value
}
$binaryRoot = Join-Path $buildRoot 'bin'
$binaries = @('xips.exe', 'xips-cli.exe', 'xips-browser.dll', 'xips-browser-impl.dll', 'XipsEla.dll')
foreach ($name in $binaries) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaryRoot $name) -PathType Leaf)) { throw "Missing $name" }
}
if ((Get-Item -LiteralPath (Join-Path $binaryRoot 'xips.exe')).VersionInfo.ProductVersion -ne $version) {
    throw 'Executable version differs from the source version.'
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
foreach ($name in $binaries) {
    $destination = Join-Path $outputRoot $name
    Copy-Item -LiteralPath (Join-Path $binaryRoot $name) -Destination $destination
    & (Join-Path $CompilerDirectory 'bin/strip.exe') --strip-debug $destination
    if ($LASTEXITCODE -ne 0) { throw "strip failed for $name" }
}
$deployTargets = $binaries | ForEach-Object { Join-Path $outputRoot $_ }
& (Join-Path $QtDirectory 'bin/windeployqt.exe') --release --no-translations --no-compiler-runtime `
    --no-system-d3d-compiler --no-opengl-sw --dir $outputRoot @deployTargets
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }
foreach ($name in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $CompilerDirectory "bin/$name") -Destination $outputRoot
}
$icons = Join-Path $outputRoot 'assets/icons'
$licenses = Join-Path $outputRoot 'licenses'
New-Item -ItemType Directory -Path $icons, $licenses | Out-Null
foreach ($name in @('xips.ico', 'xips-256.png', 'DESIGN.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot "assets/icons/$name") -Destination $icons
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'thirdparty/elawidgettools/LICENSE') -Destination (Join-Path $licenses 'ElaWidgetTools-MIT.txt')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'thirdparty/elawidgettools/Font/FontAwesome-LICENSE.txt') -Destination (Join-Path $licenses 'FontAwesome.txt')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'thirdparty/elawidgettools/UPSTREAM-REVISION.md') -Destination (Join-Path $licenses 'ElaWidgetTools-provenance.md')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'thirdparty/elawidgettools/patches') -Destination (Join-Path $licenses 'ElaWidgetTools-patches') -Recurse
$capabilities = Get-Content -Raw -LiteralPath (Join-Path $buildRoot 'xips-capabilities.json') | ConvertFrom-Json
Copy-Item -LiteralPath (Join-Path $buildRoot 'xips-capabilities.json') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $CompilerDirectory 'licenses/gcc/COPYING3.LIB') -Destination (Join-Path $licenses 'Qt-LGPLv3.txt')
foreach ($name in @('COPYING3', 'COPYING3.LIB', 'COPYING.RUNTIME')) {
    Copy-Item -LiteralPath (Join-Path $CompilerDirectory "licenses/gcc/$name") -Destination (Join-Path $licenses "GCC-$name.txt")
}
Copy-Item -LiteralPath (Join-Path $CompilerDirectory 'licenses/winpthreads/COPYING') -Destination (Join-Path $licenses 'winpthreads-COPYING.txt')
Copy-Item -LiteralPath (Join-Path $CompilerDirectory 'licenses/mingw-w64/COPYING.MinGW-w64.txt') -Destination (Join-Path $licenses 'MinGW-w64-COPYING.txt')
foreach ($name in @('README.md', 'CHANGELOG.md', 'THIRD-PARTY-NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination $outputRoot
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs') -Destination (Join-Path $outputRoot 'docs') -Recurse
[ordered]@{
    application = 'xips'; version = $version; revision = $revision; dirty = $dirty
    channel = $(if ($Formal) { 'formal' } else { 'preview' })
    releaseTag = $(if ($Formal) { $tag } else { $null })
    backend = 'ela'; qt = '6.10.2'; compiler = 'MinGW 13.1'; nativeSurfaceAbi = 1
    suiteProtocol = $(if ($Standalone) { $null } else { 'suite-app/v1' }); suiteSdk = $suiteSdkVersion
    distribution = $(if ($Standalone) { 'standalone' } else { 'appsuite' }); appSuiteEnabled = -not [bool]$Standalone
    elaBaseline = $capabilities.elaBaseline; elaSourceSha256 = $capabilities.elaSourceSha256
    elaPatchLevel = $capabilities.elaPatchLevel
    elaRuntime = 'XipsEla.dll'
    elaDllSha256 = (Get-FileHash -LiteralPath (Join-Path $outputRoot 'XipsEla.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    builtAtUtc = [DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'build-info.json') -Encoding utf8
$hashLines = Get-ChildItem -LiteralPath $outputRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($outputRoot, $_.FullName).Replace('\', '/')
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
$hashLines | Set-Content -LiteralPath (Join-Path $outputRoot 'SHA256SUMS.txt') -Encoding utf8
Write-Output $outputRoot
