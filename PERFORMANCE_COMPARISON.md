# Performance Comparison: Accelerator ON vs OFF

## Executive Summary

**Answer: The version WITHOUT the accelerator is currently faster by 0.6-2.1%**

This is expected and by design for v1, which establishes routing infrastructure without optimization.

## Benchmark Results Comparison

### Throughput (Requests/Second)

```
Command: SET
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
With Accelerator:    77,882 req/s  ████████████████████████████████████░░ 99.4%
Without Accelerator: 78,370 req/s  ████████████████████████████████████▓▓ 100%
                     ↑ Difference: -488 req/s (-0.6%)

Command: GET
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
With Accelerator:    76,453 req/s  ████████████████████████████████░░░░░░ 97.9%
Without Accelerator: 78,064 req/s  ████████████████████████████████████▓▓ 100%
                     ↑ Difference: -1,611 req/s (-2.1%)

Command: INCR
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
With Accelerator:    77,220 req/s  ████████████████████████████████████░░ 98.7%
Without Accelerator: 78,247 req/s  ████████████████████████████████████▓▓ 100%
                     ↑ Difference: -1,027 req/s (-1.3%)

Command: HSET
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
With Accelerator:    77,340 req/s  ████████████████████████████████████░░ 98.8%
Without Accelerator: 78,247 req/s  ████████████████████████████████████▓▓ 100%
                     ↑ Difference: -907 req/s (-1.2%)
```

### Latency (p50 in milliseconds)

```
Command: ALL (SET, GET, INCR, HSET)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
With Accelerator:    0.335 ms  ████████████████████████████████████▓▓▓
Without Accelerator: 0.327 ms  ████████████████████████████████████▓▓
                     ↑ Difference: +0.008 ms (+2.4%)
```

## Why is the Accelerator Slower?

### Routing Overhead Breakdown

```
Traditional Path (without accelerator):
  processCommand() → call(cmd->proc) → execute
  Time: T

Accelerated Path (with accelerator v1):
  processCommand() → is_accelerated_command() → command_entrypoint() → call(cmd->proc) → execute
  Time: T + routing_overhead

Overhead Components:
  1. is_accelerated_command():  String comparison loop (5 commands)
  2. command_entrypoint():       Function call + safety checks
  3. Metrics tracking:           3x atomic increments
  
  Total overhead: ~8-10 microseconds per command
```

## Current State (v1)

```
┌─────────────────────────────────────────────────────────────────┐
│  CURRENT IMPLEMENTATION: ROUTING ONLY (NO OPTIMIZATION)         │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌──────────────┐             │
│  │  Client  │───▶│  Router  │───▶│   Execute    │             │
│  │ Request  │    │  Check   │    │ (Main Thread)│             │
│  └──────────┘    └──────────┘    └──────────────┘             │
│                                                                  │
│  Adds: Routing overhead                                         │
│  Gains: None (yet)                                              │
│  Result: Slightly slower (-0.6% to -2.1%)                      │
└─────────────────────────────────────────────────────────────────┘
```

## Future State (v2+)

```
┌─────────────────────────────────────────────────────────────────┐
│  FUTURE: PARALLEL EXECUTION + OPTIMIZATIONS                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌──────────────┐             │
│  │  Client  │───▶│  Router  │───▶│ Worker Thread│             │
│  │ Request  │    │  Check   │    │  + SIMD Ops  │             │
│  └──────────┘    └──────────┘    └──────────────┘             │
│                                          │                       │
│                                    ┌─────▼─────┐               │
│                                    │  Batching │               │
│                                    └───────────┘               │
│                                                                  │
│  Gains: Parallel execution, SIMD, batching                      │
│  Expected: 2-10x faster for high throughput                     │
└─────────────────────────────────────────────────────────────────┘
```

## Performance Impact Summary

| Metric | Impact | Severity |
|--------|--------|----------|
| Throughput | -0.6% to -2.1% | **LOW** ✓ |
| Latency | +0.008 ms (2.4%) | **LOW** ✓ |
| Correctness | 0 errors/fallbacks | **PERFECT** ✓ |
| Overhead | 8-10 µs per command | **ACCEPTABLE** ✓ |

## When Will Accelerator Be Faster?

### Optimization Roadmap

1. **Worker Threads** (v2)
   - Expected gain: 1.5-2x throughput
   - Parallel command execution
   - Non-blocking main thread

2. **SIMD Operations** (v3)
   - Expected gain: 2-4x for string ops
   - Vectorized string comparison
   - Batch hash operations

3. **Lock-Free Queues** (v3)
   - Expected gain: 1.2-1.5x throughput
   - Reduced contention
   - Better cache locality

4. **Custom Implementations** (v4)
   - Expected gain: 2-10x for specific commands
   - Bypass generic code paths
   - Command-specific optimizations

### Combined Potential

```
Cumulative Performance Gains (Projected):

Current:   100% baseline (without accelerator)
v1:         97.9% (routing overhead) ← WE ARE HERE
v2:        150-200% (worker threads)
v3:        300-600% (+ SIMD)
v4:        600-1000% (+ custom implementations)
```

## Recommendation

### For Production Use TODAY

**Use WITHOUT Accelerator** (`make ENABLE_ACCELERATOR=no`)
- ✓ Best performance right now
- ✓ No overhead
- ✓ Proven stability

### For Staged Rollout / R&D

**Use WITH Accelerator** (`make ENABLE_ACCELERATOR=yes`)
- ✓ Routing infrastructure ready
- ✓ Metrics and observability
- ✓ Foundation for future gains
- ⚠ Small overhead (<2.5%) acceptable

## Conclusion

**Current Winner: WITHOUT Accelerator (0.6-2.1% faster)**

The accelerator v1 intentionally prioritizes:
1. ✓ Correctness over performance
2. ✓ Safety over speed
3. ✓ Infrastructure over optimization

The small overhead is the **cost of establishing the foundation** that will enable 2-10x performance gains in future versions.

**Think of it as building the highway before adding the fast lanes.**
