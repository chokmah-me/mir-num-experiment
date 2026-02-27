/*
 * MIR Inlining Proof-of-Concept v2 (Context Collapse / DCE)
 * * PURPOSE: Test if MIR's O2 generator performs Dead Code Elimination
 * across inlined boundaries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mir.h"
#include "mir-gen.h"

/* Version with MIR_CALL (inlining BLOCKED) */
static const char *mir_call_version =
"m_test: module\n"
"  export driver\n"
"  p_work: proto i64, i64:flag, i64:x\n"
"  work: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  heavy:\n"
/* ~60 instructions to pad above the 50-insn threshold */
"    add t1, x, 1\n"    /* 1 */
"    add t2, t1, 2\n"   /* 2 */
"    mul t3, t2, 3\n"   /* 3 */
"    add t4, t3, t1\n"  /* 4 */
"    sub t1, t4, t2\n"  /* 5 */
"    add t2, t1, t3\n"  /* 6 */
"    mul t3, t2, 2\n"   /* 7 */
"    add t4, t3, t1\n"  /* 8 */
"    sub t1, t4, 5\n"   /* 9 */
"    add t2, t1, t4\n"  /* 10 */
"    mul t3, t2, 3\n"   /* 11 */
"    add t4, t3, t1\n"  /* 12 */
"    sub t1, t4, t2\n"  /* 13 */
"    add t2, t1, t3\n"  /* 14 */
"    mul t3, t2, 2\n"   /* 15 */
"    add t4, t3, t1\n"  /* 16 */
"    sub t1, t4, 5\n"   /* 17 */
"    add t2, t1, t4\n"  /* 18 */
"    mul t3, t2, 3\n"   /* 19 */
"    add t4, t3, t1\n"  /* 20 */
"    sub t1, t4, t2\n"  /* 21 */
"    add t2, t1, t3\n"  /* 22 */
"    mul t3, t2, 2\n"   /* 23 */
"    add t4, t3, t1\n"  /* 24 */
"    sub t1, t4, 5\n"   /* 25 */
"    add t2, t1, t4\n"  /* 26 */
"    mul t3, t2, 3\n"   /* 27 */
"    add t4, t3, t1\n"  /* 28 */
"    sub t1, t4, t2\n"  /* 29 */
"    add t2, t1, t3\n"  /* 30 */
"    mul t3, t2, 2\n"   /* 31 */
"    add t4, t3, t1\n"  /* 32 */
"    sub t1, t4, 5\n"   /* 33 */
"    add t2, t1, t4\n"  /* 34 */
"    mul t3, t2, 3\n"   /* 35 */
"    add t4, t3, t1\n"  /* 36 */
"    sub t1, t4, t2\n"  /* 37 */
"    add t2, t1, t3\n"  /* 38 */
"    mul t3, t2, 2\n"   /* 39 */
"    add t4, t3, t1\n"  /* 40 */
"    sub t1, t4, 5\n"   /* 41 */
"    add t2, t1, t4\n"  /* 42 */
"    mul t3, t2, 3\n"   /* 43 */
"    add t4, t3, t1\n"  /* 44 */
"    sub t1, t4, t2\n"  /* 45 */
"    add t2, t1, t3\n"  /* 46 */
"    mul t3, t2, 2\n"   /* 47 */
"    add t4, t3, t1\n"  /* 48 */
"    sub t1, t4, 5\n"   /* 49 */
"    add t2, t1, t4\n"  /* 50 */
"    mul t3, t2, 3\n"   /* 51 */
"    add t4, t3, t1\n"  /* 52 */
"    sub t1, t4, t2\n"  /* 53 */
"    add t2, t1, t3\n"  /* 54 */
"    mul t3, t2, 2\n"   /* 55 */
"    add t4, t3, t1\n"  /* 56 */
"    sub t1, t4, 5\n"   /* 57 */
"    add t2, t1, t4\n"  /* 58 */
"    add t3, t2, t1\n"  /* 59 */
"    add t4, t3, t2\n"  /* 60 */
"    ret t4\n"
"  endfunc\n"
"  driver: func i64, i64:n\n"
"    local i64:i, i64:sum\n"
"    mov i, 0\n"
"    mov sum, 0\n"
"  loop:\n"
"    bge done, i, n\n"
"    call p_work, work, sum, 1, sum\n"  /* <-- PASSING 1 AS FLAG */
"    add i, i, 1\n"
"    jmp loop\n"
"  done:\n"
"    ret sum\n"
"  endfunc\n"
"endmodule\n";

