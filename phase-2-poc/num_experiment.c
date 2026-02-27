/*
 * NUM Shadow-Price Inlining Experiment (Paper 2, Option A)
 * Author: dyb (Chokmah LLC, ORCID: 0000-0002-9040-6914)
 *
 * PURPOSE: Implement the full dual decomposition protocol end-to-end and test
 * whether shadow-price-guided inlining produces measurably faster compiled code
 * than uninformed alternatives. Dependent variable: wall-clock execution time.
 *
 * ARCHITECTURE:
 *   Phase 1: External profiling pass (count call targets via IR traversal + interpreter)
 *   Phase 2: Price-guided MIR_CALL -> MIR_INLINE mutation (between load and link)
 *   Phase 3: JIT compilation + timed execution
 *
 * 5 EXPERIMENTAL CONDITIONS:
 *   1. No inlining       - All calls remain MIR_CALL (lower bound)
 *   2. Blind inline-all  - All MIR_CALL -> MIR_INLINE (upper bound)
 *   3. Random 50%        - Randomly convert half ("any inlining helps" control)
 *   4. Shadow-price      - NUM-predicted threshold formula (hypothesis)
 *   5. Inverted-price    - Hot->LOW threshold, cold->HIGH (killer control)
 *
 * BUILD (Linux/WSL2):
 *   gcc -O2 -DNDEBUG -I. num_experiment.c mir.c mir-gen.c -o num_experiment -lm -ldl -lpthread
 *
 * API ANCHORS (from PoC v2, confirmed working):
 *   - MIR_gen_init(ctx)                    [1-arg version, newer API]
 *   - MIR_gen_set_optimize_level(ctx, 2)   [2-arg version, newer API]
 *   - MIR_link(ctx, MIR_set_gen_interface, NULL)
 *   - driver_item->addr for function pointer extraction
 *   - DLIST_HEAD/DLIST_NEXT/DLIST_TAIL for item traversal
 *   - item->item_type == MIR_func_item, item->u.func->name
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include "mir.h"
#include "mir-gen.h"

/* ========================================================================
 * SECTION 1: CONFIGURATION
 * ======================================================================== */

/* MIR native thresholds (from mir.c lines 3851-3864) */
#define MIR_CALL_INLINE_THRESHOLD   50   /* MIR_MAX_INSNS_FOR_CALL_INLINE */
#define MIR_INLINE_THRESHOLD       200   /* MIR_MAX_INSNS_FOR_INLINE */

/* Experiment parameters */
#define NUM_RUNS         20       /* Runs per condition for statistical robustness */
#define WARMUP_ITERS     1000     /* Warmup iterations before timing */
#define BENCH_N          100000000LL  /* Main benchmark iteration count */

/* Shadow price formula bounds (from dossier Section 4.3) */
#define SCALE_FLOOR      0.1
#define SCALE_CEIL       5.0
#define THRESHOLD_FLOOR  5
/* THRESHOLD_CEIL = MIR_INLINE_THRESHOLD (200) */

/* Random seed for reproducibility */
#define RANDOM_SEED      42

/* ========================================================================
 * SECTION 2: PROFILING DATA STRUCTURES (Phase 1 - The Master)
 *
 * Simple hash map: function name -> call count.
 * We use name-based lookup because MIR_item_t pointers differ across
 * context re-initializations (each condition gets a fresh context).
 * ======================================================================== */

#define PROFILE_MAP_SIZE 256

typedef struct {
    const char *func_name;
    uint64_t    call_count;
    double      shadow_price;  /* Normalized after profiling */
} profile_entry_t;

typedef struct {
    profile_entry_t entries[PROFILE_MAP_SIZE];
    int             count;
    double          max_count;  /* For normalization */
} profile_map_t;

static void profile_map_init(profile_map_t *map) {
    memset(map, 0, sizeof(*map));
}

static profile_entry_t *profile_map_get(profile_map_t *map, const char *name) {
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].func_name, name) == 0)
            return &map->entries[i];
    }
    return NULL;
}

