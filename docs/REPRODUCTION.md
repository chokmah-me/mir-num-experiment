# Detailed Reproduction Guide

## System Requirements

- **OS**: Linux, macOS, or Windows with WSL2/MinGW
- **Compiler**: GCC 9+ or Clang 11+
- **Python**: 3.8 or higher
- **Memory**: 2GB RAM minimum
- **Time**: ~60 seconds total execution

## Step-by-Step Instructions

### 1. Preparation
```bash
cd mir-num-experiment
```

### 2. Clone MIR and Apply Patches

```bash
# Clone MIR repository
git clone https://github.com/vnmakarov/mir.git mir-baseline
cd mir-baseline

# Apply instrumentation patches
git apply ../patches/01-mir-h-shadow-price.patch
git apply ../patches/02-mir-gen-c-logging.patch

# Build MIR
make

# Return to project root
cd ..
```

**Note**: The patches add:
- `shadow_price` field to MIR's `MIR_insn_t` structure
- Logging infrastructure for decision tracking

### 3. Compile Test Harness

From the project root:

```bash
gcc -O2 -I./mir-baseline ./src/test-num-experiment.c \
    ./mir-baseline/mir.c ./mir-baseline/mir-gen.c \
    -o test-num-experiment -lm
```

### 4. Run All Experimental Conditions

The test harness requires a condition argument. Run each condition separately:

```bash
./test-num-experiment baseline    # λ = 100 (neutral control)
./test-num-experiment uniform     # λ = 100 (replication)
./test-num-experiment skewed      # λ ∈ {10, 1000} (main test)
./test-num-experiment perturbed   # λ ∈ {5, 1200} + noise (robustness)
```

Each run generates a CSV file in the `results/` directory:
- `baseline_decisions.csv`
- `uniform_decisions.csv`
- `skewed_decisions.csv`
- `perturbed_decisions.csv`

Each CSV contains 750 rows (50 functions × 5 sizes × 3 optimization levels).

### 5. Install Python Dependencies

```bash
pip install -r src/requirements.txt
```

Required packages:
- numpy
- scipy
- pandas
- matplotlib

### 6. Statistical Analysis

```bash
python src/analyze-num.py results/*.csv
```

This produces:
- Spearman correlation (ρ) with 95% CI
- Cohen's d (hot vs cold effect size)
- Decision agreement rates
- Stratified analysis by optimization level and function size
- Summary visualization

## Expected Output

### CSV Format (one row per function decision)

| Column | Type | Example | Description |
|--------|------|---------|-------------|
| func_name | str | `func_0` | Synthetic function ID |
| shadow_price | float | 1000.0 | Execution frequency proxy (λ) |
| ir_count | int | 100 | Function size in IR instructions |
| opt_level | int | 1 | Optimization level (0, 1, or 2) |
| inlined | int | 1 | Inlining decision (0=no, 1=yes) |
| threshold_baseline | int | 50 | Base threshold for opt level |
| threshold_adjusted | float | 500.0 | Shadow-price-adjusted threshold |

### Statistical Results (Console Output)

Expected key metrics:

```
Spearman ρ (pooled):           0.707
95% Confidence Interval:       [0.67, 0.74]
p-value:                       < 1e-114

Cohen's d (Hot vs Cold):       1.997 (≈2.00)
  Hot inlining rate:           66.7% (250/375)
  Cold inlining rate:          0.0% (0/375)

Decision Agreement:            98.4% (738/750)
  Baseline vs Uniform:         100.0% (750/750)
  Skewed vs Perturbed:         98.4% (738/750)
  Discordant pairs:            12 (all: skewed=no, perturbed=yes)
```

### Visualization

`num-analysis.png` contains 6 subplots:
- Top-left: Scatterplot (shadow price vs IR count, colored by decision)
- Top-middle: Bar chart (hot vs cold inlining rates)
- Top-right: Inlining rates across all 4 conditions
- Bottom-left: Spearman ρ by optimization level (O0, O1, O2)
- Bottom-middle: Hot vs cold inlining rates by function size
- Bottom-right: Heatmap of inlining decisions

## Troubleshooting

### Issue: MIR build fails

**Symptom**: `make` command fails with compilation errors

**Solutions**:
1. Verify GCC version: `gcc --version` (must be 9+)
2. Install build essentials: `sudo apt-get install build-essential` (Linux)
3. For Windows, use MinGW-w64 from MSYS2

### Issue: Patch application fails

**Symptom**: `git apply` returns error or `patch: command not found`

**Solutions**:
1. Ensure git is installed and in PATH
2. Verify patch files exist in `patches/` directory
3. Check git status with `git status` to see if patches were partially applied
4. If needed, manually apply patches by editing `mir.h` and `mir-gen.c` according to patch content

### Issue: test-num-experiment crashes or fails to run

**Symptom**: Segmentation fault or runtime error

**Solutions**:
1. Verify executable built: `ls -l test-num-experiment`
2. Verify MIR build completed: `ls -l mir-baseline/mir*.o`
3. Re-run compilation with verbose output: `gcc ... -v` to check linking
4. Ensure all arguments are provided: `./test-num-experiment baseline` (not just `./test-num-experiment`)

### Issue: Python script fails with "No such file or directory"

**Symptom**: `FileNotFoundError: results/baseline_decisions.csv`

**Solutions**:
1. Verify CSVs were generated: `ls results/*.csv`
2. Re-run all experimental conditions (step 4 above)
3. Check current working directory: `pwd` (should be project root)
4. Run script with absolute path if needed: `python $(pwd)/src/analyze-num.py $(pwd)/results/*.csv`

### Issue: Columns missing or misnamed in CSV

**Symptom**: Python script error about missing columns

**Solutions**:
1. Verify CSV structure: `head -1 results/baseline_decisions.csv`
2. Regenerate CSV by re-running condition: `./test-num-experiment baseline`
3. Check test harness was built with latest patch versions

## Verification Procedure

To confirm results match publication:

1. **Run all conditions** (Step 4) — should complete in ~60 seconds total
2. **Check row counts**: Each CSV should have exactly 750 rows
3. **Run analysis** (Step 6) — key metrics should match table below

### Reference Results (Must Match)

| Metric | Expected | Tolerance |
|--------|----------|-----------|
| Spearman ρ | 0.707 | ±0.005 |
| 95% CI (ρ) | [0.67, 0.74] | ±0.01 |
| p-value (ρ) | < 1e-100 | any < 0.001 |
| Cohen's d | 1.997 | ±0.01 |
| Hot rate | 66.7% | exact |
| Cold rate | 0.0% | exact |
| Agreement | 98.4% | ±0.1% |
| O0 ρ | 0.50 | ±0.05 |
| O1 ρ | 0.82 | ±0.05 |
| O2 ρ | 0.82 | ±0.05 |

Deviations may indicate:
- Different random seed (verify `random_seed=42` in test harness)
- Different MIR version
- Different compiler optimizations
- Incomplete patch application

## Additional Resources

- **GitHub Repository**: https://github.com/chokmah-me/mir-num-experiment
- **Zenodo Archive**: https://doi.org/10.5281/zenodo.18714810
- **Empirical Paper**: https://doi.org/10.5281/zenodo.18828679
- **Theory Companion**: https://doi.org/10.5281/zenodo.18715390

## Contact

For questions about reproduction:
- Check GitHub Issues: https://github.com/chokmah-me/mir-num-experiment/issues
- Author: Daniyel Yaacov Bilar (ORCID: 0000-0002-9040-6914)
