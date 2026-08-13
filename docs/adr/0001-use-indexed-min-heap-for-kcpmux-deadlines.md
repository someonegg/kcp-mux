---
status: accepted
date: 2026-08-10
---

# Use an indexed binary min-heap for kcpmux deadlines

## Context and Problem Statement

Before this decision, kcpmux woke on a fixed 10 ms cadence and repeatedly scanned every connection and stream to discover due work. Besides making timer processing proportional to all live objects, deadline queries mutated timestamps and omitted some timeout conditions, so deadlines could drift or be observed late. The scheduler needs predictable incremental performance while preserving the host timer callback and the opaque `kcpmux_kcp_ops_s` interface used by multiple KCP implementations.

## Decision Drivers

- Timer dispatch must be proportional to due work rather than all live connections and streams.
- Connection and stream deadlines must support arbitrary insertion, adjustment, and removal without hierarchical scans.
- The scheduler must promptly represent a deadline equal to `now` and must stop at the first future deadline.
- The design cannot add requirements to `kcpmux_kcp_ops_s` or depend on KCP implementation internals.
- Automatic object finalization and callback reentrancy must not leave stale scheduler entries.

## Considered Options

- Keep the fixed-period engine scan and correct only the deadline calculations.
- Use a hierarchical scheduler in which each connection aggregates its streams' earliest deadline.
- Use a radix-oriented queue optimized for batches of equal or nearby deadlines.
- Use one engine-wide indexed binary min-heap with independent connection and stream timer nodes.

## Decision Outcome

Chosen option: "one engine-wide indexed binary min-heap with independent connection and stream timer nodes", because it provides predictable arbitrary rescheduling and removal in `O(log N)`, lets dispatch stop at the first future root, and avoids both unbounded connection-to-stream scans and assumptions about synchronized KCP deadlines.

Each live schedulable connection or stream owns one intrusive timer node. `kcpmux_engine_update()` collects a snapshot of nodes whose absolute deadline is at or before `now`, invokes each node's internal timeout callback at most once in that update, and then rearms the host timer from the heap root. A callback that reschedules itself at or before `now` is processed through a subsequent `set_timer(0)` wakeup, preventing a single update call from spinning forever.

### Positive Consequences

- Updating one object's deadline does not require scanning its siblings or parent hierarchy.
- Dispatch touches only due nodes plus heap maintenance and terminates at the first future deadline.
- Connection control, keepalive, idle, and stream KCP deadlines can be maintained independently as absolute times.
- Indexed removal prevents stale heap entries from accumulating when objects enter a terminal state and are automatically removed from ownership maps.
- The design uses only the existing KCP `update` and `check` operations.

### Negative Consequences

- Every schedulable connection and stream carries an intrusive node and heap index.
- Creation may need heap-capacity allocation before the object becomes externally visible, with explicit OOM rollback.
- Terminal finalization must cancel and unregister the timer node immediately, while an operation-scoped pending-release barrier defers physical reclamation until callback, KCP, and due-list dispatch stacks have returned.
- An active KCP implementation that continually returns a short deadline will still cause frequent callbacks for that stream; the scheduler cannot safely infer that an opaque KCP session is idle.
- Heap operations are `O(log N)` rather than the potential amortized bounds of a specialized radix structure.

## Pros and Cons of the Options

### Fixed-period engine scan with corrected deadlines

- Good: Smallest code change and no additional scheduler storage.
- Bad: Update and rearm remain `O(N)` even when no object is due, and the fixed wake cadence still adds latency or unnecessary wakeups.

### Hierarchical connection and stream scheduler

- Good: Keeps stream ownership aligned with the existing connection hierarchy.
- Bad: Maintaining a connection's aggregate minimum needs a second ordered structure or reintroduces an unbounded stream scan; dispatch also becomes coupled to hierarchy traversal.

### Radix-oriented deadline queue

- Good: Can be efficient for monotonic keys or large waves of identical deadlines.
- Bad: Arbitrary earlier/later rescheduling and deletion are more complex, while equal intervals do not establish equal absolute deadlines for independently active streams.

### Engine-wide indexed binary min-heap

- Good: Well-understood invariants, deterministic minimum lookup, and predictable insertion, adjustment, and removal for independently scheduled objects.
- Bad: Requires intrusive lifecycle state and logarithmic heap maintenance for every deadline change.
