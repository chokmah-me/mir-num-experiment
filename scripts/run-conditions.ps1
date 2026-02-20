<#
.SYNOPSIS
    MIR NUM Experiment: Run All Conditions
.DESCRIPTION
    Executes test_baseline.exe for each of 4 conditions:
    baseline, uniform, skewed, perturbed
    
    Each run produces a CSV in results/ directory.
.NOTES
    Generated: 2026-02-19
    Requires: test_baseline.exe (from build.ps1)
#>

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$Exe = Join-Path $ProjectRoot 'test_baseline.exe'
$ResultsDir = Join-Path $ProjectRoot 'results'
$Conditions = @('baseline', 'uniform', 'skewed', 'perturbed')

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host " MIR NUM Experiment: Run Conditions" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check executable exists
if (-not (Test-Path $Exe)) {
    Write-Host "ERROR: $Exe not found. Run build.ps1 first." -ForegroundColor Red
    exit 1
}

# Ensure results directory exists
if (-not (Test-Path $ResultsDir)) {
    New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null
}

$total = $Conditions.Count
$current = 0
$failed = 0

foreach ($cond in $Conditions) {
    $current++
    Write-Host "[$current/$total] Running condition: $cond" -ForegroundColor Yellow
    
    & $Exe $cond
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ERROR: Condition '$cond' failed (exit code $LASTEXITCODE)" -ForegroundColor Red
        $failed++
        continue
    }
    
    $csvPath = Join-Path $ResultsDir "${cond}_decisions.csv"
    if (Test-Path $csvPath) {
        $lineCount = (Get-Content $csvPath | Measure-Object -Line).Lines
        Write-Host "  OK: $csvPath ($lineCount lines)" -ForegroundColor Green
    } else {
        Write-Host "  WARN: Expected $csvPath not found" -ForegroundColor DarkYellow
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan

if ($failed -gt 0) {
    Write-Host " $failed/$total conditions FAILED" -ForegroundColor Red
    exit 1
} else {
    Write-Host " All $total conditions completed" -ForegroundColor Green
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# List output files
Write-Host "Output files:" -ForegroundColor Yellow
Get-ChildItem -Path $ResultsDir -Filter '*_decisions.csv' | ForEach-Object {
    Write-Host "  $($_.FullName) ($($_.Length) bytes)"
}

Write-Host ""
Write-Host "Next: python .\src\analyze-num.py" -ForegroundColor Yellow
Write-Host ""
