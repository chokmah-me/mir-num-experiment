# Phase 2 Proof of Concept: Context Collapse via `MIR_INLINE`

## Objective
Before building the full Network Utility Maximization (NUM) telemetry and pricing engine, we must verify the physical value of the commodity our shadow-market will trade: **Inlining**. 

Initial tests showed that eliminating the basic stack-frame overhead of a `MIR_CALL` only yielded a ~1.5% speedup. For a decentralized pricing model to effectively optimize code, the act of inlining must trigger deeper cross-boundary optimizations like **Dead Code Elimination (DCE)**.

## Methodology
We constructed a dynamic branch workload (~65 IR instructions) where a "driver" loops 100,000,000 times, passing a constant flag to a "work" function. 
* **Condition A (`MIR_CALL`)**: The compiler cannot see across the call boundary, forcing the CPU to evaluate the branch 100 million times.
* **Condition B (`MIR_INLINE`)**: The `work` function is mechanically spliced into the driver loop prior to `MIR_gen()` optimization. 

## Results
* **`MIR_CALL` Execution Time:** ~0.3985 sec
* **`MIR_INLINE` Execution Time:** ~0.0534 sec
* **Speedup Delta:** **7.46x**

## Conclusion
MASSIVE SIGNAL. MIR's `O2` generator successfully collapses the context and performs Dead Code Elimination across inlined boundaries. This proves that dynamically converting `MIR_CALL` to `MIR_INLINE` is a highly valuable optimization lever, validating the physical foundation for the upcoming shadow-price decentralized LTO experiment.
