# v3 Design: True Multi-Core Async Execution (10-100x Target)

## Executive Summary

v3 aims for 10-100x performance improvement through true multi-core parallelism. This requires fundamental architectural changes to Redis/Valkey's single-threaded model.

## Performance Target Analysis

### Current Performance (v2)
- ~78k req/s single command type
- Single-threaded execution
- Key locking overhead: ~1%

### Target Performance (v3)
- **10x improvement**: ~780k req/s = 8-core parallel execution
- **100x improvement**: ~7.8M req/s = requires batching + SIMD + zero-copy

### Feasibility Assessment

**Achievable with current Redis architecture:**
- ✅ 2-4x: Parallel execution with completion queue
- ✅ 5-10x: + Batching + optimized data paths
- ⚠️ 10-50x: + SIMD + custom command implementations
- ❌ 50-100x: Requires complete Redis rewrite (not feasible)

## v3 Architecture

### Design Pattern: Async Worker with Completion Queue

```
┌─────────────────────────────────────────────────────────────────┐
│                          MAIN THREAD                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Receive command                                             │
│  2. Enqueue to worker pool (non-blocking)                       │
│  3. Mark client as "pending"                                    │
│  4. Continue event loop                                          │
│  5. Process completion queue (send responses)                   │
│                                                                  │
└──────────────────┬──────────────────────────────────────────────┘
                   │
                   ├─ Command Queue ─→ WORKER THREAD 1 (String)
                   │                   │
                   │                   ├─ Acquire key lock
                   │                   ├─ Execute command logic
                   │                   ├─ Prepare response
                   │                   ├─ Release key lock
                   │                   └─ Enqueue to completion
                   │
                   ├─ Command Queue ─→ WORKER THREAD 2 (Hash)
                   │                   │
                   │                   └─ (same as above)
                   │
                   └─ Completion Queue ←─ All workers
```

### Key Components

#### 1. Command Execution Separation
```c
/* Separate command execution into 3 phases */

Phase 1: Validation (Main Thread)
- Parse command
- Validate arguments
- Route to worker

Phase 2: Execution (Worker Thread)
- Acquire key lock
- Execute command logic
- Build response data
- Release key lock

Phase 3: Response (Main Thread)
- Send response to client
- Update client state
- Free resources
```

#### 2. Completion Queue
```c
typedef struct completion_entry {
    client *c;               /* Client to respond to */
    sds response;            /* Pre-formatted response */
    int status;              /* Success/error */
    struct completion_entry *next;
} completion_entry;

/* Lock-free MPSC queue for completions */
static struct {
    completion_entry *head;
    completion_entry *tail;
    pthread_mutex_t lock;
} completion_queue;
```

#### 3. Response Batching
```c
/* Process multiple completions per event loop iteration */
void worker_process_completions(void) {
    int batch_size = 100;  /* Process up to 100 at once */
    
    for (int i = 0; i < batch_size; i++) {
        completion_entry *entry = dequeue_completion();
        if (!entry) break;
        
        /* Send response to client */
        addReplyBulkCBuffer(entry->c, entry->response, sdslen(entry->response));
        
        /* Cleanup */
        sdsfree(entry->response);
        zfree(entry);
    }
}
```

## Implementation Strategy

### Phase 1: Async Execution (2-4x improvement)
- Implement completion queue
- Workers execute in parallel
- Main thread sends responses
- **Expected**: 2-4x throughput on multi-core

### Phase 2: Optimized Data Paths (2x improvement)
- Custom fast paths for GET/SET/INCR
- Bypass generic command infrastructure
- Direct dict access
- **Expected**: 2x additional improvement

### Phase 3: SIMD Operations (2-3x improvement)
- Vectorized string operations
- Batch memory copies
- SIMD hash calculations
- **Expected**: 2-3x additional improvement

### Phase 4: Zero-Copy & Batching (1.5-2x improvement)
- Batch multiple commands
- Zero-copy response building
- Memory pool allocation
- **Expected**: 1.5-2x additional improvement

### Combined Expected Performance
- Phase 1: 4x
- Phase 1+2: 8x
- Phase 1+2+3: 24x
- Phase 1+2+3+4: 48x

**Realistic target: 10-50x improvement**

