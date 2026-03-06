# MIR NUM Validation Experiment

Empirical validation of the compiler-as-NUM hypothesis using MIR (Medium Internal Representation) JIT compiler.

## Preprint & Publications

**Empirical Validation Paper (Preprint):**
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.18828679.svg)](https://doi.org/10.5281/zenodo.18828679)

Bilar, D. Y. (2026). Empirical Validation of the Compiler-as-NUM Hypothesis: A Shadow-Price Experiment with MIR. Zenodo. https://doi.org/10.5281/zenodo.18828679

**Companion Theory Paper:**
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.18715390.svg)](https://doi.org/10.5281/zenodo.18715390)

Bilar, D. Y. (2025). The Compiler as a Decomposed Optimization System. Zenodo. https://doi.org/10.5281/zenodo.18715390

**Experimental Dataset:**
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.18714810.svg)](https://doi.org/10.5281/zenodo.18714810)

Bilar, D. Y. (2026). Empirical Validation of Compiler-as-NUM Hypothesis: MIR JIT Experiment Dataset. Zenodo. https://doi.org/10.5281/zenodo.18714810

## Repository Structure
```
mir-num-experiment/
├── patches/          # MIR instrumentation patches
│   ├── 01-mir-h-shadow-price.patch
│   └── 02-mir-gen-c-logging.patch
├── src/              # Test harness source
│   └── test-num-experiment.c
├── scripts/          # Analysis and build scripts
│   ├── requirements.txt
│   └── *.ps1 (build scripts)
├── phase-2-poc/      # Phase 2 proof-of-concept
│   ├── poc.c         # Context collapse / DCE validation
│   ├── num_experiment.c   # 8-function NUM crucible
│   └── README.md
├── results/          # Raw experimental results
│   ├── baseline_decisions.csv
│   ├── uniform_decisions.csv
│   ├── skewed_decisions.csv
│   └── perturbed_decisions.csv
├── docs/             # Documentation
│   ├── EXPERIMENT.md
│   └── REPRODUCTION.md
└── README.md
```

## Quick Start

### Prerequisites
- MIR compiler (https://github.com/vnmakarov/mir)
- Python 3.8+
- GCC or Clang (on Windows: MSVC or MinGW)

### Reproduction

1. **Clone MIR and apply patches:**
```bash
git clone https://github.com/vnmakarov/mir.git
cd mir
git apply ../patches/01-mir-h-shadow-price.patch
git apply ../patches/02-mir-gen-c-logging.patch
make
```

2. **Build test harness:**
```bash
gcc -O2 -I./mir src/test-num-experiment.c mir.c mir-gen.c -o test-num-experiment -lm
```

3. **Run experiment (requires condition argument: baseline | uniform | skewed | perturbed):**
```bash
./test-num-experiment baseline
./test-num-experiment uniform
./test-num-experiment skewed
./test-num-experiment perturbed
```

4. **Analyze results:**
```bash
pip install -r src/requirements.txt
python src/analyze-num.py results/*.csv
```

See docs/REPRODUCTION.md for detailed instructions.

## Data Description

Each CSV contains 750 rows (50 functions × 5 sizes × 3 optimization levels) with columns:
- **func_name**: Synthetic function identifier
- **shadow_price**: Dual variable λ (execution frequency proxy)
- **ir_count**: Function size in IR instructions
- **opt_level**: Optimization level (0, 1, 2)
- **inlined**: Binary inlining decision (0/1)
- **threshold_baseline**: Base threshold for optimization level
- **threshold_adjusted**: Shadow-price-adjusted threshold

## Statistical Results

- **Spearman ρ**: 0.71 (95% CI: [0.67, 0.74], p < 10⁻¹¹⁴)
- **Cohen's d**: 2.00 (hot vs cold functions)
- **Perturbation robustness**: 98.4% agreement (738/750 matched decisions)

See publication for full analysis and stratified results.

## Experimental Conditions

1. **Baseline** (λ = 100): Neutral shadow price
2. **Uniform** (λ = 100): Replication control
3. **Skewed** (λ ∈ {10, 1000}): 100× price differential
4. **Perturbed** (skewed + noise): Robustness test

## License

MIT License - See LICENSE file

## Citation
```bibtex
@article{bilar2026mir,
  title={Empirical Validation of the Compiler-as-NUM Hypothesis: A Shadow-Price Experiment with MIR},
  author={Bilar, Daniyel Yaacov},
  journal={Zenodo},
  year={2026},
  doi={10.5281/zenodo.18828679}
}
```

## Contact

Daniyel Yaacov Bilar  
Chokmah LLC  
Vermont 05055