static void profile_map_increment(profile_map_t *map, const char *name) {
    profile_entry_t *e = profile_map_get(map, name);
    if (e) {
        e->call_count++;
    } else if (map->count < PROFILE_MAP_SIZE) {
        map->entries[map->count].func_name = name;
        map->entries[map->count].call_count = 1;
        map->entries[map->count].shadow_price = 0.0;
        map->count++;
    }
}

static void profile_map_normalize(profile_map_t *map) {
    /* Find max call count for normalization */
    map->max_count = 0;
    for (int i = 0; i < map->count; i++) {
        if (map->entries[i].call_count > map->max_count)
            map->max_count = (double)map->entries[i].call_count;
    }
    /* Normalize: shadow_price = call_count / max_count */
    if (map->max_count > 0) {
        for (int i = 0; i < map->count; i++) {
            map->entries[i].shadow_price =
                (double)map->entries[i].call_count / map->max_count;
        }
    }
}

static void profile_map_print(const profile_map_t *map) {
    printf("  Shadow Prices (%d functions profiled):\n", map->count);
    for (int i = 0; i < map->count; i++) {
        printf("    %-20s  calls=%-10lu  lambda=%.4f\n",
               map->entries[i].func_name,
               (unsigned long)map->entries[i].call_count,
               map->entries[i].shadow_price);
    }
}

/* ========================================================================
 * SECTION 3: PROFILING PASS (Phase 1)
 *
 * External profiling: traverse the IR to identify call targets and their
 * static call-site counts. For the first experiment, we use static analysis
 * augmented by loop structure to estimate execution frequency.
 *
 * For a production implementation, this would be replaced by interpreter
 * instrumentation (patching call_insn_execute() in mir-interp.c).
 *
 * The key insight: in our benchmarks, the driver loop calls work() N times.
 * Static call-site count * estimated loop trip count = approximate profile.
 * ======================================================================== */

/*
 * count_func_insns: Count instructions in a MIR function.
 * Anchored to: DLIST traversal pattern from PoC v2 (confirmed working).
 *
 * NOTE: We access func->insns via DLIST. The MIR_insn_t is iterable
 * through the same DLIST_HEAD/DLIST_NEXT pattern used for items.
 */
static int count_func_insns(MIR_func_t func) {
    int count = 0;
    for (MIR_insn_t insn = DLIST_HEAD(MIR_insn_t, func->insns);
         insn != NULL;
         insn = DLIST_NEXT(MIR_insn_t, insn)) {
        count++;
    }
    return count;
}

/*
 * profile_module: Static profiling pass.
 *
 * Walk every function in the module. For each MIR_CALL or MIR_INLINE
 * instruction, extract the callee name and record a call-site hit.
 *
 * Anchored to dossier Section 2.5:
 *   insn->ops[1].u.ref gives the callee item for call instructions.
 *   (ops[0] is the proto, ops[1] is the callee ref)
 *
 * We also estimate loop multipliers: if a call site is inside a loop
 * (detected by the presence of backward jumps), we weight it higher.
 * For simplicity in v1, we just count static call sites.
 */
static void profile_module(MIR_module_t m, profile_map_t *map) {
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items);
         item != NULL;
         item = DLIST_NEXT(MIR_item_t, item)) {

        if (item->item_type != MIR_func_item) continue;
        MIR_func_t func = item->u.func;

        for (MIR_insn_t insn = DLIST_HEAD(MIR_insn_t, func->insns);
             insn != NULL;
             insn = DLIST_NEXT(MIR_insn_t, insn)) {

            if (insn->code != MIR_CALL && insn->code != MIR_INLINE) continue;

            /*
             * For MIR_CALL/MIR_INLINE: ops layout is:
             *   ops[0] = proto ref
             *   ops[1] = callee ref (MIR_item_t)
             *   ops[2..] = return value(s), then arguments
             *
             * Anchored to dossier Section 2.5:
             *   insn->ops[1].u.ref gives the callee item
             */
            MIR_item_t callee_item = insn->ops[1].u.ref;
            if (callee_item == NULL) continue;
            if (callee_item->item_type != MIR_func_item) continue;

            const char *callee_name = callee_item->u.func->name;
            profile_map_increment(map, callee_name);
        }
    }
    profile_map_normalize(map);
}

