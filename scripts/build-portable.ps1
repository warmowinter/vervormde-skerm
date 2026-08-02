param(
  [string]$Version = "1.0.0"
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$electronDist = (Resolve-Path -LiteralPath (Join-Path $projectRoot "node_modules\electron\dist")).Path
$releaseRoot = Join-Path $projectRoot "dist-release"
$folderName = "Vervormde-Skerm-v$Version-win-x64"
$portableRoot = Join-Path $releaseRoot $folderName
$zipPath = Join-Path $releaseRoot "$folderName.zip"
$hashPath = "$zipPath.sha256"

$projectPrefix = $projectRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$resolvedReleaseParent = [IO.Path]::GetFullPath((Split-Path -Parent $releaseRoot)).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $releaseRoot.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not $resolvedReleaseParent.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to build outside the project directory: $releaseRoot"
}

if (Test-Path -LiteralPath $releaseRoot) {
  $resolvedReleaseRoot = (Resolve-Path -LiteralPath $releaseRoot).Path
  if (-not $resolvedReleaseRoot.Equals([IO.Path]::GetFullPath($releaseRoot), [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unexpected release output path: $resolvedReleaseRoot"
  }
  Remove-Item -LiteralPath $resolvedReleaseRoot -Recurse -Force
}

New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
Copy-Item -Path (Join-Path $electronDist "*") -Destination $portableRoot -Recurse -Force

$electronExe = Join-Path $portableRoot "electron.exe"
$appExe = Join-Path $portableRoot "VervormdeSkerm.exe"
Move-Item -LiteralPath $electronExe -Destination $appExe

$resourcesRoot = Join-Path $portableRoot "resources"
$defaultApp = Join-Path $resourcesRoot "default_app.asar"
if (Test-Path -LiteralPath $defaultApp) {
  Remove-Item -LiteralPath $defaultApp -Force
}

$appRoot = Join-Path $resourcesRoot "app"
New-Item -ItemType Directory -Path $appRoot -Force | Out-Null

$appFiles = @(
  "package.json",
  "main.js",
  "preload.js",
  "index.html",
  "renderer.js",
  "styles.css",
  "undistort.html",
  "undistort-renderer.js",
  "undistort-control.html",
  "undistort-control.js",
  "undistort-control.css",
  "README.md",
  "README-undistort.md",
  "LICENSE"
)

foreach ($relativePath in $appFiles) {
  $sourcePath = Join-Path $projectRoot $relativePath
  if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "Required application file is missing: $sourcePath"
  }
  Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $appRoot $relativePath)
}

Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination (Join-Path $portableRoot "APP-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $projectRoot "release-assets\PORTABLE-README.txt") -Destination (Join-Path $portableRoot "README.txt")
Copy-Item -LiteralPath (Join-Path $projectRoot "release-assets\Start Black Hole.cmd") -Destination (Join-Path $portableRoot "Start Black Hole.cmd")

Compress-Archive -LiteralPath $portableRoot -DestinationPath $zipPath -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $hashPath -Value "$hash  $([IO.Path]::GetFileName($zipPath))" -Encoding Ascii

Write-Host "Portable build created:"
Write-Host "  $zipPath"
Write-Host "  $hashPath"
