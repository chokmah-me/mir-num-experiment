<#
.SYNOPSIS
    MIR NUM Experiment: Build Script
.DESCRIPTION
    Clones MIR, applies instrumentation patches (shadow_price + logging),
    and compiles the test harness executable.
    
    Uses git apply for patches with automatic sed-based fallback if patches fail.
.NOTES
    Generated: 2026-02-19
    Requires: git, gcc (MinGW or MSYS2), PowerShell 7+
#>

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

# ---- Configuration ----
$MirDir      = Join-Path $ProjectRoot 'mir-baseline'
$PatchDir    = Join-Path $ProjectRoot 'mir-patches'
$SrcDir      = Join-Path $ProjectRoot 'src'
$ResultsDir  = Join-Path $ProjectRoot 'results'
$OutputExe   = Join-Path $ProjectRoot 'test_baseline.exe'

$Patch1 = Join-Path $PatchDir '01-mir-h-shadow-price.patch'
$Patch2 = Join-Path $PatchDir '02-mir-gen-c-logging.patch'

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host " MIR NUM Experiment: Build" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# ---- Phase 1: Clone MIR ----
Write-Host "[1/4] Cloning MIR repository..." -ForegroundColor Yellow

if (Test-Path $MirDir) {
    Write-Host "  mir-baseline/ already exists, removing..." -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $MirDir
}

git clone --depth 1 https://github.com/vnmakarov/mir.git $MirDir
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: git clone failed" -ForegroundColor Red
    exit 1
}
Write-Host "  OK: MIR cloned" -ForegroundColor Green

# ---- Phase 2: Apply Patch 1 (mir.h — shadow_price field) ----
Write-Host ""
Write-Host "[2/4] Applying Patch 1 (mir.h: shadow_price field)..." -ForegroundColor Yellow

$mirH = Join-Path $MirDir 'mir.h'
Set-Location $MirDir

# Try git apply first
git apply $Patch1 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "  OK: Patch 1 applied via git apply" -ForegroundColor Green
} else {
    Write-Host "  WARN: git apply failed, using sed fallback..." -ForegroundColor DarkYellow
    
    # Sed-based fallback: insert shadow_price after machine_code field
    $content = Get-Content $mirH -Raw
    $marker = 'void *machine_code;'
    if ($content -match [regex]::Escape($marker)) {
        $replacement = $marker + "`n" + '  double shadow_price;            /* NUM experiment: utility signal (default 1.0) */'
        $content = $content -replace [regex]::Escape($marker), $replacement
        Set-Content -Path $mirH -Value $content -NoNewline
        Write-Host "  OK: Patch 1 applied via text insertion" -ForegroundColor Green
    } else {
        # Try alternate: search for the comment version
        $marker2 = 'void *machine_code; '
        if ($content.Contains('machine_code')) {
            # Find the line and insert after it
            $lines = Get-Content $mirH
            $newLines = @()
            $inserted = $false
            foreach ($line in $lines) {
                $newLines += $line
                if ((-not $inserted) -and ($line -match 'void \*machine_code;')) {
                    $newLines += '  double shadow_price;            /* NUM experiment: utility signal (default 1.0) */'
                    $inserted = $true
                }
            }
            if ($inserted) {
                Set-Content -Path $mirH -Value ($newLines -join "`n") -NoNewline
                Write-Host "  OK: Patch 1 applied via line insertion" -ForegroundColor Green
            } else {
                Write-Host "  ERROR: Could not find machine_code field in mir.h" -ForegroundColor Red
                exit 1
            }
        } else {
            Write-Host "  ERROR: Could not find insertion point in mir.h" -ForegroundColor Red
            exit 1
        }
    }
}

# Verify patch 1
$verify1 = Select-String -Path $mirH -Pattern 'shadow_price' -SimpleMatch
if (-not $verify1) {
    Write-Host "  ERROR: shadow_price not found in mir.h after patching" -ForegroundColor Red
    exit 1
}
Write-Host "  Verified: shadow_price field present in mir.h" -ForegroundColor DarkGreen

# ---- Phase 3: Apply Patch 2 (mir-gen.c — logging + inlining) ----
Write-Host ""
Write-Host "[3/4] Applying Patch 2 (mir-gen.c: logging framework)..." -ForegroundColor Yellow

$mirGenC = Join-Path $MirDir 'mir-gen.c'

