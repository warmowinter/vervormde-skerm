param(
  [string]$NativeExe = "native\dist\Vervormde-Skerm-Native-v2.1.3-win-x64\VervormdeSkermNative.exe",
  [string]$ElectronExe = "dist-release\Vervormde-Skerm-v1.0.0-win-x64\VervormdeSkerm.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path

function Resolve-AppPath([string]$Path) {
  if ([IO.Path]::IsPathRooted($Path)) {
    return (Resolve-Path -LiteralPath $Path).Path
  }
  return (Resolve-Path -LiteralPath (Join-Path $projectRoot $Path)).Path
}

function Get-AppSnapshot([string]$ProcessName) {
  $items = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
  $cpuMs = 0.0
  $workingSet = [int64]0
  $privateBytes = [int64]0
  foreach ($item in $items) {
    try {
      $cpuMs += $item.TotalProcessorTime.TotalMilliseconds
      $workingSet += $item.WorkingSet64
      $privateBytes += $item.PrivateMemorySize64
    } catch {
      # A short-lived helper may exit between enumeration and sampling.
    }
  }
  [pscustomobject]@{
    At = [DateTime]::UtcNow
    CpuMs = $cpuMs
    WorkingSet = $workingSet
    PrivateBytes = $privateBytes
    ProcessCount = $items.Count
  }
}

function Measure-App([string]$Label, [string]$Executable) {
  $processName = [IO.Path]::GetFileNameWithoutExtension($Executable)
  $process = Start-Process -FilePath $Executable -ArgumentList "--smoke-test" -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 3
  $before = Get-AppSnapshot $processName
  Start-Sleep -Seconds 2
  $after = Get-AppSnapshot $processName

  $elapsedMs = ($after.At - $before.At).TotalMilliseconds
  $cpuPercent = if ($elapsedMs -gt 0) {
    ($after.CpuMs - $before.CpuMs) / $elapsedMs / [Environment]::ProcessorCount * 100
  } else {
    0
  }

  if (-not $process.WaitForExit(15000)) {
    throw "$Label did not complete its smoke test. Use Esc to exit it safely."
  }
  Start-Sleep -Milliseconds 500

  [pscustomobject]@{
    Label = $Label
    Processes = $after.ProcessCount
    WorkingSetMiB = [Math]::Round($after.WorkingSet / 1MB, 2)
    PrivateMiB = [Math]::Round($after.PrivateBytes / 1MB, 2)
    CpuPercent = [Math]::Round($cpuPercent, 3)
    ExitCode = $process.ExitCode
    RemainingProcesses = @(Get-Process -Name $processName -ErrorAction SilentlyContinue).Count
  }
}

$native = Measure-App "Native D3D11" (Resolve-AppPath $NativeExe)
$electron = Measure-App "Electron WebGL" (Resolve-AppPath $ElectronExe)
$results = @($native, $electron)
$results | Format-Table -AutoSize

if ($electron.WorkingSetMiB -gt 0 -and $electron.PrivateMiB -gt 0 -and $electron.CpuPercent -gt 0) {
  "working_set_reduction_percent=$([Math]::Round((1 - $native.WorkingSetMiB / $electron.WorkingSetMiB) * 100, 2))"
  "private_memory_reduction_percent=$([Math]::Round((1 - $native.PrivateMiB / $electron.PrivateMiB) * 100, 2))"
  "cpu_reduction_percent=$([Math]::Round((1 - $native.CpuPercent / $electron.CpuPercent) * 100, 2))"
}
