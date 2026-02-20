#!/usr/bin/env python3
"""
analyze-num.py (v2 — stratified)
MIR NUM Experiment: Statistical Analysis

Core tests (unchanged):
  1. Spearman correlation (skewed condition, all data)
  2. Cohen's d effect size (hot vs cold)
  3. McNemar test (skewed vs perturbed agreement)

New stratified analyses:
  4. Spearman rho per optimization level (0, 1, 2)
  5. Inlining rate by function size bucket
  6. Interaction: size × shadow_price heatmap

Outputs:
  - Console: all test results + stratified tables + verdict
  - results/num-analysis.png (3×3 grid)

Generated: 2026-02-19, updated with stratification
"""

import os
import sys
import numpy as np
import pandas as pd
from scipy import stats
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

# ---- Configuration ----
RESULTS_DIR = os.path.join('.', 'results')
CONDITIONS = ['baseline', 'uniform', 'skewed', 'perturbed']

SPEARMAN_RHO_THRESHOLD = 0.5
COHENS_D_THRESHOLD = 0.5
MCNEMAR_P_THRESHOLD = 0.05
ALPHA = 0.05

SIZE_LABELS = {10: 'Tiny (10)', 50: 'Small (50)', 100: 'Medium (100)',
               200: 'Large (200)', 500: 'Huge (500)'}
OPT_LABELS = {0: 'O0 (thresh=20)', 1: 'O1 (thresh=50)', 2: 'O2 (thresh=100)'}


# ---- Helper Functions ----

def cohens_d(group1, group2):
    n1, n2 = len(group1), len(group2)
    if n1 < 2 or n2 < 2:
        return 0.0
    var1 = np.var(group1, ddof=1)
    var2 = np.var(group2, ddof=1)
    pooled_std = np.sqrt(((n1 - 1) * var1 + (n2 - 1) * var2) / (n1 + n2 - 2))
    if pooled_std == 0:
        return 0.0
    return (np.mean(group1) - np.mean(group2)) / pooled_std


def mcnemar_test(decisions_a, decisions_b):
    both_yes = np.sum((decisions_a == 1) & (decisions_b == 1))
    both_no = np.sum((decisions_a == 0) & (decisions_b == 0))
    a_yes_b_no = np.sum((decisions_a == 1) & (decisions_b == 0))
    a_no_b_yes = np.sum((decisions_a == 0) & (decisions_b == 1))
    b = a_yes_b_no
    c = a_no_b_yes
    if (b + c) == 0:
        return 0.0, 1.0, [[both_yes, b], [c, both_no]]
    stat = (abs(b - c) - 1) ** 2 / (b + c)
    p_value = 1 - stats.chi2.cdf(stat, df=1)
    return stat, p_value, [[both_yes, b], [c, both_no]]


def spearman_with_ci(x, y, confidence=0.95):
    """Spearman rho with Fisher z-transform confidence interval."""
    rho, p = stats.spearmanr(x, y)
    n = len(x)
    if n < 4:
        return rho, p, (np.nan, np.nan)
    # Fisher z-transform for CI
    z = np.arctanh(rho)
    se = 1.0 / np.sqrt(n - 3)
    z_crit = stats.norm.ppf(1 - (1 - confidence) / 2)
    ci_low = np.tanh(z - z_crit * se)
    ci_high = np.tanh(z + z_crit * se)
    return rho, p, (ci_low, ci_high)


# ---- Load Data ----

def load_data():
    data = {}
    print("Loading CSV data from", RESULTS_DIR)
    for cond in CONDITIONS:
        path = os.path.join(RESULTS_DIR, f'{cond}_decisions.csv')
        try:
            df = pd.read_csv(path)
            data[cond] = df
            n_opts = df['opt_level'].nunique() if 'opt_level' in df.columns else 1
            print(f"  Loaded {path}: {len(df)} rows, {n_opts} opt level(s)")
        except FileNotFoundError:
            print(f"  ERROR: {path} not found")
            sys.exit(1)
        except Exception as e:
            print(f"  ERROR reading {path}: {e}")
            sys.exit(1)
    print()
    return data


# ============================================================
# CORE TESTS (1-3)
# ============================================================

