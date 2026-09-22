# Benchmark method and observed run

## Reproduce

```sh
make all test sanitize
./build/nanobook-bench --events 5000000 --rounds 5 --json
./scripts/download-sample.sh
./build/nanobook-bench --lobster data/AMZN_2012-06-21_34200000_57600000_message_1.csv --rounds 20 --json
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