## Critical Challenges

### Challenge 1: Redis Data Structure Thread Safety

**Problem**: Redis dict, sds, robj are not thread-safe

**Solution Options**:
1. **Per-key locking** (current) - Only safe for independent keys
2. **Copy-on-write** - Workers get snapshots
3. **Lock-free data structures** - Replace core data structures (massive change)

**v3 Approach**: Use per-key locks + validate no other global state modification

### Challenge 2: Memory Management

**Problem**: Redis memory allocator (jemalloc) may have contention

**Solution**:
- Thread-local memory pools
- Batch allocations
- Pre-allocated response buffers

### Challenge 3: Client State Management

**Problem**: Client structure accessed from multiple threads

**Solution**:
- Mark clients as "pending" (atomic flag)
- Only main thread modifies client state
- Workers read client data, write to completion queue

## Implementation Plan

### Step 1: Refactor call() Function
```c
/* Split call() into execution and response */
typedef struct command_result {
    int status;
    sds response;
    int error_code;
} command_result;

command_result *execute_command_logic(client *c) {
    /* Execute without sending response */
    /* Return result data */
}
```

### Step 2: Implement Completion Queue
```c
/* Add to event loop */
void beforeSleep(struct aeEventLoop *eventLoop) {
    /* Process completions */
    worker_process_completions();
}
```

### Step 3: Update Worker Threads
```c
void *worker_thread_main(void *arg) {
    while (1) {
        client *c = dequeue_command();
        
        /* Execute */
        command_result *result = execute_command_logic(c);
        
        /* Enqueue completion */
        enqueue_completion(c, result);
    }
}
```

### Step 4: Custom Fast Paths

```c
/* Optimized GET */
command_result *fast_get(client *c, sds key) {
    robj *val = dictFetchValue(c->db->dict, key);
    if (!val) return error_result();
    return string_result(val);
}

/* Optimized SET */
command_result *fast_set(client *c, sds key, sds value) {
    robj *val = createStringObject(value, sdslen(value));
    dictReplace(c->db->dict, key, val);
    return ok_result();
}
```

## Performance Projections

### Scenario 1: Independent Keys (Best Case)
- 8 worker threads
- No lock contention
- **Expected**: 8-10x improvement

### Scenario 2: Mixed Workload
- 70% reads, 30% writes
- Some key contention
- **Expected**: 5-7x improvement

### Scenario 3: High Contention
- Many operations on same keys
- Lock wait time
- **Expected**: 2-3x improvement

## Risks & Mitigation

### Risk 1: Correctness
- **Risk**: Data races, race conditions
- **Mitigation**: Extensive testing, ThreadSanitizer, formal verification

### Risk 2: Complexity
- **Risk**: Hard to debug, maintain
- **Mitigation**: Comprehensive documentation, logging, metrics

### Risk 3: Redis Compatibility
- **Risk**: Breaking Redis semantics
- **Mitigation**: Fallback to sync mode, extensive testing

## Testing Strategy

### Load Testing
- Single key operations
- Multiple independent keys
- High contention scenarios
- Mixed read/write workloads

### Correctness Testing
- Concurrent access patterns
- Race condition detection
- Memory leak detection
- Semantic equivalence tests

### Performance Testing
- Throughput benchmarks
- Latency percentiles (p50, p99, p999)
- CPU utilization
- Lock contention metrics

## Timeline Estimate

### Minimal v3 (Async only): 1-2 weeks
- Completion queue
- Basic async execution
- **Target**: 2-4x improvement

### Full v3 (All optimizations): 1-2 months
- All phases implemented
- Extensive testing
- **Target**: 10-50x improvement

## Recommendation

**For 10x improvement target**: Implement Phases 1+2
- Achievable within Redis architecture constraints
- Reasonable development time
- Production-ready path

**For 100x improvement target**: Requires Redis fork
- Complete rewrite of core data structures
- Multi-year effort
- Out of scope for this project

## Next Steps

1. Implement minimal v3 with completion queue
2. Measure actual performance gains
3. Iterate on optimizations based on profiling
4. Add custom fast paths for high-impact commands

---

**Bottom Line**: 10x improvement is achievable with v3. 100x would require a fundamentally different architecture (essentially a new database).
