# Accelerated Execution Layer

## Overview

The Accelerated Execution Layer is a **minimal-touch, surgical optimization** for Valkey that provides a selective, opt-in routing mechanism for specific commands. It enables explicitly whitelisted commands to be routed through a custom execution path while maintaining 100% Redis/Valkey semantics.

## Design Principles

1. **Correctness First**: Redis/Valkey is the contract authority, default execution path is the ground truth
2. **Minimal Changes**: Only the control plane is modified, data structures remain untouched
3. **Opt-In**: Only explicitly enabled commands use the accelerated path
4. **Reversible**: Easy to disable and fallback to legacy behavior
5. **Observable**: Full metrics and visibility into routing decisions

## Architecture

```
Client Request
      ↓
processCommand()   ← validation + routing only
      ↓
      ├─→ if (accelerated command)
      │        → command_entrypoint()
      │            → execute in main thread (v1)
      │
      └─→ else
               → legacy call(cmd->proc)
```

## Files

### Added
- `src/router/entrypoint.h` - Router interface definitions
- `src/router/entrypoint.c` - Command routing implementation
- `src/executor/worker.h` - Worker thread interface (for future use)
- `src/executor/worker.c` - Worker thread implementation (for future use)

### Modified
- `src/server.c` - Added routing logic to `processCommand()`, initialization/shutdown, INFO metrics
- `src/Makefile` - Added new object files and `ENABLE_ACCELERATOR` build flag

## Whitelisted Commands

Currently, the following commands are routed through the accelerated path:

- `GET` - Read a string value
- `SET` - Write a string value
- `INCR` - Increment a counter
- `HGET` - Get hash field
- `HSET` - Set hash field

## Building

### With Accelerator Enabled (Default)

```bash
make
```

or explicitly:

```bash
make ENABLE_ACCELERATOR=yes
```

### With Accelerator Disabled

```bash
make ENABLE_ACCELERATOR=no
```

## Metrics

When the accelerator is enabled, additional metrics are available via the `INFO stats` command:

```
127.0.0.1:6379> INFO stats
...
accelerated_commands_total:400002
legacy_commands_total:3
fallback_invocations:0
...
```

- `accelerated_commands_total`: Number of commands routed through accelerated path
- `legacy_commands_total`: Number of commands using legacy path
- `fallback_invocations`: Number of times accelerated path failed and fell back to legacy

## Testing

### Functional Testing

```bash
# Start server
./src/valkey-server

# Test accelerated commands
./src/valkey-cli SET mykey "hello"
./src/valkey-cli GET mykey
./src/valkey-cli INCR counter
./src/valkey-cli HSET myhash field1 "value1"
./src/valkey-cli HGET myhash field1

# Check metrics
./src/valkey-cli INFO stats | grep accelerated
```

### Unit Tests

```bash
# Run string tests (SET, GET, INCR)
./runtest --single unit/type/string

# Run hash tests (HSET, HGET)
./runtest --single unit/type/hash
```

### Benchmarking

```bash
# Benchmark accelerated commands
./src/valkey-benchmark -t get,set,incr,hset,hget -n 100000 -q
```

See [BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md) for detailed performance analysis.

## Performance Impact

Based on benchmarking with 100,000 operations per command:

- **Throughput**: < 2.5% overhead
- **Latency**: Similar p50 latencies (~0.33ms)
- **Correctness**: 400,000+ commands with zero fallbacks

## Current Implementation (v1)

The current implementation executes commands **synchronously in the main thread**, identical to the legacy path. This ensures:

✅ 100% correctness and Redis semantics  
✅ No threading issues  
✅ No data races  
✅ Easy to test and debug  

The routing infrastructure is in place for future optimizations.

## Future Enhancements

The accelerator provides a foundation for future optimizations:

1. **Worker Thread Execution**: Move command execution to dedicated worker threads
2. **SIMD Optimizations**: Implement SIMD-accelerated command handlers
3. **Batch Processing**: Process multiple commands in batches
4. **Custom Implementations**: Optimized implementations for specific commands
5. **Adaptive Routing**: Dynamically route based on workload patterns
6. **Extended Whitelist**: Add more commands to the accelerated path

## Safety and Fallback

The accelerator includes multiple safety mechanisms:

1. **Compile-Time Toggle**: Can be completely disabled at build time
2. **Runtime Fallback**: Any routing failure automatically falls back to legacy path
3. **Metrics Tracking**: Full visibility into fallback invocations
4. **Zero Data Structure Changes**: No modifications to core data structures

## Non-Goals (v1)

The following are explicitly **not** implemented in v1:

- ❌ Lua script acceleration
- ❌ Multi-key atomic commands
- ❌ Replication/AOF/RDB changes
- ❌ Cluster modifications
- ❌ GPU offload
- ❌ Mass command rewrites

## Debugging

### Enable Debug Logging

Look for accelerator-related messages in the server log:

```
* Accelerator enabled (routing mode)
* Server initialized
```

### Check Routing Decisions

Use INFO stats to verify routing behavior:

```bash
./src/valkey-cli INFO stats | grep -E "accelerated|legacy|fallback"
```

### Disable Accelerator

If issues arise, rebuild without the accelerator:

```bash
make clean
make ENABLE_ACCELERATOR=no
```

## Contributing

When adding new commands to the whitelist:

1. Add command name to `accelerated_command_names[]` in `src/router/entrypoint.c`
2. Verify command works correctly with functional tests
3. Run benchmarks to measure performance impact
4. Update this README with the new command

## License

Same as Valkey/Redis - BSD 3-Clause License

## Acknowledgments

This implementation follows the design principles outlined in the original Epic specification, prioritizing correctness, minimal changes, and reversibility.
