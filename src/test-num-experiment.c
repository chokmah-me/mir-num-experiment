/*
 * test-num-experiment.c
 * MIR NUM Experiment: Test Harness (v2 — with opt-level stratification)
 *
 * Generates synthetic MIR IR functions with varying sizes and shadow prices,
 * then invokes the NUM-modified inlining decision logic and logs results to CSV.
 *
 * v2 changes:
 *   - Runs each condition at 3 optimization levels (0, 1, 2)
 *   - Each opt level has a different baseline inlining threshold:
 *       Level 0: threshold=20  (conservative, inline only tiny functions)
 *       Level 1: threshold=50  (moderate, standard)
 *       Level 2: threshold=100 (aggressive, inline larger functions)
 *   - CSV now includes opt_level column for stratified analysis
 *   - Total: 50 funcs x 5 sizes x 3 opt levels = 750 rows per condition
 *
 * Usage: ./test_baseline <condition>
 *   condition: baseline | uniform | skewed | perturbed
 *
 * Output: results/<condition>_decisions.csv
 *
 * Generated: 2026-02-19, updated with stratification
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "mir.h"
#include "mir-gen.h"

/* ---- External functions from patched mir-gen.c ---- */
extern void mir_experiment_init_logging(const char *filename);
extern void mir_experiment_close_logging(void);

/* ---- Experiment Parameters ---- */
#define NUM_FUNCS      50
#define NUM_SIZES      5
#define NUM_OPT_LEVELS 3

static const int sizes[NUM_SIZES] = {10, 50, 100, 200, 500};

/*
 * Baseline inlining thresholds per optimization level.
 *
 * Real compilers vary inlining aggressiveness with opt level:
 *   -O0: almost no inlining (threshold low, only tiny functions)
 *   -O1: moderate inlining
 *   -O2: aggressive inlining (threshold high, inline larger functions)
 *
 * These thresholds represent the "cost budget" in IR instructions
 * that the compiler is willing to duplicate for an inline.
 */
static const int baseline_thresholds[NUM_OPT_LEVELS] = {20, 50, 100};

/* ---- CSV Logging (extended with opt_level) ---- */

static FILE *experiment_log_v2 = NULL;

static void init_logging_v2(const char *filename) {
    experiment_log_v2 = fopen(filename, "w");
    if (experiment_log_v2) {
        fprintf(experiment_log_v2,
            "func_name,shadow_price,ir_count,opt_level,"
            "inlined,threshold_baseline,threshold_adjusted\n");
    }
}

static void close_logging_v2(void) {
    if (experiment_log_v2) {
        fclose(experiment_log_v2);
        experiment_log_v2 = NULL;
    }
}

/*
 * should_inline_with_num_v2:
 *   NUM-aware inlining decision with per-opt-level baseline threshold.
 *
 *   Formula: adjusted_threshold = baseline_threshold[opt_level] * (shadow_price / 100.0)
 *   Decision: inline if ir_count < adjusted_threshold
 *
 *   Lambda scaling clamped to [0.1, 5.0] to prevent degenerate decisions.
 *   Adjusted threshold clamped to [5, 1000].
 */
static int should_inline_with_num_v2(const char *func_name, int ir_count,
                                      double shadow_price, int opt_level) {
    int base_thresh = baseline_thresholds[opt_level];

    double lambda_scale = shadow_price / 100.0;
    if (lambda_scale < 0.1) lambda_scale = 0.1;
    if (lambda_scale > 5.0) lambda_scale = 5.0;

    int adjusted_threshold = (int)(base_thresh * lambda_scale);
    if (adjusted_threshold < 5) adjusted_threshold = 5;
    if (adjusted_threshold > 1000) adjusted_threshold = 1000;

    int inlined = (ir_count < adjusted_threshold) ? 1 : 0;

    if (experiment_log_v2) {
        fprintf(experiment_log_v2, "%s,%.2f,%d,%d,%d,%d,%d\n",
                func_name, shadow_price, ir_count, opt_level,
                inlined, base_thresh, adjusted_threshold);
        fflush(experiment_log_v2);
    }

    return inlined;
}

/* ---- Shadow Price Assignment per Condition ---- */

static double get_shadow_price(const char *condition, int func_idx, int size_idx) {
    (void)size_idx;

    if (strcmp(condition, "baseline") == 0) {
        return 100.0;
    } else if (strcmp(condition, "uniform") == 0) {
        return 100.0;
    } else if (strcmp(condition, "skewed") == 0) {
        if (func_idx % 2 == 0)
            return 1000.0;
        else
            return 10.0;
    } else if (strcmp(condition, "perturbed") == 0) {
        /*
         * Perturbed: same hot/cold split as skewed, but with +/-noise.
         * Hot (even): 1000.0 + uniform noise in [-200, +200]
         * Cold (odd):   10.0 + uniform noise in [-5, +5]
         * Clamped to [1.0, 2000.0].
         */
        double noise = ((double)(rand() % 401) - 200.0);
        double base;
        if (func_idx % 2 == 0) {
            base = 1000.0 + noise;
        } else {
            double small_noise = ((double)(rand() % 11) - 5.0);
            base = 10.0 + small_noise;
        }
        if (base < 1.0) base = 1.0;
        if (base > 2000.0) base = 2000.0;
        return base;
    }
    return 100.0;
}