def test_spearman(data):
    print("=" * 60)
    print("Test 1: Spearman Correlation (Skewed Condition, All Levels)")
    print("=" * 60)

    skewed = data['skewed']
    rho, p_val, ci = spearman_with_ci(skewed['shadow_price'], skewed['inlined'])
    passed = (abs(rho) > SPEARMAN_RHO_THRESHOLD) and (p_val < ALPHA)

    print(f"  rho (correlation): {rho:.4f}")
    print(f"  95% CI:            [{ci[0]:.4f}, {ci[1]:.4f}]")
    print(f"  p-value:           {p_val:.2e}")
    print(f"  n:                 {len(skewed)}")
    print(f"  Threshold:         |rho| > {SPEARMAN_RHO_THRESHOLD}, p < {ALPHA}")
    sym = "\u2713" if passed else "\u2717"
    print(f"  Result:            {sym} {'PASS' if passed else 'FAIL'}")
    print()

    return passed, rho, p_val, ci


def test_cohens_d(data):
    print("=" * 60)
    print("Test 2: Cohen's d (Hot vs Cold Shadow Price)")
    print("=" * 60)

    skewed = data['skewed']
    median_price = skewed['shadow_price'].median()
    hot = skewed[skewed['shadow_price'] > median_price]['inlined']
    cold = skewed[skewed['shadow_price'] <= median_price]['inlined']
    d = cohens_d(hot.values, cold.values)
    passed = abs(d) > COHENS_D_THRESHOLD
    hot_rate = hot.mean() * 100
    cold_rate = cold.mean() * 100

    print(f"  Hot  (price > {median_price:.1f}) inlining rate: {hot_rate:.1f}% (n={len(hot)})")
    print(f"  Cold (price <= {median_price:.1f}) inlining rate: {cold_rate:.1f}% (n={len(cold)})")
    print(f"  Cohen's d:       {d:.4f}")
    print(f"  Threshold:       |d| > {COHENS_D_THRESHOLD}")
    sym = "\u2713" if passed else "\u2717"
    print(f"  Result:           {sym} {'PASS' if passed else 'FAIL'}")
    print()

    return passed, d, hot_rate, cold_rate


def test_mcnemar(data):
    print("=" * 60)
    print("Test 3: McNemar Test (Skewed vs Perturbed)")
    print("=" * 60)

    skewed = data['skewed']
    perturbed = data['perturbed']
    merged = pd.merge(skewed[['func_name', 'inlined']],
                       perturbed[['func_name', 'inlined']],
                       on='func_name', suffixes=('_skewed', '_perturbed'))

    stat, p_val, table = mcnemar_test(
        merged['inlined_skewed'].values,
        merged['inlined_perturbed'].values
    )

    discordant = table[0][1] + table[1][0]
    concordant = table[0][0] + table[1][1]
    agreement_rate = concordant / len(merged) if len(merged) > 0 else 0.0

    AGREEMENT_THRESHOLD = 0.80
    NEAR_PERFECT_FRAC = 0.05

    if discordant <= len(merged) * NEAR_PERFECT_FRAC:
        passed = True
        pass_reason = "PERFECT AGREEMENT"
    elif agreement_rate >= AGREEMENT_THRESHOLD and p_val >= MCNEMAR_P_THRESHOLD:
        passed = True
        pass_reason = "HIGH AGREEMENT (no asymmetric shift)"
    elif agreement_rate >= AGREEMENT_THRESHOLD and p_val < MCNEMAR_P_THRESHOLD:
        passed = False
        pass_reason = "ASYMMETRIC DISAGREEMENT despite high agreement"
    else:
        passed = False
        pass_reason = "LOW AGREEMENT or systematic shift"

    print(f"  Matched pairs:             {len(merged)}")
    print(f"  Both inlined:              {table[0][0]}")
    print(f"  Skewed yes, perturbed no:  {table[0][1]}")
    print(f"  Skewed no, perturbed yes:  {table[1][0]}")
    print(f"  Neither inlined:           {table[1][1]}")
    print(f"  Agreement rate:            {agreement_rate:.1%} ({concordant}/{len(merged)})")
    print(f"  Discordant pairs:          {discordant}")
    print(f"  McNemar statistic:         {stat:.4f}")
    print(f"  McNemar p-value:           {p_val:.2e}")
    print(f"  Assessment:                {pass_reason}")
    sym = "\u2713" if passed else "\u2717"
    print(f"  Result:                    {sym} {'PASS' if passed else 'FAIL'}")
    print()

    return passed, stat, p_val


# ============================================================
# STRATIFIED ANALYSES (4-5)
# ============================================================

