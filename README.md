# Nanobook

A C++20 exchange pipeline built around a preallocated limit order book. It now includes an OUCH-inspired binary TCP gateway, a checksummed write-ahead journal with deterministic recovery, per-symbol multicore sharding, and an end-to-end benchmark that reports throughput and p50/p99/p99.9/p99.99 latency. The original single-book harness and LOBSTER market-data replay remain available.

## Quick start

Requires a C++20 compiler and `make` (no runtime dependencies).

```sh
make all test
./build/nanobook-bench --events 500000 --rounds 5
./build/nanobook-bench --events 500000 --rounds 5 --json
./build/nanobook-gateway --port 9001 --symbol AAPL --journal orders.nbj --sync-every 1
./build/nanobook-exchange-bench --events 1000000 --symbols 256 --shards 4
EVENTS=1000000 ./scripts/benchmark-scaling.sh
```

To replay a public LOBSTER sample:

```sh
./scripts/download-sample.sh
./build/nanobook-bench --lobster data/AMZN_2012-06-21_34200000_57600000_message_1.csv --rounds 5
```

The download script fetches a [public mirror of LOBSTER's AMZN level-1 sample](https://huggingface.co/datasets/totalorganfailure/lobster-data/tree/main/LOBSTER_SampleFile_AMZN_2012-06-21_1), verifies its SHA-256 checksum, and keeps it outside Git. You can instead download a message file from [LOBSTER's official sample page](https://data.lobsterdata.com/info/DataSamples.php) and pass its path to `--lobster`. The [official format description](https://data.lobsterdata.com/info/DataStructure.php) defines the six CSV fields and event types.

## What it does

- **Matching mode:** A new buy matches the cheapest available sell at or below its limit; a new sell matches the highest buy at or above its limit. At one price, earlier resting orders fill first. Trades execute at the resting order's price. Any remainder rests on the book.
- **Order management:** Cancel removes an order; reduce preserves its place in the queue. A same-price modify that only decreases quantity preserves priority. Increasing quantity or changing price removes and re-enters the order, losing priority and potentially matching immediately.
- **Replay mode:** Type 1 inserts a visible order without matching; types 2 and 4 reduce it; type 3 deletes it. Hidden executions, cross trades, and trading-halt indicators (types 5, 6, 7) leave the visible book unchanged. Unknown IDs and other rejected events are counted.
- **Measurement:** The harness loads or generates events before timing, measures throughput in separate runs, then samples individual operation latency and reports p50, p99, and p99.9. It also reports a clock baseline and the minimum observed positive clock step.
- **Binary order entry:** A versioned, big-endian, fixed-width protocol accepts enter, cancel, and replace commands over fragmented TCP streams. Every command receives a monotonic ingress sequence and explicit execution or rejection events. See [protocol v1](docs/PROTOCOL.md).
- **Recovery:** A write-ahead decorator appends each sequenced command before it reaches the book. Fixed-size CRC32 records, monotonic sequence validation, and crash-tail repair make restart behavior deterministic. Buffered, batched, and per-command `fsync` policies make the durability/latency tradeoff explicit. See [recovery design](docs/RECOVERY.md).
- **Multicore execution:** Symbols are assigned to single-owner worker shards. Preallocated SPSC queues pass commands and events without locks or hot-path allocation, and bounded queues expose backpressure instead of hiding it with unbounded memory growth. See [sharding design](docs/SHARDING.md).
- **Pipeline benchmark:** The benchmark includes request encoding/decoding, routing, queueing, matching, and response encoding/decoding. Saturated and open-loop modes report operation-specific tail latency, throughput, and shard pressure. Journaling can be enabled to measure its real cost.

The engine preallocates its order slots, price levels, occupancy bitsets, and ID table at construction. Core order operations perform no heap allocation after construction when called without a user callback; a test counts allocations while exercising them. A caller-provided callback may allocate. Prices are integer units of $0.0001. The configured price range and order capacity are fixed for each book instance.

## Important limits

This is a research and portfolio engine, not exchange software. Each individual book is deliberately single-writer; parallelism comes from independent symbol shards. The TCP example server processes one connection at a time, and the multicore path is currently exercised through the library and benchmark rather than a production session layer. There are no pre-trade risk checks, auction rules, authentication, replication, snapshots, or failover. Trade callbacks must not mutate the same book while an operation is in progress.

Journal replay assumes the same engine behavior, symbol table, and book configuration. It detects corrupt records and incomplete crash tails, but it does not replicate the log or resolve an uncertain storage failure. Recovery is O(number of recorded commands) until snapshots are added.

LOBSTER's level-1 sample begins mid-session and includes only events relevant to a limited price range. It does not contain every earlier or deeper order needed to reconstruct a full book from zero. Replay therefore measures how this implementation handles the **real message stream**, but its final book is not claimed to equal LOBSTER's published snapshots. The program prints the missing-ID count so this limitation is visible. More complete message data or an initialization source would be needed for exact reconstruction.

The synthetic trace is deterministic, starts with 2,048 resting orders by default, and cycles through adds, matching, reductions, modifies, and cancels. It is intentionally a controlled workload, not a claim about production exchange traffic. Override the preloaded depth with `--seed-orders N` or the order capacity with `--capacity N`.

Individual nanosecond results are limited by the host clock's granularity. A reported `0 ns` means the two clock reads landed in the same clock tick; it is **not** proof of zero latency. Compare runs only with the same compiler, hardware, workload, settings, and system conditions. See [benchmark methodology](docs/BENCHMARK.md) and [engine design](docs/DESIGN.md).

## API sketch

```cpp
#include "nanobook/order_book.hpp"

nanobook::OrderBook book({500000, 2500000, 100, 100000});
book.add(1, nanobook::Side::sell, 1000000, 100);  // $100.00
auto result = book.add(2, nanobook::Side::buy, 1000000, 40);
// result.executed_quantity == 40; best ask now has 60 shares.
```

## Project layout

| Path | Purpose |
| --- | --- |
| `include/nanobook/order_book.hpp`, `src/order_book.cpp` | Matching engine and data structures |
| `include/nanobook/feed.hpp`, `src/feed.cpp` | LOBSTER CSV parser and replay mapping |
| `src/bench.cpp` | Synthetic and real-feed benchmark harness |
| `protocol.hpp`, `gateway.cpp`, `gateway_server.cpp` | Binary framing, command adapter, and loopback TCP server |
| `journal.hpp`, `journal.cpp` | Write-ahead log, durability policies, validation, and replay |
| `sharded_exchange.hpp`, `sharded_exchange.cpp` | Symbol routing, SPSC queues, and worker ownership |
| `src/exchange_bench.cpp` | Full pipeline saturation/open-loop benchmark |
| `tests/test.cpp` | FIFO tests, replay tests, randomized reference comparison, allocation checks |
| `tests/test_gateway.cpp`, `test_journal.cpp`, `test_sharded.cpp` | Fragmentation, recovery, corruption, routing, and concurrency tests |
| `docs/DESIGN.md` | Data structure choices and semantics |
| `docs/BENCHMARK.md` | Measurement method, observed results, and caveats |

## License

MIT; see [LICENSE](LICENSE). The downloaded LOBSTER sample is not part of this repository.