/* ========================================================================
 * SECTION 4: MUTATION PASS (Phase 2 - The Agent)
 *
 * Implements mir_num_optimize_calls(): runs between MIR_load_module()
 * and MIR_link() to mutate MIR_CALL -> MIR_INLINE based on condition.
 *
 * The mutation is: insn->code = MIR_INLINE
 * This promotes the call to use the 200-insn threshold in MIR's native
 * process_inlines() instead of the 50-insn threshold for MIR_CALL.
 *
 * NOTE ON TWO-STAGE FILTER: Our mutation promotes MIR_CALL -> MIR_INLINE,
 * then MIR's native process_inlines() in MIR_link() makes the final
 * inlining decision at the 200-insn threshold. We are NOT bypassing
 * MIR's native inliner; we are feeding it different instruction codes.
 * ======================================================================== */

typedef enum {
    COND_NO_INLINE      = 0,  /* Leave all as MIR_CALL */
    COND_BLIND_ALL      = 1,  /* Convert all MIR_CALL -> MIR_INLINE */
    COND_RANDOM_50      = 2,  /* Random 50% conversion */
    COND_SHADOW_PRICE   = 3,  /* NUM-predicted threshold */
    COND_INVERTED_PRICE = 4,  /* Inverted: hot->low, cold->high */
    COND_COUNT          = 5
} experiment_condition_t;

static const char *condition_names[COND_COUNT] = {
    "No inlining",
    "Blind inline-all",
    "Random 50%",
    "Shadow-price (NUM)",
    "Inverted-price (control)"
};

/*
 * compute_adjusted_threshold: The NUM shadow-price formula.
 * From dossier Section 4.3 (one free parameter, simple linear scaling).
 *
 * For COND_SHADOW_PRICE:
 *   scale = clamp(lambda, 0.1, 5.0)
 *   T_adj = clamp(50 * scale, 5, 200)
 *
 * For COND_INVERTED_PRICE:
 *   lambda_inv = 1.0 - lambda  (hot becomes cold, cold becomes hot)
 *   Then same formula.
 */
static int compute_adjusted_threshold(double shadow_price, int inverted) {
    double lambda = shadow_price;
    if (inverted) {
        /* Invert: hottest (lambda=1.0) -> coldest (0.0), and vice versa */
        lambda = 1.0 - lambda;
    }

    /* Scale factor: lambda is already [0,1], scale to [0.1, 5.0] range */
    /* Map [0,1] -> [SCALE_FLOOR, SCALE_CEIL] linearly */
    double scale_factor = SCALE_FLOOR + lambda * (SCALE_CEIL - SCALE_FLOOR);
    if (scale_factor < SCALE_FLOOR) scale_factor = SCALE_FLOOR;
    if (scale_factor > SCALE_CEIL)  scale_factor = SCALE_CEIL;

    int adjusted = (int)(MIR_CALL_INLINE_THRESHOLD * scale_factor);
    if (adjusted < THRESHOLD_FLOOR)    adjusted = THRESHOLD_FLOOR;
    if (adjusted > MIR_INLINE_THRESHOLD) adjusted = MIR_INLINE_THRESHOLD;

    return adjusted;
}

/*
 * Recursion breaker: simple visited-set to prevent cycles.
 * Track which callee names we've already decided to inline in the
 * current mutation pass to prevent mutual-recursion expansion.
 */
#define MAX_INLINE_CHAIN 64

typedef struct {
    const char *names[MAX_INLINE_CHAIN];
    int         depth;
} inline_chain_t;

static void chain_init(inline_chain_t *chain) {
    chain->depth = 0;
}

static int chain_contains(const inline_chain_t *chain, const char *name) {
    for (int i = 0; i < chain->depth; i++) {
        if (strcmp(chain->names[i], name) == 0) return 1;
    }
    return 0;
}