def analyze_by_opt_level(data):
    """
    Stratified analysis 4: Spearman rho per optimization level.
    
    Key question for reviewers: Is lambda-sensitivity level-dependent?
    We expect rho to be significant at all levels, but the *pattern*
    of inlining rates should differ (O0 conservative, O2 aggressive).
    """
    print("=" * 60)
    print("Analysis 4: Stratified by Optimization Level (Skewed)")
    print("=" * 60)

    skewed = data['skewed']

    if 'opt_level' not in skewed.columns:
        print("  SKIP: opt_level column not found (v1 data)")
        print()
        return {}

    results = {}
    for opt in sorted(skewed['opt_level'].unique()):
        subset = skewed[skewed['opt_level'] == opt]
        rho, p, ci = spearman_with_ci(subset['shadow_price'], subset['inlined'])
        n = len(subset)
        inline_rate = subset['inlined'].mean() * 100
        label = OPT_LABELS.get(opt, f'O{opt}')

        results[opt] = {
            'rho': rho, 'p': p, 'ci': ci,
            'n': n, 'inline_rate': inline_rate,
            'label': label,
            'threshold': subset['threshold_baseline'].iloc[0] if len(subset) > 0 else 0
        }

        sig = "\u2713" if (abs(rho) > 0.5 and p < 0.05) else "\u2717"
        print(f"  {label}:")
        print(f"    n={n}, inline rate={inline_rate:.1f}%")
        print(f"    rho={rho:.4f}, p={p:.2e}, 95% CI=[{ci[0]:.3f}, {ci[1]:.3f}]  {sig}")

    # Cross-level consistency check
    rhos = [r['rho'] for r in results.values()]
    if len(rhos) >= 2:
        rho_range = max(rhos) - min(rhos)
        print(f"\n  Cross-level rho range: {rho_range:.4f}")
        if rho_range < 0.2:
            print("  Assessment: CONSISTENT across opt levels")
        else:
            print("  Assessment: VARIABLE across opt levels (worth discussing)")
    print()

    return results


def analyze_by_size(data):
    """
    Stratified analysis 5: Inlining rate by function size bucket.
    
    Key question: Is the effect driven entirely by trivially small functions?
    We expect:
      - Tiny functions (10 IR): always inlined regardless of shadow_price
      - Huge functions (500 IR): never inlined regardless of shadow_price
      - Middle sizes (50-200 IR): shadow_price MATTERS — this is where
        the NUM signal should be strongest
    
    This is the most important stratification for the paper because it
    shows the "decision boundary" where shadow price actually changes
    the inlining outcome.
    """
    print("=" * 60)
    print("Analysis 5: Stratified by Function Size (Skewed)")
    print("=" * 60)

    skewed = data['skewed']
    results = {}

    for size in sorted(skewed['ir_count'].unique()):
        subset = skewed[skewed['ir_count'] == size]
        n = len(subset)
        inline_rate = subset['inlined'].mean() * 100

        # Split into hot/cold for this size
        median_p = skewed['shadow_price'].median()
        hot = subset[subset['shadow_price'] > median_p]['inlined']
        cold = subset[subset['shadow_price'] <= median_p]['inlined']
        hot_rate = hot.mean() * 100 if len(hot) > 0 else 0
        cold_rate = cold.mean() * 100 if len(cold) > 0 else 0

        # Spearman within this size (if enough variance)
        if subset['inlined'].nunique() > 1 and subset['shadow_price'].nunique() > 1:
            rho_size, p_size = stats.spearmanr(subset['shadow_price'], subset['inlined'])
        else:
            rho_size, p_size = 0.0, 1.0

        label = SIZE_LABELS.get(size, f'{size} IR')
        results[size] = {
            'n': n, 'inline_rate': inline_rate,
            'hot_rate': hot_rate, 'cold_rate': cold_rate,
            'rho': rho_size, 'p': p_size, 'label': label,
        }

        # Classify this size bucket
        if hot_rate > 95 and cold_rate > 95:
            verdict = "ALWAYS INLINED (floor)"
        elif hot_rate < 5 and cold_rate < 5:
            verdict = "NEVER INLINED (ceiling)"
        elif abs(hot_rate - cold_rate) > 20:
            verdict = "DECISION BOUNDARY <-- shadow_price matters"
        else:
            verdict = "MARGINAL"

        print(f"  {label} (n={n}):")
        print(f"    Overall: {inline_rate:.1f}%  |  Hot: {hot_rate:.1f}%  Cold: {cold_rate:.1f}%")
        print(f"    rho={rho_size:.3f}, p={p_size:.2e}  |  {verdict}")

    # Identify decision boundary sizes
    boundary_sizes = [s for s, r in results.items()
                       if abs(r['hot_rate'] - r['cold_rate']) > 20]
    if boundary_sizes:
        print(f"\n  Decision boundary sizes: {boundary_sizes} IR instructions")
        print("  These are where shadow_price most affects inlining decisions")
    else:
        print("\n  No clear decision boundary found")
    print()

    return results