/*
 * create_synthetic_func:
 *   Creates a MIR function with `ir_count` synthetic instructions.
 */
static MIR_item_t create_synthetic_func(MIR_context_t ctx, MIR_module_t mod,
                                         const char *func_name, int ir_count) {
    (void)mod;
    MIR_item_t func_item = MIR_new_func(ctx, func_name, 0, NULL, 0);
    MIR_reg_t reg = MIR_new_func_reg(ctx, func_item->u.func, MIR_T_I64, "acc");
    MIR_op_t reg_op = MIR_new_reg_op(ctx, reg);
    MIR_op_t one_op = MIR_new_int_op(ctx, 1);

    for (int i = 0; i < ir_count; i++) {
        MIR_append_insn(ctx, func_item,
                        MIR_new_insn(ctx, MIR_ADD, reg_op, reg_op, one_op));
    }

    MIR_append_insn(ctx, func_item, MIR_new_ret_insn(ctx, 0));
    MIR_finish_func(ctx);
    return func_item;
}

/*
 * run_experiment:
 *   For each opt_level in {0, 1, 2}:
 *     For each of NUM_FUNCS functions at each of NUM_SIZES sizes:
 *       1. Generate synthetic MIR function
 *       2. Assign shadow_price based on condition
 *       3. Call should_inline_with_num_v2() which logs the decision
 *
 *   Total rows per condition: NUM_FUNCS x NUM_SIZES x NUM_OPT_LEVELS = 750
 */
static int run_experiment(const char *condition) {
    char logfile[512];
    snprintf(logfile, sizeof(logfile), "results/%s_decisions.csv", condition);

    init_logging_v2(logfile);

    int total_per_condition = NUM_FUNCS * NUM_SIZES * NUM_OPT_LEVELS;
    printf("  Condition:  %s\n", condition);
    printf("  Log file:   %s\n", logfile);
    printf("  Functions:  %d funcs x %d sizes x %d opt levels = %d total\n",
           NUM_FUNCS, NUM_SIZES, NUM_OPT_LEVELS, total_per_condition);

    MIR_context_t ctx = MIR_init();
    if (!ctx) {
        fprintf(stderr, "ERROR: MIR_init() failed\n");
        return 1;
    }

    MIR_module_t mod = MIR_new_module(ctx, "num_experiment");

    int counts[NUM_OPT_LEVELS][2]; /* [level][0=not_inlined, 1=inlined] */
    memset(counts, 0, sizeof(counts));

    for (int opt = 0; opt < NUM_OPT_LEVELS; opt++) {
        for (int fi = 0; fi < NUM_FUNCS; fi++) {
            for (int si = 0; si < NUM_SIZES; si++) {
                int ir_count = sizes[si];
                double shadow_price = get_shadow_price(condition, fi, si);

                char func_name[128];
                snprintf(func_name, sizeof(func_name),
                         "func_%d_size_%d_opt_%d", fi, ir_count, opt);

                MIR_item_t func_item = create_synthetic_func(
                    ctx, mod, func_name, ir_count);
                func_item->u.func->shadow_price = shadow_price;

                int inlined = should_inline_with_num_v2(
                    func_name, ir_count, shadow_price, opt);

                counts[opt][inlined ? 1 : 0]++;
            }
        }
    }

    MIR_finish_module(ctx);
    MIR_finish(ctx);
    close_logging_v2();

    /* Per-level summary */
    int total_inlined = 0, total_not = 0;
    for (int opt = 0; opt < NUM_OPT_LEVELS; opt++) {
        int yes = counts[opt][1];
        int no  = counts[opt][0];
        total_inlined += yes;
        total_not += no;
        printf("    Opt %d (thresh=%3d): %3d inlined, %3d not (%.1f%%)\n",
               opt, baseline_thresholds[opt], yes, no,
               100.0 * yes / (yes + no));
    }
    printf("  Total:      %d inlined, %d not inlined (%.1f%% rate)\n",
           total_inlined, total_not,
           100.0 * total_inlined / (total_inlined + total_not));
    printf("  Saved:      %s\n\n", logfile);

    return 0;
}

/* ---- Main ---- */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <condition>\n"
                "  condition: baseline | uniform | skewed | perturbed\n",
                argv[0]);
        return 1;
    }

    const char *condition = argv[1];

    if (strcmp(condition, "baseline") != 0 &&
        strcmp(condition, "uniform") != 0 &&
        strcmp(condition, "skewed") != 0 &&
        strcmp(condition, "perturbed") != 0) {
        fprintf(stderr, "ERROR: Unknown condition '%s'\n", condition);
        fprintf(stderr, "  Valid: baseline, uniform, skewed, perturbed\n");
        return 1;
    }

    printf("========================================\n");
    printf("MIR NUM Experiment (v2 - stratified)\n");
    printf("========================================\n\n");

    /* Fixed seed for reproducible perturbed noise */
    srand(42);

    int rc = run_experiment(condition);

    if (rc != 0) {
        fprintf(stderr, "ERROR: Experiment failed with code %d\n", rc);
        return rc;
    }

    printf("Done.\n");
    return 0;
}