static int chain_push(inline_chain_t *chain, const char *name) {
    if (chain->depth >= MAX_INLINE_CHAIN) return 0;
    chain->names[chain->depth++] = name;
    return 1;
}

/*
 * mutate_module: Apply experimental condition to all MIR_CALL instructions.
 *
 * Runs BEFORE MIR_link(). Modifies insn->code in-place.
 *
 * Returns: number of MIR_CALL instructions promoted to MIR_INLINE.
 */
static int mutate_module(MIR_module_t m,
                         experiment_condition_t condition,
                         const profile_map_t *profile,
                         unsigned int *rng_state) {
    int mutations = 0;
    inline_chain_t chain;
    chain_init(&chain);

    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items);
         item != NULL;
         item = DLIST_NEXT(MIR_item_t, item)) {

        if (item->item_type != MIR_func_item) continue;
        MIR_func_t func = item->u.func;

        for (MIR_insn_t insn = DLIST_HEAD(MIR_insn_t, func->insns);
             insn != NULL;
             insn = DLIST_NEXT(MIR_insn_t, insn)) {

            if (insn->code != MIR_CALL) continue;

            /* Extract callee (same pattern as profiling pass) */
            MIR_item_t callee_item = insn->ops[1].u.ref;
            if (callee_item == NULL) continue;
            if (callee_item->item_type != MIR_func_item) continue;

            const char *callee_name = callee_item->u.func->name;
            int callee_insns = count_func_insns(callee_item->u.func);

            /* Recursion breaker: skip if callee already in chain */
            if (chain_contains(&chain, callee_name)) continue;

            int should_promote = 0;

            switch (condition) {
            case COND_NO_INLINE:
                /* Leave as MIR_CALL */
                should_promote = 0;
                break;

            case COND_BLIND_ALL:
                /* Promote everything */
                should_promote = 1;
                break;

            case COND_RANDOM_50:
                /* XorShift32 for reproducible randomness */
                *rng_state ^= *rng_state << 13;
                *rng_state ^= *rng_state >> 17;
                *rng_state ^= *rng_state << 5;
                should_promote = (*rng_state % 2 == 0);
                break;

            case COND_SHADOW_PRICE: {
                profile_entry_t *pe = profile_map_get(
                    (profile_map_t *)profile, callee_name);
                double lambda = pe ? pe->shadow_price : 0.0;
                int threshold = compute_adjusted_threshold(lambda, 0);
                should_promote = (callee_insns < threshold);
                break;
            }

            case COND_INVERTED_PRICE: {
                profile_entry_t *pe = profile_map_get(
                    (profile_map_t *)profile, callee_name);
                double lambda = pe ? pe->shadow_price : 0.0;
                int threshold = compute_adjusted_threshold(lambda, 1);
                should_promote = (callee_insns < threshold);
                break;
            }

            default:
                break;
            }

            if (should_promote) {
                insn->code = MIR_INLINE;
                chain_push(&chain, callee_name);
                mutations++;
            }
        }
    }

    return mutations;
}

/* ========================================================================
 * SECTION 5: BENCHMARK IR (v2 - 8-function growth-budget stress test)
 *
 * 8 callee functions at varying sizes in the 55-180 insn sweet spot:
 *   HOT  (called every iter):       f_hot1(55), f_hot2(75), f_hot3(100), f_hot4(60)
 *   WARM (called every 10th iter):  f_warm1(80), f_warm2(120)
 *   COLD (called every 1000th iter): f_cold1(150), f_cold2(180)
 *
 * Total callee insns if all inlined: 55+75+100+60+80+120+150+180 = 820
 * Driver starts at ~40 insns. Growth to ~860 = 2150% >> 150% budget.
 * MIR growth check: caller > 150% original AND > 200 insns -> stop.
 * Blind-all WILL hit the ceiling. Shadow-price should prioritize hot.
 * ======================================================================== */

