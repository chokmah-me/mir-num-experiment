# MIR NUM Experiment Repository Structure

**Last Updated:** March 5, 2026  
**Publication:** https://doi.org/10.5281/zenodo.18828679

## Directory Overview

```
mir-num-experiment/
├── patches/              # MIR compiler instrumentation patches
│   ├── 01-mir-h-shadow-price.patch     # Header modifications for shadow price tracking
│   └── 02-mir-gen-c-logging.patch      # Code generation logging extensions
│
├── src/                  # Primary test harness and utilities
│   └── test-num-experiment.c           # Synthetic function generation & decision logging
│
├── scripts/              # Analysis pipeline
│   ├── analyze-num.py    # Comprehensive statistical analysis (stratified)
│   ├── requirements.txt   # Python dependencies
│   └── *.ps1            # Windows build automation scripts
│
├── phase-2-poc/          # Phase 2: Proof-of-Concept Validation
│   ├── poc.c            # Context Collapse / Dead Code Elimination validation
│   │                     # Demonstrates 7.46x speedup via inlining + DCE
│   ├── num_experiment.c # Full dual decomposition protocol (8-function benchmark)
│   │                     # 5-condition experiment: baseline, blind-all, random, shadow-price, inverted-price
│   └── README.md        # PoC-specific documentation
│
├── data/                 # Raw experimental CSV results
│   ├── baseline_decisions.csv      # Neutral control (λ = 100)
│   ├── uniform_decisions.csv       # Replication control
│   ├── skewed_decisions.csv        # Main test (λ ∈ {10, 1000})
│   └── perturbed_decisions.csv     # Robustness test (skewed + noise)
│
├── docs/                 # Extended documentation
│   ├── EXPERIMENT.md     # Detailed experimental methodology
│   └── REPRODUCTION.md   # Step-by-step reproduction guide
│
├── README.md             # Main project overview
├── CITATION.cff          # Citation metadata (updated 2026-03-05)
└── LICENSE               # MIT License
```

## Key Files by Purpose

### For Understanding the Research
- **README.md** — Publication links, quick-start guide
- **phase-2-poc/README.md** — Physical foundation & empirical results
- **docs/EXPERIMENT.md** — Methodology details

### For Reproduction
- **patches/*** — Apply to MIR source before building
- **src/test-num-experiment.c** — Generate synthetic benchmarks
- **scripts/analyze-num.py** — Analyze CSV results
- **docs/REPRODUCTION.md** — Step-by-step instructions

### For Data & Results
- **data/*.csv** — Raw 750-row decision logs per condition
- **scripts/results/num-analysis.png** — Stratified analysis visualization

## Version History

| Date | Version | Notes |
|------|---------|-------|
| 2026-02-20 | v1.0.0 | Initial release (DOI: 10.5281/zenodo.18714810) |
| 2026-03-05 | v1.0.0 | Publication update (DOI: 10.5281/zenodo.18828679) |

## Citation

```bibtex
@article{bilar2026mir,
  title={Empirical Validation of the Compiler-as-NUM Hypothesis: A Shadow-Price Experiment with MIR},
  author={Bilar, Daniyel Yaacov},
  journal={Journal of Systems Architecture},
  year={2026},
  note={Preprint},
  doi={10.5281/zenodo.18828679}
}
```
