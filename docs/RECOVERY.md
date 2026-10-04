# Journal and deterministic recovery

`JournaledProcessor` is a write-ahead decorator around the command interface.
For every decoded command it writes a complete record before invoking the
matching processor. Invalid engine commands are recorded too, so replay repeats
the same rejection and preserves later sequence order.

## Record format

Records are fixed at 49 bytes:

| Field | Bytes |
|---|---:|
| magic (`NBJ1`) | 4 |
| record length | 2 |
| journal version | 1 |
| command type | 1 |
| ingress sequence | 8 |
| symbol | 8 |
| order ID | 8 |
| side | 1 |
| price | 8 |
| quantity | 4 |
| CRC32 | 4 |

Fixed records allow recovery to identify an incomplete final write without
searching for delimiters. Recovery verifies magic, length, version, command
fields, checksum, and strictly increasing sequence numbers before applying a
record. A partial final record is treated as a crash tail and is truncated when
the writer reopens the journal. A damaged complete record fails recovery.

## Durability choices

| Mode | Acknowledgement meaning | Cost |
|---|---|---|
| buffered | `write` succeeded into the operating system | lowest; recent commands may be lost on host failure |
| batch | every Nth command waits for `fsync` | bounded batches; earlier acknowledgements in the batch are not yet durable |
| every command | each command waits for `fsync` before matching | strongest local guarantee; storage latency is on the request path |

An `fsync` failure is an uncertain commit: the record may or may not survive.
The writer becomes unhealthy and stops applying new commands. A production
supervisor should stop the process and reconcile the last sequence with the
client after recovery.

The journal stores commands rather than memory images, so it remains easy to
inspect and replay but startup is linear in history length. Periodic snapshots,
an engine/configuration manifest, replicated consensus, and log rotation are
the natural next persistence steps.