static const char *benchmark_ir =
"m_bench: module\n"
"  export driver\n"
"  p_f_hot1: proto i64, i64:flag, i64:x\n"
"  p_f_hot2: proto i64, i64:flag, i64:x\n"
"  p_f_hot3: proto i64, i64:flag, i64:x\n"
"  p_f_hot4: proto i64, i64:flag, i64:x\n"
"  p_f_warm1: proto i64, i64:flag, i64:x\n"
"  p_f_warm2: proto i64, i64:flag, i64:x\n"
"  p_f_cold1: proto i64, i64:flag, i64:x\n"
"  p_f_cold2: proto i64, i64:flag, i64:x\n"
"\n"
/* --- f_hot1: ~55 insns, HOT path --- */
"  f_hot1: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne h1_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  h1_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_hot2: ~75 insns, HOT path --- */
"  f_hot2: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne h2_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  h2_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_hot3: ~100 insns, HOT path --- */
"  f_hot3: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne h3_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  h3_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_hot4: ~60 insns, HOT path --- */
"  f_hot4: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne h4_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  h4_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_warm1: ~80 insns, WARM path --- */
"  f_warm1: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne w1_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  w1_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_warm2: ~120 insns, WARM path --- */
"  f_warm2: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne w2_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  w2_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_cold1: ~150 insns, COLD path --- */
"  f_cold1: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne c1_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  c1_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- f_cold2: ~180 insns, COLD path --- */
"  f_cold2: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne c2_heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  c2_heavy:\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    sub t4, t3, 5\n"
"    add t1, x, 1\n"
"    add t2, t1, 2\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    sub t4, t3, t1\n"
"    add t1, t4, t2\n"
"    mul t2, t1, 2\n"
"    add t3, t2, t4\n"
"    ret t4\n"
"  endfunc\n"
"\n"
/* --- driver: 8-function benchmark with hot/warm/cold separation --- */
/*
 * Call frequencies (per N iterations):
 *   f_hot1..f_hot4:  N each      (lambda ~ 1.0)
 *   f_warm1,f_warm2: N/10 each   (lambda ~ 0.1)
 *   f_cold1,f_cold2: N/1000 each (lambda ~ 0.01)
 *
 * Total callee insns if all inlined: 55+75+100+60+80+120+150+180 = 820
 * Driver starts at ~40 insns. Growth to ~860 = 2150% >> 150% budget.
 * MIR growth check: caller > 150% original AND > 200 insns -> stop inlining.
 * So blind-all will inline the first few and then hit the ceiling.
 * Shadow-price should inline hot functions first (highest value per budget).
 */
"  driver: func i64, i64:n\n"
"    local i64:i, i64:sum, i64:tmp, i64:mod\n"
"    mov i, 0\n"
"    mov sum, 0\n"
"  loop:\n"
"    bge done, i, n\n"
/* Hot: every iteration */
"    call p_f_hot1, f_hot1, sum, 1, sum\n"
"    call p_f_hot2, f_hot2, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"    call p_f_hot3, f_hot3, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"    call p_f_hot4, f_hot4, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
/* Warm: every 10th iteration */
"    mod mod, i, 10\n"
"    bne skip_warm, mod, 0\n"
"    call p_f_warm1, f_warm1, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"    call p_f_warm2, f_warm2, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"  skip_warm:\n"
/* Cold: every 1000th iteration */
"    mod mod, i, 1000\n"
"    bne skip_cold, mod, 0\n"
"    call p_f_cold1, f_cold1, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"    call p_f_cold2, f_cold2, tmp, 1, sum\n"
"    add sum, sum, tmp\n"
"  skip_cold:\n"
"    add i, i, 1\n"
"    jmp loop\n"
"  done:\n"
"    ret sum\n"
"  endfunc\n"
"endmodule\n";

/* ========================================================================
 * SECTION 6: EXPERIMENT RUNNER (Phase 3)
 *
 * For each condition:
 *   1. Fresh MIR context (clean state)
 *   2. Scan IR string
 *   3. Profile module (Phase 1)
 *   4. Apply mutation (Phase 2)
 *   5. Link + JIT compile
 *   6. Execute with timing (Phase 3)
 *   7. Tear down context
 *
 * Lifecycle anchored to PoC v2's confirmed working pattern.
 * ======================================================================== */

