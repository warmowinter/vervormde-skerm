param(
  [string]$Version = "2.1.1"
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$nativeRoot = Join-Path $projectRoot "native"
$projectFile = Join-Path $nativeRoot "VervormdeSkermNative.vcxproj"
$distRoot = Join-Path $nativeRoot "dist"
$folderName = "Vervormde-Skerm-Native-v$Version-win-x64"
$portableRoot = Join-Path $distRoot $folderName
$zipPath = Join-Path $distRoot "$folderName.zip"
$zipHashPath = "$zipPath.sha256"
$standaloneExePath = Join-Path $distRoot "VervormdeSkermNative-v$Version-win-x64.exe"
$standaloneHashPath = "$standaloneExePath.sha256"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
  throw "Visual Studio Installer helper was not found: $vswhere"
}

$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
  throw "Visual Studio 2022 C++ build tools were not found."
}
$msbuild = Join-Path $installationPath.Trim() "MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
  throw "MSBuild was not found: $msbuild"
}

$nativePrefix = $nativeRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $distRoot.StartsWith($nativePrefix, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to write outside the native project directory: $distRoot"
}
if (Test-Path -LiteralPath $distRoot) {
  $resolvedDist = (Resolve-Path -LiteralPath $distRoot).Path
  if (-not $resolvedDist.Equals([IO.Path]::GetFullPath($distRoot), [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unexpected native output path: $resolvedDist"
  }
  Remove-Item -LiteralPath $resolvedDist -Recurse -Force
}

& $msbuild $projectFile /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
if ($LASTEXITCODE -ne 0) {
  throw "Native x64 build failed with exit code $LASTEXITCODE."
}

$builtExe = Join-Path $nativeRoot "build\Release\VervormdeSkermNative.exe"
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
  throw "Native executable was not produced: $builtExe"
}

New-Item -ItemType Directory -Path $portableRoot -Force | Out-Null
Copy-Item -LiteralPath $builtExe -Destination (Join-Path $portableRoot "VervormdeSkermNative.exe")
Copy-Item -LiteralPath $builtExe -Destination $standaloneExePath
Copy-Item -LiteralPath (Join-Path $nativeRoot "README.md") -Destination (Join-Path $portableRoot "README.md")
Copy-Item -LiteralPath (Join-Path $projectRoot "CHANGELOG.md") -Destination (Join-Path $portableRoot "CHANGELOG.md")
Copy-Item -LiteralPath (Join-Path $projectRoot "AUTHORS.md") -Destination (Join-Path $portableRoot "AUTHORS.md")
Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination (Join-Path $portableRoot "LICENSE")

Compress-Archive -LiteralPath $portableRoot -DestinationPath $zipPath -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$standaloneHash = (Get-FileHash -LiteralPath $standaloneExePath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $zipHashPath -Value "$zipHash  $([IO.Path]::GetFileName($zipPath))" -Encoding Ascii
Set-Content -LiteralPath $standaloneHashPath -Value "$standaloneHash  $([IO.Path]::GetFileName($standaloneExePath))" -Encoding Ascii

Write-Host "Native release artifacts created:"
Write-Host "  $standaloneExePath"
Write-Host "  $standaloneHashPath"
Write-Host "  $zipPath"
Write-Host "  $zipHashPath"
