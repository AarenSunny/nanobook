# Nanobook

A single-threaded C++20 limit order book matching engine with a reproducible benchmark harness. It supports limit-order adds, FIFO price-time matching, cancellations, partial reductions, and modifies. A second path replays LOBSTER message CSVs as recorded visible-book events.

## Quick start

Requires a C++20 compiler and `make` (no runtime dependencies).

```sh
make all test
./build/nanobook-bench --events 500000 --rounds 5
./build/nanobook-bench --events 500000 --rounds 5 --json
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

The engine preallocates its order slots, price levels, occupancy bitsets, and ID table at construction. Core order operations perform no heap allocation after construction when called without a user callback; a test counts allocations while exercising them. A caller-provided callback may allocate. Prices are integer units of $0.0001. The configured price range and order capacity are fixed for each book instance.

## Important limits

This is a research and portfolio engine, not exchange software. The core is single-threaded; it has no persistence, networking, risk checks, auction rules, or production fault tolerance. Trade callbacks must not mutate the same book while an operation is in progress.

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
| `tests/test.cpp` | FIFO tests, replay tests, randomized reference comparison, allocation checks |
| `docs/DESIGN.md` | Data structure choices and semantics |
| `docs/BENCHMARK.md` | Measurement method, observed results, and caveats |

## License

MIT; see [LICENSE](LICENSE). The downloaded LOBSTER sample is not part of this repository.