# Try git apply first
git apply $Patch2 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "  OK: Patch 2 applied via git apply" -ForegroundColor Green
} else {
    Write-Host "  WARN: git apply failed, using code insertion fallback..." -ForegroundColor DarkYellow
    
    # Fallback: Insert logging code after #include "mir-gen.h"
    $loggingCode = @'

/* ======== NUM EXPERIMENT LOGGING ======== */

static FILE *experiment_log = NULL;

void mir_experiment_init_logging(const char *filename) {
  experiment_log = fopen(filename, "w");
  if (experiment_log) {
    fprintf(experiment_log,
      "func_name,shadow_price,ir_count,inlined,threshold_baseline,threshold_adjusted\n");
  }
}

void mir_experiment_close_logging(void) {
  if (experiment_log) {
    fclose(experiment_log);
    experiment_log = NULL;
  }
}

static void log_inlining_decision(const char *func_name, double shadow_price,
                                   int ir_count, int inlined,
                                   int threshold_baseline, int threshold_adjusted) {
  if (experiment_log) {
    fprintf(experiment_log, "%s,%.2f,%d,%d,%d,%d\n",
      func_name, shadow_price, ir_count, inlined,
      threshold_baseline, threshold_adjusted);
    fflush(experiment_log);
  }
}

int should_inline_with_num(const char *func_name, int ir_count,
                            double shadow_price) {
  int baseline_threshold = 50;
  double lambda_scale = shadow_price / 100.0;
  if (lambda_scale < 0.1) lambda_scale = 0.1;
  if (lambda_scale > 5.0) lambda_scale = 5.0;
  int adjusted_threshold = (int)(baseline_threshold * lambda_scale);
  if (adjusted_threshold < 10) adjusted_threshold = 10;
  if (adjusted_threshold > 500) adjusted_threshold = 500;
  int inlined = (ir_count < adjusted_threshold) ? 1 : 0;
  log_inlining_decision(func_name, shadow_price, ir_count, inlined,
                         baseline_threshold, adjusted_threshold);
  return inlined;
}
/* ======== END NUM EXPERIMENT LOGGING ======== */
'@

    $lines = Get-Content $mirGenC
    $newLines = @()
    $inserted = $false
    foreach ($line in $lines) {
        $newLines += $line
        if ((-not $inserted) -and ($line -match '#include\s+"mir-gen\.h"')) {
            $newLines += $loggingCode
            $inserted = $true
        }
    }
    
    if ($inserted) {
        Set-Content -Path $mirGenC -Value ($newLines -join "`n") -NoNewline
        Write-Host "  OK: Patch 2 applied via code insertion" -ForegroundColor Green
    } else {
        Write-Host "  ERROR: Could not find '#include `"mir-gen.h`"' in mir-gen.c" -ForegroundColor Red
        Write-Host "  Searching for alternative insertion points..." -ForegroundColor DarkYellow
        
        # Try inserting after the last #include
        $lastIncludeIdx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^#include\s') {
                $lastIncludeIdx = $i
            }
        }
        
        if ($lastIncludeIdx -ge 0) {
            $newLines = @()
            for ($i = 0; $i -lt $lines.Count; $i++) {
                $newLines += $lines[$i]
                if ($i -eq $lastIncludeIdx) {
                    $newLines += $loggingCode
                }
            }
            Set-Content -Path $mirGenC -Value ($newLines -join "`n") -NoNewline
            Write-Host "  OK: Patch 2 applied after last #include (line $($lastIncludeIdx + 1))" -ForegroundColor Green
        } else {
            Write-Host "  ERROR: No #include directives found in mir-gen.c" -ForegroundColor Red
            exit 1
        }
    }
}

# Verify patch 2
$verify2 = Select-String -Path $mirGenC -Pattern 'mir_experiment_init_logging' -SimpleMatch
if (-not $verify2) {
    Write-Host "  ERROR: Logging functions not found in mir-gen.c after patching" -ForegroundColor Red
    exit 1
}
Write-Host "  Verified: Logging functions present in mir-gen.c" -ForegroundColor DarkGreen

# ---- Phase 4: Compile ----
Write-Host ""
Write-Host "[4/4] Compiling test harness..." -ForegroundColor Yellow

Set-Location $ProjectRoot

# Ensure results directory exists
if (-not (Test-Path $ResultsDir)) {
    New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null
}

$SrcFile = Join-Path $SrcDir 'test-num-experiment.c'
$MirC    = Join-Path $MirDir 'mir.c'
$MirGenC = Join-Path $MirDir 'mir-gen.c'

$gccArgs = @(
    '-O2',
    "-I$MirDir",
    $SrcFile,
    $MirC,
    $MirGenC,
    '-o', $OutputExe,
    '-lm'
)

Write-Host "  Command: gcc $($gccArgs -join ' ')" -ForegroundColor DarkGray

gcc @gccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  ERROR: Compilation failed (exit code $LASTEXITCODE)" -ForegroundColor Red
    Write-Host "  Check that gcc is in PATH and MIR source is valid." -ForegroundColor Red
    Write-Host ""
    Write-Host "  Troubleshooting:" -ForegroundColor Yellow
    Write-Host "    1. Verify gcc: gcc --version" -ForegroundColor DarkGray
    Write-Host "    2. Check mir.h patch: grep shadow_price $mirH" -ForegroundColor DarkGray
    Write-Host "    3. Check mir-gen.c patch: grep mir_experiment_init_logging $mirGenC" -ForegroundColor DarkGray
    exit 1
}

Write-Host "  OK: Compiled $OutputExe" -ForegroundColor Green

# ---- Summary ----
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Build Complete" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Executable: $OutputExe"
Write-Host "  MIR source: $MirDir"
Write-Host "  Results dir: $ResultsDir"
Write-Host ""
Write-Host "  Next: .\scripts\run-conditions.ps1" -ForegroundColor Yellow
Write-Host ""