typedef struct {
    double times[NUM_RUNS];
    double mean;
    double stddev;
    int    mutations;
    int64_t result;  /* Correctness check: all conditions should produce same result */
} condition_result_t;

static int run_condition(experiment_condition_t condition,
                         const profile_map_t *profile,
                         condition_result_t *out) {
    memset(out, 0, sizeof(*out));
    unsigned int rng_state = RANDOM_SEED;

    for (int run = 0; run < NUM_RUNS; run++) {
        /* Step 1: Fresh context */
        MIR_context_t ctx = MIR_init();
        MIR_gen_init(ctx);
        MIR_gen_set_optimize_level(ctx, 2);  /* O2 for full optimization */

        /* Step 2: Scan IR */
        MIR_scan_string(ctx, benchmark_ir);
        MIR_module_t m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));

        /* Step 3: Load module (makes items traversable) */
        MIR_load_module(ctx, m);

        /* Step 4: Mutate (between load and link) */
        rng_state = RANDOM_SEED;  /* Reset RNG per run for reproducibility */
        int muts = mutate_module(m, condition, profile, &rng_state);
        if (run == 0) out->mutations = muts;

        /* Step 5: Link + JIT compile */
        MIR_link(ctx, MIR_set_gen_interface, NULL);

        /* Step 6: Find driver function */
        MIR_item_t driver_item = NULL;
        for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items);
             item != NULL;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item &&
                strcmp(item->u.func->name, "driver") == 0) {
                driver_item = item;
                break;
            }
        }

        if (driver_item == NULL || driver_item->addr == NULL) {
            fprintf(stderr, "ERROR: Could not find/compile 'driver'\n");
            MIR_gen_finish(ctx);
            MIR_finish(ctx);
            return -1;
        }

        typedef int64_t (*driver_fn_t)(int64_t);
        driver_fn_t driver_fn = (driver_fn_t)driver_item->addr;

        /* Warmup */
        (void)driver_fn(WARMUP_ITERS);

        /* Timed execution */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int64_t result = driver_fn(BENCH_N);
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double elapsed = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;
        out->times[run] = elapsed;
        if (run == 0) out->result = result;

        /* Step 7: Tear down */
        MIR_gen_finish(ctx);
        MIR_finish(ctx);
    }

    /* Compute statistics */
    double sum = 0, sum2 = 0;
    for (int i = 0; i < NUM_RUNS; i++) sum += out->times[i];
    out->mean = sum / NUM_RUNS;
    for (int i = 0; i < NUM_RUNS; i++) {
        double d = out->times[i] - out->mean;
        sum2 += d * d;
    }
    out->stddev = (NUM_RUNS > 1) ? sqrt(sum2 / (NUM_RUNS - 1)) : 0.0;

    return 0;
}

/* ========================================================================
 * SECTION 7: MAIN - ORCHESTRATE THE FULL EXPERIMENT
 * ======================================================================== */

/*
 * cohen_d: Effect size between two conditions.
 * d = (mean_a - mean_b) / pooled_sd
 */
static double cohen_d(const condition_result_t *a, const condition_result_t *b) {
    double pooled_var = (a->stddev * a->stddev + b->stddev * b->stddev) / 2.0;
    double pooled_sd = sqrt(pooled_var);
    if (pooled_sd < 1e-12) return 0.0;
    return (a->mean - b->mean) / pooled_sd;
}

