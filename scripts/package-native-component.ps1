[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Output must be a new staging directory.' }
$manifest = Get-Content -LiteralPath (Join-Path $package 'xips-native-runtime.json') -Raw | ConvertFrom-Json
$info = Get-Content -LiteralPath (Join-Path $package 'build-info.json') -Raw | ConvertFrom-Json
if ($manifest.schema -ne 'xips.native-deployment/v1' -or $info.application -ne 'xips' -or $info.qt -ne $manifest.qt) {
    throw 'Package identity or native deployment format mismatch.'
}
$component = Join-Path $output 'components/xips'
$notices = Join-Path $output 'licenses/components/xips'
New-Item -ItemType Directory -Path $component, $notices | Out-Null
function Copy-Owned([string]$Relative, [string]$DestinationRoot) {
    $source = [IO.Path]::GetFullPath((Join-Path $package $Relative))
    $destination = [IO.Path]::GetFullPath((Join-Path $DestinationRoot $Relative))
    if (-not $source.StartsWith($package.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        -not $destination.StartsWith($DestinationRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Manifest file escapes its package root.'
    }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing native component input: $Relative" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
foreach ($name in $manifest.componentFiles) { Copy-Owned $name $component }
foreach ($name in $manifest.additionalHostRuntimeFiles) { Copy-Owned $name $output }
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $package 'licenses') -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath((Join-Path $package 'licenses'), $file.FullName)
    $destination = Join-Path $notices $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $package 'THIRD-PARTY-NOTICES.md') -Destination $notices
Copy-Item -LiteralPath (Join-Path $package 'docs/PROJECT-ARCHIVE.md') -Destination $notices
$hashLines = Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),
        [IO.Path]::GetRelativePath($output, $_.FullName).Replace('\', '/')
}
$hashLines | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding utf8
Write-Output $output
