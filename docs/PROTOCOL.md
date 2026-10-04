# Nanobook Order Entry Protocol v1

Nanobook uses an OUCH-inspired binary protocol. It is intentionally small and
is not wire-compatible with Nasdaq OUCH.

## Framing

Every message begins with a four-byte header:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 1 | protocol version (`1`) |
| 1 | 1 | ASCII message type |
| 2 | 2 | big-endian payload length |

All integer fields are big-endian. Symbols are eight ASCII bytes padded on the
right with spaces. Prices are signed 64-bit integers in units of `$0.0001`.

## Commands

| Type | Name | Payload |
|---|---|---|
| `O` | enter | symbol(8), order ID(8), side(1), price(8), quantity(4) |
| `X` | cancel | symbol(8), order ID(8) |
| `U` | replace | symbol(8), order ID(8), price(8), quantity(4) |

Side is `0` for buy and `1` for sell. The gateway assigns a monotonically
increasing 64-bit ingress sequence to every decoded command or rejected frame.

## Events

The response types are accepted (`A`), canceled (`C`), replaced (`R`), executed
(`E`), and rejected (`J`). Every event has the same 49-byte payload:

`sequence(8), symbol(8), order ID(8), resting ID(8), price(8), quantity(4), remaining(4), codes(1)`

The low nibble of `codes` is the matching-engine status. The high nibble is the
protocol error. A crossing enter or replace emits one execution per resting
fill, followed by a terminal accepted or replaced event containing aggregate
executed and remaining quantities. This order permits the matching engine to
stream an unbounded number of fills without allocating a temporary response
buffer.

The streaming decoder accepts arbitrary TCP fragmentation and multiple frames
in one read. Its storage is fixed at 64 bytes, so malformed lengths cannot
force a heap allocation.

## Running the gateway

```sh
make build/nanobook-gateway
./build/nanobook-gateway --port 9001 --symbol AAPL
```

Add `--journal orders.nbj` to recover the book from a write-ahead log on
startup. `--sync-every 0` uses buffered writes, `--sync-every 1` calls `fsync`
before every command is applied, and larger values sync in batches of that
size. The selected durability policy is part of any meaningful latency result.

The milestone-one server binds to loopback and processes one connection at a
time. It disables Nagle's algorithm and retains a global ingress sequence over
connection changes. The multicore milestone provides the asynchronous,
multi-symbol execution path.