int main(void) {
    printf("================================================================\n");
    printf("NUM Shadow-Price Inlining Experiment (Paper 2, Option A)\n");
    printf("================================================================\n");
    printf("Benchmark: 8-function module (4 hot, 2 warm, 2 cold)\n");
    printf("Iterations: %lld\n", (long long)BENCH_N);
    printf("Runs per condition: %d\n", NUM_RUNS);
    printf("Optimization level: O2\n\n");

    /* ---- Phase 1: Profile the benchmark ---- */
    printf("Phase 1: Profiling benchmark IR...\n");

    /* We need a temporary context just to parse the IR and profile it */
    MIR_context_t prof_ctx = MIR_init();
    MIR_scan_string(prof_ctx, benchmark_ir);
    MIR_module_t prof_m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(prof_ctx));

    profile_map_t profile;
    profile_map_init(&profile);

    /* Synthetic profiling based on known benchmark structure:
     *   f_hot1..f_hot4:  called N times each    -> weight 1000 (lambda ~ 1.0)
     *   f_warm1,f_warm2: called N/10 times each -> weight 100  (lambda ~ 0.1)
     *   f_cold1,f_cold2: called N/1000 each     -> weight 1    (lambda ~ 0.001)
     */
    for (int i = 0; i < 1000; i++) profile_map_increment(&profile, "f_hot1");
    for (int i = 0; i < 1000; i++) profile_map_increment(&profile, "f_hot2");
    for (int i = 0; i < 1000; i++) profile_map_increment(&profile, "f_hot3");
    for (int i = 0; i < 1000; i++) profile_map_increment(&profile, "f_hot4");
    for (int i = 0; i < 100; i++) profile_map_increment(&profile, "f_warm1");
    for (int i = 0; i < 100; i++) profile_map_increment(&profile, "f_warm2");
    profile_map_increment(&profile, "f_cold1");
    profile_map_increment(&profile, "f_cold2");

    profile_map_normalize(&profile);
    profile_map_print(&profile);

    MIR_finish(prof_ctx);

    /* Print instruction counts for reference */
    printf("\n  Function sizes (IR instruction count):\n");
    {
        MIR_context_t tmp_ctx = MIR_init();
        MIR_scan_string(tmp_ctx, benchmark_ir);
        MIR_module_t tmp_m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(tmp_ctx));
        for (MIR_item_t item = DLIST_HEAD(MIR_item_t, tmp_m->items);
             item != NULL;
             item = DLIST_NEXT(MIR_item_t, item)) {
            if (item->item_type == MIR_func_item) {
                int n = count_func_insns(item->u.func);
                printf("    %-20s  %d insns", item->u.func->name, n);
                if (n > MIR_CALL_INLINE_THRESHOLD && n <= MIR_INLINE_THRESHOLD)
                    printf("  [IN SWEET SPOT: 50 < n <= 200]");
                else if (n <= MIR_CALL_INLINE_THRESHOLD)
                    printf("  [AUTO-INLINED by MIR_CALL threshold]");
                else
                    printf("  [TOO LARGE for any inlining]");
                printf("\n");
            }
        }
        MIR_finish(tmp_ctx);
    }

    /* Print threshold decisions for shadow-price vs inverted */
    printf("\n  Threshold decisions (shadow-price formula):\n");
    for (int i = 0; i < profile.count; i++) {
        int t_normal = compute_adjusted_threshold(profile.entries[i].shadow_price, 0);
        int t_invert = compute_adjusted_threshold(profile.entries[i].shadow_price, 1);
        printf("    %-20s  lambda=%.4f  T_shadow=%3d  T_inverted=%3d\n",
               profile.entries[i].func_name,
               profile.entries[i].shadow_price,
               t_normal, t_invert);
    }

    /* ---- Phase 2+3: Run all 5 conditions ---- */
    printf("\n================================================================\n");
    printf("Phase 2+3: Running %d conditions x %d runs each...\n\n",
           COND_COUNT, NUM_RUNS);

    condition_result_t results[COND_COUNT];

    for (int c = 0; c < COND_COUNT; c++) {
        printf("  Condition %d: %s ...", c + 1, condition_names[c]);
        fflush(stdout);

        if (run_condition((experiment_condition_t)c, &profile, &results[c]) != 0) {
            fprintf(stderr, "\n  FAILED!\n");
            return 1;
        }

        printf(" done (mutations=%d, mean=%.4f s, sd=%.4f s, result=%ld)\n",
               results[c].mutations,
               results[c].mean,
               results[c].stddev,
               (long)results[c].result);
    }

    /* ---- Phase 4: Statistical Summary ---- */
    printf("\n================================================================\n");
    printf("RESULTS SUMMARY\n");
    printf("================================================================\n\n");

    printf("%-30s  %10s  %10s  %10s\n",
           "Condition", "Mean (s)", "SD (s)", "Mutations");
    printf("%-30s  %10s  %10s  %10s\n",
           "------------------------------", "----------", "----------", "----------");
    for (int c = 0; c < COND_COUNT; c++) {
        printf("%-30s  %10.4f  %10.4f  %10d\n",
               condition_names[c],
               results[c].mean,
               results[c].stddev,
               results[c].mutations);
    }

    /* Pairwise comparisons (predictions from dossier Section 4.5) */
    printf("\nFALSIFIABLE PREDICTIONS:\n\n");

    /* P1: Shadow-price vs Random 50% */
    double d1 = cohen_d(&results[COND_RANDOM_50], &results[COND_SHADOW_PRICE]);
    printf("  P1: Shadow-price vs Random 50%%\n");
    printf("      Shadow=%.4f s, Random=%.4f s, Cohen's d=%.2f\n",
           results[COND_SHADOW_PRICE].mean,
           results[COND_RANDOM_50].mean, d1);
    printf("      %s\n\n",
           results[COND_SHADOW_PRICE].mean < results[COND_RANDOM_50].mean
           ? "CONFIRMED: Shadow-price outperforms random"
           : "FALSIFIED: Random matches or beats shadow-price");

    /* P2: Shadow-price vs No inlining */
    double d2 = cohen_d(&results[COND_NO_INLINE], &results[COND_SHADOW_PRICE]);
    printf("  P2: Shadow-price vs No inlining\n");
    printf("      Shadow=%.4f s, None=%.4f s, Cohen's d=%.2f\n",
           results[COND_SHADOW_PRICE].mean,
           results[COND_NO_INLINE].mean, d2);
    printf("      %s\n\n",
           results[COND_SHADOW_PRICE].mean < results[COND_NO_INLINE].mean
           ? "CONFIRMED: Shadow-price outperforms no inlining"
           : "FALSIFIED: No inlining matches or beats shadow-price");

    /* P3: Shadow-price vs Inverted-price (THE KILLER TEST) */
    double d3 = cohen_d(&results[COND_INVERTED_PRICE], &results[COND_SHADOW_PRICE]);
    printf("  P3: Shadow-price vs Inverted-price (KILLER CONTROL)\n");
    printf("      Shadow=%.4f s, Inverted=%.4f s, Cohen's d=%.2f\n",
           results[COND_SHADOW_PRICE].mean,
           results[COND_INVERTED_PRICE].mean, d3);
    printf("      %s\n\n",
           results[COND_SHADOW_PRICE].mean < results[COND_INVERTED_PRICE].mean
           ? "CONFIRMED: Price signal carries information"
           : "FALSIFIED: Inverted prices match or beat correct prices");

    /* P4: Shadow-price vs Blind inline-all */
    double d4 = cohen_d(&results[COND_BLIND_ALL], &results[COND_SHADOW_PRICE]);
    printf("  P4: Shadow-price vs Blind inline-all\n");
    printf("      Shadow=%.4f s, Blind=%.4f s, Cohen's d=%.2f\n",
           results[COND_SHADOW_PRICE].mean,
           results[COND_BLIND_ALL].mean, d4);
    if (results[COND_SHADOW_PRICE].mean < results[COND_BLIND_ALL].mean) {
        printf("      Shadow-price outperforms blind (discrimination helps)\n\n");
    } else {
        printf("      Blind matches or beats shadow-price "
               "(inlining everything is fine here)\n\n");
    }

    /* Raw timing data for external analysis */
    printf("================================================================\n");
    printf("RAW TIMING DATA (for external statistical analysis)\n");
    printf("================================================================\n");
    printf("condition,run,time_sec\n");
    for (int c = 0; c < COND_COUNT; c++) {
        for (int r = 0; r < NUM_RUNS; r++) {
            printf("%s,%d,%.6f\n", condition_names[c], r + 1, results[c].times[r]);
        }
    }

    return 0;
}
