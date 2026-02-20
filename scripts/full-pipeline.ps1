<#
.SYNOPSIS
    MIR NUM Experiment: Full Pipeline
.DESCRIPTION
    End-to-end automation: Build -> Run -> Analyze -> Report
    Single command to reproduce the entire experiment.
.EXAMPLE
    .\scripts\full-pipeline.ps1
.NOTES
    Generated: 2026-02-19
    Requires: git, gcc, python3 (with pandas, scipy, matplotlib, numpy)
#>

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$ScriptsDir = Join-Path $ProjectRoot 'scripts'
$ResultsDir = Join-Path $ProjectRoot 'results'
$AnalyzeScript = Join-Path $ProjectRoot 'src' 'analyze-num.py'

$startTime = Get-Date

Write-Host ""
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host " MIR NUM Experiment: Full Pipeline" -ForegroundColor Cyan
Write-Host " Testing compiler inlining as Network Utility Maximization" -ForegroundColor DarkCyan
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host ""

# ---- [1/4] BUILD ----
Write-Host "[1/4] BUILD: Clone MIR, apply patches, compile" -ForegroundColor Magenta
Write-Host ("-" * 50) -ForegroundColor DarkGray

& (Join-Path $ScriptsDir 'build.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "PIPELINE FAILED at BUILD phase" -ForegroundColor Red
    exit 1
}

# ---- [2/4] RUN ----
Write-Host "[2/4] RUN: Execute all 4 experimental conditions" -ForegroundColor Magenta
Write-Host ("-" * 50) -ForegroundColor DarkGray

& (Join-Path $ScriptsDir 'run-conditions.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "PIPELINE FAILED at RUN phase" -ForegroundColor Red
    exit 1
}

# ---- [3/4] ANALYZE ----
Write-Host "[3/4] ANALYZE: Statistical tests + visualization" -ForegroundColor Magenta
Write-Host ("-" * 50) -ForegroundColor DarkGray

python $AnalyzeScript
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "PIPELINE FAILED at ANALYZE phase" -ForegroundColor Red
    Write-Host "  Check Python dependencies: pip install pandas scipy matplotlib numpy" -ForegroundColor Yellow
    exit 1
}

# ---- [4/4] REPORT ----
$elapsed = (Get-Date) - $startTime
Write-Host ""
Write-Host "[4/4] COMPLETE" -ForegroundColor Magenta
Write-Host ("-" * 50) -ForegroundColor DarkGray
Write-Host ""
Write-Host "========================================================" -ForegroundColor Green
Write-Host " Pipeline finished in $($elapsed.TotalSeconds.ToString('F1')) seconds" -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Green
Write-Host ""
Write-Host "Output files:" -ForegroundColor Yellow
Get-ChildItem -Path $ResultsDir | ForEach-Object {
    Write-Host "  $($_.Name) ($($_.Length) bytes)"
}
Write-Host ""
Write-Host "View visualization: Start-Process .\results\num-analysis.png" -ForegroundColor Cyan
Write-Host ""
