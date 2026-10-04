# Multicore sharding

`ShardedExchange` assigns every configured symbol to one worker shard with a
stable FNV-1a hash. A book never moves while the process is running, so exactly
one thread mutates each `OrderBook` and the matching hot path needs no lock.

```text
one ingress sequencer
        |
        +-- bounded SPSC queue --> shard 0 --> bounded event queue --+
        +-- bounded SPSC queue --> shard 1 --> bounded event queue --+--> one egress
        +-- bounded SPSC queue --> shard N --> bounded event queue --+
```

Symbols and routes are built before worker threads start. Routing is a binary
search over a sorted table and passes a precomputed book index to the worker.
Neither queue operation allocates. Head and tail counters occupy separate cache
lines to avoid false sharing.

## Ordering

Commands for one symbol are processed in ingress order because they always use
the same queue. Events from different shards may arrive in a different order.
Every event retains the global ingress sequence, allowing an egress sequencer or
client to reorder them when global presentation order is required.

## Backpressure and shutdown

The queues are deliberately bounded. `try_submit` returns `queue_full` instead
of allocating or blocking the ingress thread. A full event queue pauses its
worker until egress catches up. During shutdown, a worker may discard events
only to avoid deadlocking on an abandoned full output queue; the
`dropped_on_shutdown` counter makes that condition visible.

The API has one producer and one consumer by design. Multiple network readers
must feed a single sequencer rather than calling `try_submit` concurrently.
Per-shard counters reveal queue pressure and symbol skew. Static hashing is
simple and preserves ownership; a production deployment would choose the
symbol-to-shard table from measured traffic because a single hot symbol cannot
be parallelized without changing price-time semantics.

## Recovery

Before `start()`, the exchange also implements the synchronous
`CommandProcessor` interface. This lets journal recovery rebuild all books with
the same symbol routing before worker threads take ownership.
