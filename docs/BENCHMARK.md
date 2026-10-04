# Benchmark method and observed run

## Reproduce

```sh
make all test sanitize
./build/nanobook-bench --events 5000000 --rounds 5 --json
./scripts/download-sample.sh
./build/nanobook-bench --lobster data/AMZN_2012-06-21_34200000_57600000_message_1.csv --rounds 20 --json
./build/nanobook-exchange-bench --events 1000000 --warmup 50000 --symbols 256 --shards 4
./build/nanobook-exchange-bench --events 100000 --symbols 64 --shards 4 --rate 100000
EVENTS=500000 SYMBOLS=256 WARMUP=50000 ./scripts/benchmark-scaling.sh
```

The LOBSTER sample is identified by SHA-256 `9506cea0aab42b2815e13d2f2485b39ef6c0aa212d1bb68f344a52f0a24475f5`. The script verifies this before use. The sample is downloaded at run time; it is not redistributed in Git.

## What is timed

The synthetic command vector and LOBSTER CSV are prepared before timing. Each throughput round constructs a fresh book and, for synthetic mode, inserts 2,048 background orders before starting the clock. It then applies the events in one loop. Throughput is the median events per second across rounds. The timed loop includes command dispatch, replay counters, ID lookup, matching, and book mutation. It excludes parsing, command generation, construction, and invariant verification.

Latency is measured in a separate fresh-book run using a clock read immediately before and after each operation. Samples are grouped by operation, sorted, and reported using nearest-rank p50/p99/p99.9. `--sample-every N` can reduce timing overhead for very large traces. The harness also times 10,000 back-to-back clock pairs. It does not subtract clock overhead or claim that a `0 ns` sample is instantaneous.

Synthetic commands repeat an eight-event cycle: add bid, add ask, add a crossing bid, reduce a resting bid, reprice/increase it, cancel it, add an ask, cancel it. The 2,048 background orders are farther from the spread. The cycle exercises the matching and management code but represents a controlled, shallow workload. Throughput is a property of this trace and machine, not a general exchange capacity rating.

Replay mode uses `add_passive` for submissions and applies LOBSTER's recorded reductions/deletions. It measures feed-event processing, not matching throughput. The public level-1 sample starts without the prior complete book and omits deeper orders, so missing-ID events are expected and counted. Events of types 5, 6, and 7 are counted but do not change the visible book. This makes the measured feed-event rate especially unsuitable as a standalone engine-performance claim.

## One observed run

Recorded on 2026-09-21, arm64 macOS 27.0, Apple clang 21.0.0, C++20, `-O3 -DNDEBUG`. System load, thermal state, and processor frequency were not controlled.

| Workload | Events | Throughput, median round | Other observed facts |
| --- | ---: | ---: | --- |
| Synthetic matching | 5,000,000 | 30,081,446 events/sec | 0 rejected; 625,000 trades; 2,048 final background orders |
| LOBSTER AMZN level 1 replay | 57,515 | 95,355,018 feed events/sec | 7,467 missing-ID rejections; 2,445 ignored hidden/cross/halt events |

The observed minimum positive clock step was **41 ns**; the median back-to-back clock pair was **0 ns**. For the synthetic add subset, raw sampled p50/p99/p99.9 were **0 / 42 / 125 ns**. These single-operation values are quantized at roughly the same scale as the work, so they should be treated as timer-limited observations, not portable nanosecond latency guarantees. A more precise tail study would use suitable hardware timing support, CPU isolation/pinning where available, longer-running representative traces, and repeated trials across system conditions.

The raw command output is reproducible from the commands above. When citing this project on a résumé, identify the synthetic workload and use the measured throughput only if you can rerun it in your own environment; do not quote the screenshot's illustrative 2.1M orders/sec or 180 ns p99 as achieved results.

## Full pipeline benchmark

`nanobook-exchange-bench` measures a different boundary from the original
single-book microbenchmark. Its timed path starts before client request
encoding and includes binary decoding, sequence assignment, optional journal
writes/sync, symbol routing, SPSC handoff, matching, event handoff, response
encoding, and client-side response decoding. Construction, symbol generation,
warmup, percentile sorting, and invariant checking are outside the timed region.

The deterministic five-command-per-symbol workload performs a resting enter,
same-price size-reducing replace, cancel, resting sell, and crossing buy. It
therefore exercises every wire command and produces a trade every fifth
command. One ingress thread, one egress thread, and N matching workers model a
sequenced partitioned exchange. Sequence IDs correlate out-of-order shard
responses.

Saturated mode submits as quickly as possible. Its latency includes queueing
under that offered load, so it should be read with throughput. Open-loop mode
uses `--rate COMMANDS_PER_SECOND`, paces requests from an independent schedule,
and measures from each intended send time. When the process falls behind, that
lateness remains in the sample rather than being hidden by coordinated
omission. Latency uses `steady_clock`; no clock cost is subtracted.

The benchmark reports nearest-rank p50, p99, p99.9, p99.99, and maximum for all
commands and separately for enter, cancel, and replace. Queue-full retry counts
and per-shard accepted/processed/event totals make overload and skew visible.
`--journal PATH --sync-every N` measures buffered (`0`), per-command (`1`), or
batched (`N > 1`) persistence in the same path. Use a fresh journal for each
comparison.

### Observed scaling run

One run on 2026-10-04 used arm64 macOS 27.0, Apple clang 21.0.0, C++20
`-O3 -DNDEBUG`, 256 symbols, 50,000 warmup commands, and 500,000 measured
commands. CPU affinity, frequency, thermal state, and background load were not
controlled.

| Shards | Throughput (commands/sec) | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|
| 1 | 4,653,430 | 5.06 ms | 11.53 ms | 11.65 ms |
| 2 | 4,621,830 | 3.68 ms | 8.38 ms | 8.46 ms |
| 4 | 3,724,130 | 3.38 us | 141.67 us | 301.25 us |
| 8 | 3,393,380 | 4.79 us | 193.83 us | 234.88 us |

This run shows why both throughput and latency are required. One and two worker
configurations accumulated a long queue. Four workers kept matching close to
ingress and sharply reduced queueing, but command throughput did not scale
linearly because the single binary sequencer and egress became the limiting
stages; eight workers added scheduling and cache-coherence cost. These are
observations from one synthetic run, not portable capacity claims. Repeat runs,
pinning, hardware performance counters, realistic symbol skew, and a real
network load generator are needed before drawing production conclusions.