# ============================================================
# VERDICT
# ============================================================

def print_verdict(results):
    num_passed = sum(results)

    print("=" * 60)
    print("VERDICT")
    print("=" * 60)

    if num_passed == 3:
        print("\n  \u2713 CORROBORATES: All 3 tests pass.")
        print("  The inlining decisions respond strongly to shadow_price signals,")
        print("  with substantial effect size and robustness to perturbation.\n")
        return "CORROBORATES"
    elif num_passed == 2:
        print("\n  \u25D0 PARTIAL: 2/3 tests pass.")
        print("  Mixed evidence for the NUM hypothesis.")
        print("  Effect exists but may be weak or unstable.\n")
        return "PARTIAL"
    else:
        print(f"\n  \u2717 DISPROVES: {num_passed}/3 tests pass.")
        print("  No significant evidence for the NUM hypothesis.")
        print("  Inlining heuristic does not respond to shadow_price signals.\n")
        return "DISPROVES"


# ============================================================
# VISUALIZATION (3x3 grid)
# ============================================================

def create_visualization(data, rho, p_rho, ci_rho, d, hot_rate, cold_rate,
                          verdict, opt_results, size_results):
    """Create 3x3 subplot visualization with stratified analyses."""

    fig, axes = plt.subplots(3, 3, figsize=(18, 16))
    fig.suptitle('MIR NUM Experiment Results (v2 — Stratified)',
                 fontsize=18, fontweight='bold', y=0.98)

    skewed = data['skewed']

    # ---- Row 0: Core Tests ----

    # (0,0) Scatter: shadow_price vs inlined
    ax = axes[0, 0]
    jitter = np.random.uniform(-0.05, 0.05, len(skewed))
    colors = ['#e74c3c' if x == 0 else '#2ecc71' for x in skewed['inlined']]
    ax.scatter(skewed['shadow_price'], skewed['inlined'] + jitter,
               c=colors, alpha=0.4, s=20, edgecolors='none')
    ax.set_xlabel('Shadow Price (\u03BB)')
    ax.set_ylabel('Inlined (0/1)')
    ax.set_title(f'Test 1: Spearman Correlation\n\u03C1={rho:.3f} [{ci_rho[0]:.2f}, {ci_rho[1]:.2f}], p={p_rho:.1e}')
    ax.set_yticks([0, 1])
    ax.set_yticklabels(['No', 'Yes'])

    # (0,1) Hot vs Cold bar
    ax = axes[0, 1]
    bars = ax.bar(['Hot\n(high \u03BB)', 'Cold\n(low \u03BB)'],
                   [hot_rate, cold_rate],
                   color=['#e74c3c', '#3498db'], alpha=0.8, width=0.5)
    ax.set_ylabel('Inlining Rate (%)')
    ax.set_title(f"Test 2: Hot vs Cold\nCohen's d = {d:.3f}")
    ax.set_ylim(0, 105)
    for bar, rate in zip(bars, [hot_rate, cold_rate]):
        ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height() + 2,
                f'{rate:.1f}%', ha='center', va='bottom', fontweight='bold')

    # (0,2) All conditions bar
    ax = axes[0, 2]
    rates = [data[c]['inlined'].mean() * 100 for c in CONDITIONS]
    colors_bar = ['#95a5a6', '#95a5a6', '#e67e22', '#9b59b6']
    bars = ax.bar(CONDITIONS, rates, color=colors_bar, alpha=0.8, width=0.6)
    ax.set_ylabel('Mean Inlining Rate (%)')
    ax.set_title('Inlining Rate by Condition')
    ax.set_ylim(0, 105)
    for bar, rate in zip(bars, rates):
        ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height() + 2,
                f'{rate:.1f}%', ha='center', va='bottom', fontsize=9)

    # ---- Row 1: Opt-Level Stratification ----

    if opt_results:
        # (1,0) Rho by opt level
        ax = axes[1, 0]
        opt_levels = sorted(opt_results.keys())
        rhos = [opt_results[o]['rho'] for o in opt_levels]
        ci_lows = [opt_results[o]['ci'][0] for o in opt_levels]
        ci_highs = [opt_results[o]['ci'][1] for o in opt_levels]
        labels = [f"O{o}" for o in opt_levels]
        yerr_low = [r - cl for r, cl in zip(rhos, ci_lows)]
        yerr_high = [ch - r for r, ch in zip(rhos, ci_highs)]
        bars = ax.bar(labels, rhos, color='#2980b9', alpha=0.8, width=0.5,
                       yerr=[yerr_low, yerr_high], capsize=8, error_kw={'linewidth': 2})
        ax.axhline(y=0.5, color='red', linestyle='--', alpha=0.5, label='\u03C1=0.5 threshold')
        ax.set_ylabel('Spearman \u03C1')
        ax.set_title('Analysis 4: \u03C1 by Opt Level\n(Is \u03BB-sensitivity level-dependent?)')
        ax.set_ylim(0, 1.05)
        ax.legend(fontsize=9)

        # (1,1) Inlining rate by opt level (hot vs cold)
        ax = axes[1, 1]
        x = np.arange(len(opt_levels))
        width = 0.35
        hot_rates_opt = []
        cold_rates_opt = []
        for o in opt_levels:
            subset = skewed[skewed['opt_level'] == o]
            med = skewed['shadow_price'].median()
            hr = subset[subset['shadow_price'] > med]['inlined'].mean() * 100
            cr = subset[subset['shadow_price'] <= med]['inlined'].mean() * 100
            hot_rates_opt.append(hr)
            cold_rates_opt.append(cr)
        ax.bar(x - width/2, hot_rates_opt, width, label='Hot (\u03BB high)',
               color='#e74c3c', alpha=0.8)
        ax.bar(x + width/2, cold_rates_opt, width, label='Cold (\u03BB low)',
               color='#3498db', alpha=0.8)
        ax.set_xticks(x)
        ax.set_xticklabels([f"O{o}\n(t={opt_results[o]['threshold']})" for o in opt_levels])
        ax.set_ylabel('Inlining Rate (%)')
        ax.set_title('Hot vs Cold by Opt Level')
        ax.set_ylim(0, 105)
        ax.legend(fontsize=9)
    else:
        axes[1, 0].text(0.5, 0.5, 'No opt_level data\n(v1 CSV format)',
                         transform=axes[1, 0].transAxes, ha='center', va='center')
        axes[1, 0].set_title('Analysis 4: Opt Level Stratification')
        axes[1, 1].axis('off')

    # (1,2) Opt level summary table
    ax = axes[1, 2]
    ax.axis('off')
    if opt_results:
        table_data = []
        for o in sorted(opt_results.keys()):
            r = opt_results[o]
            sig = "\u2713" if abs(r['rho']) > 0.5 else "\u2717"
            table_data.append([f"O{o}", f"{r['threshold']}",
                                f"{r['inline_rate']:.1f}%",
                                f"{r['rho']:.3f}", sig])
        table = ax.table(cellText=table_data,
                          colLabels=['Level', 'Thresh', 'Rate', '\u03C1', 'Sig'],
                          loc='center', cellLoc='center')
        table.auto_set_font_size(False)
        table.set_fontsize(11)
        table.scale(1, 1.8)
        ax.set_title('Opt Level Summary', fontsize=12, pad=20)

    # ---- Row 2: Size Stratification ----

    if size_results:
        # (2,0) Inlining rate by size (hot vs cold)
        ax = axes[2, 0]
        sz = sorted(size_results.keys())
        hot_by_size = [size_results[s]['hot_rate'] for s in sz]
        cold_by_size = [size_results[s]['cold_rate'] for s in sz]
        x = np.arange(len(sz))
        width = 0.35
        ax.bar(x - width/2, hot_by_size, width, label='Hot (\u03BB high)',
               color='#e74c3c', alpha=0.8)
        ax.bar(x + width/2, cold_by_size, width, label='Cold (\u03BB low)',
               color='#3498db', alpha=0.8)
        ax.set_xticks(x)
        ax.set_xticklabels([str(s) for s in sz])
        ax.set_xlabel('Function Size (IR instructions)')
        ax.set_ylabel('Inlining Rate (%)')
        ax.set_title('Analysis 5: Hot vs Cold by Size\n(Where does \u03BB matter?)')
        ax.set_ylim(0, 105)
        ax.legend(fontsize=9)

        # Annotate decision boundary
        for i, s in enumerate(sz):
            diff = abs(size_results[s]['hot_rate'] - size_results[s]['cold_rate'])
            if diff > 20:
                ax.annotate('\u2190 decision\n   boundary',
                            xy=(i, max(hot_by_size[i], cold_by_size[i]) + 3),
                            fontsize=8, color='#e67e22', fontweight='bold',
                            ha='center')

        # (2,1) Heatmap: size x shadow_price -> inlining rate
        ax = axes[2, 1]
        if 'opt_level' in skewed.columns:
            # Use opt_level=1 (moderate) for the heatmap
            hm_data = skewed[skewed['opt_level'] == 1] if 1 in skewed['opt_level'].values else skewed
        else:
            hm_data = skewed
        pivot = hm_data.pivot_table(values='inlined', index='ir_count',
                                     columns='shadow_price', aggfunc='mean')
        # Sort index for display
        pivot = pivot.sort_index(ascending=True)
        cmap = LinearSegmentedColormap.from_list('rg', ['#3498db', '#f1c40f', '#e74c3c'])
        im = ax.imshow(pivot.values, aspect='auto', cmap=cmap, vmin=0, vmax=1)
        ax.set_xticks(range(len(pivot.columns)))
        ax.set_xticklabels([f'{c:.0f}' for c in pivot.columns], fontsize=8)
        ax.set_yticks(range(len(pivot.index)))
        ax.set_yticklabels([str(s) for s in pivot.index])
        ax.set_xlabel('Shadow Price (\u03BB)')
        ax.set_ylabel('IR Count')
        ax.set_title('Inlining Heatmap (O1)\nRed=inlined, Blue=not')
        plt.colorbar(im, ax=ax, shrink=0.8, label='P(inlined)')

        # (2,2) Verdict and full summary
        ax = axes[2, 2]
        ax.axis('off')
        verdict_color = {'CORROBORATES': '#27ae60', 'PARTIAL': '#f39c12',
                          'DISPROVES': '#e74c3c'}
        color = verdict_color.get(verdict, '#333333')
        ax.text(0.5, 0.8, f'VERDICT: {verdict}',
                transform=ax.transAxes, fontsize=22, fontweight='bold',
                ha='center', va='center', color=color)

        lines = [
            f'Spearman \u03C1 = {rho:.3f}  [CI: {ci_rho[0]:.2f}, {ci_rho[1]:.2f}]',
            f"Cohen's d = {d:.3f}",
            f'Hot = {hot_rate:.1f}%  Cold = {cold_rate:.1f}%',
            '',
            'Opt-level: \u03C1 consistent' if opt_results and
            (max(r['rho'] for r in opt_results.values()) -
             min(r['rho'] for r in opt_results.values()) < 0.2)
            else 'Opt-level: \u03C1 varies by level',
            f'Decision boundary: {[s for s in sorted(size_results.keys()) if abs(size_results[s]["hot_rate"] - size_results[s]["cold_rate"]) > 20]} IR'
            if size_results else '',
        ]
        ax.text(0.5, 0.35, '\n'.join(lines),
                transform=ax.transAxes, fontsize=10, ha='center', va='center',
                family='monospace', linespacing=1.6)
    else:
        for i in range(3):
            axes[2, i].text(0.5, 0.5, 'No size stratification data',
                             transform=axes[2, i].transAxes, ha='center')

    plt.tight_layout(rect=[0, 0, 1, 0.96])
    output_path = os.path.join(RESULTS_DIR, 'num-analysis.png')
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    print(f"Visualization saved to: {output_path}")
    plt.close()


# ============================================================
# MAIN
# ============================================================

def main():
    print("=" * 60)
    print("MIR NUM Experiment Analysis (v2 - Stratified)")
    print("=" * 60)
    print()

    data = load_data()

    # Core tests
    pass1, rho, p_rho, ci_rho = test_spearman(data)
    pass2, d, hot_rate, cold_rate = test_cohens_d(data)
    pass3, mcnemar_stat, mcnemar_p = test_mcnemar(data)

    # Stratified analyses
    opt_results = analyze_by_opt_level(data)
    size_results = analyze_by_size(data)

    # Verdict
    verdict = print_verdict([pass1, pass2, pass3])

    # Visualization
    try:
        create_visualization(data, rho, p_rho, ci_rho, d, hot_rate, cold_rate,
                              verdict, opt_results, size_results)
    except Exception as e:
        print(f"WARNING: Could not create visualization: {e}")
        import traceback
        traceback.print_exc()

    return 0 if verdict in ("CORROBORATES", "PARTIAL") else 1


if __name__ == '__main__':
    sys.exit(main())