/* Version with MIR_INLINE (inlining ALLOWED) */
static const char *mir_inline_version =
"m_test: module\n"
"  export driver\n"
"  p_work: proto i64, i64:flag, i64:x\n"
"  work: func i64, i64:flag, i64:x\n"
"    local i64:t1, i64:t2, i64:t3, i64:t4\n"
"    bne heavy, flag, 1\n"
"    add t1, x, 1\n"
"    ret t1\n"
"  heavy:\n"
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
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    mul t3, t2, 3\n"
"    add t4, t3, t1\n"
"    sub t1, t4, t2\n"
"    add t2, t1, t3\n"
"    mul t3, t2, 2\n"
"    add t4, t3, t1\n"
"    sub t1, t4, 5\n"
"    add t2, t1, t4\n"
"    add t3, t2, t1\n"
"    add t4, t3, t2\n"
"    ret t4\n"
"  endfunc\n"
"  driver: func i64, i64:n\n"
"    local i64:i, i64:sum\n"
"    mov i, 0\n"
"    mov sum, 0\n"
"  loop:\n"
"    bge done, i, n\n"
"    inline p_work, work, sum, 1, sum\n"  /* <-- INLINE with 1 AS FLAG */
"    add i, i, 1\n"
"    jmp loop\n"
"  done:\n"
"    ret sum\n"
"  endfunc\n"
"endmodule\n";

/* Run one test condition. Returns wall-clock seconds. */
static double run_test(const char *ir_code, const char *label) {
    MIR_context_t ctx = MIR_init();
    MIR_gen_init(ctx);
    MIR_gen_set_optimize_level(ctx, 2); /* O2 optimization */

    MIR_scan_string(ctx, ir_code);
    MIR_module_t m = DLIST_TAIL(MIR_module_t, *MIR_get_module_list(ctx));
    MIR_load_module(ctx, m);
    MIR_link(ctx, MIR_set_gen_interface, NULL);

    MIR_item_t driver_item = NULL;
    for (MIR_item_t item = DLIST_HEAD(MIR_item_t, m->items); item != NULL; item = DLIST_NEXT(MIR_item_t, item)) {
        if (item->item_type == MIR_func_item && strcmp(item->u.func->name, "driver") == 0) {
            driver_item = item;
            break;
        }
    }

    if (driver_item == NULL || driver_item->addr == NULL) {
        fprintf(stderr, "ERROR: Could not find/compile 'driver' function\n");
        return -1.0;
    }

    typedef int64_t (*driver_fn_t)(int64_t);
    driver_fn_t driver_fn = (driver_fn_t)driver_item->addr;

    (void)driver_fn(1000); /* Warm up */

    int64_t N = 100000000LL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int64_t result = driver_fn(N);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("  [%s] result = %ld, time = %.4f sec\n", label, (long)result, elapsed);

    MIR_gen_finish(ctx);
    MIR_finish(ctx);
    return elapsed;
}

int main(void) {
    printf("MIR Inlining PoC v2 (Context Collapse)\n");
    printf("========================================\n");
    printf("Callee: Dynamic branching inside ~65 IR instruction body\n");
    printf("Loop:   100,000,000 iterations\n\n");

    printf("Condition A: MIR_CALL (Compiler evaluates branch 100M times)\n");
    double t_call = run_test(mir_call_version, "CALL");

    printf("\nCondition B: MIR_INLINE (Compiler should perform Dead Code Elimination)\n");
    double t_inline = run_test(mir_inline_version, "INLINE");

    printf("\n========================================\n");
    if (t_call > 0 && t_inline > 0) {
        double ratio = t_call / t_inline;
        printf("CALL time:   %.4f sec\n", t_call);
        printf("INLINE time: %.4f sec\n", t_inline);
        printf("Speedup:     %.2fx\n", ratio);
        
        if (ratio > 1.20) {
            printf("\nVERDICT: MASSIVE SIGNAL! MIR O2 successfully collapsed the context.\n");
        } else {
            printf("\nVERDICT: No collapse. MIR's O2 generator might not optimize across inline boundaries.\n");
        }
    }

    return 0;
}
