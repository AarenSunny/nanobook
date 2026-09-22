# Engine design

## Representation

At construction, `OrderBook` allocates two arrays of price levels (bids and asks), each covering a configured integer price ladder. A level contains the first and last order slot plus aggregate quantity and order count. Each order slot has links to its predecessor and successor, giving a FIFO intrusive list at its price. Removing a known order takes constant time without allocating or searching the queue.

An open-addressed, fixed-capacity ID table maps exchange order IDs to slot indices. Deletion shifts later entries back when needed to preserve their probe paths, avoiding an accumulation of tombstones during long add/cancel workloads. A free list recycles order slots. Both structures are sized before any orders arrive. Occupancy bitsets identify which price levels contain orders. The current best bid and ask indices are cached; when a best level empties, the bitset finds the next occupied level.

The fixed ladder favors predictable memory use and direct indexing over arbitrary price range flexibility. An out-of-range or off-tick order is rejected. A book's capacity is also fixed. These are explicit configuration constraints, not silent resizing. IDs are nonzero and must be unique among resting orders.

When the order pool is full, an incoming order is admitted only if the book already contains enough opposite-side quantity at crossing prices to fill it completely. This precheck prevents an incoming order from partly executing and then failing to rest its remainder because no slot is available.

## Matching

For each incoming limit order, the engine checks the best price on the opposite side. It stops when no opposite order exists or the best price is outside the incoming limit. Otherwise it fills the head of that price level, invokes an optional caller callback, updates quantities, and removes the resting order if fully filled. The engine itself does not allocate for this callback; caller code may. The loop repeats until the incoming order is filled or cannot cross. Remaining shares join the tail at the incoming price. Executions use the resting price.

This gives price priority from the best opposite level and time priority from FIFO within that level. `cancel` uses the ID index to unlink an order directly. `reduce` leaves its queue position unchanged. `modify` leaves priority unchanged for a same-price decrease; a price change or size increase cancels and re-enters the order. A modify can therefore execute against opposite liquidity.

The engine assumes one thread owns a book. It does not synchronize callbacks; a callback may inspect a trade but must not re-enter or mutate the book during matching.

## Feed replay is separate from matching

LOBSTER message files describe changes that already occurred on an exchange. Feeding their type-1 submissions through `add()` would manufacture new executions and corrupt the reconstruction. `add_passive()` instead inserts the visible resting order. Type-2 cancellations and type-4 visible executions reduce the recorded resting order; type-3 deletions remove it. Types 5, 6, and 7 are counted but do not alter this visible book. This mapping follows [LOBSTER's own field definitions](https://data.lobsterdata.com/info/DataStructure.php).

A limited-depth LOBSTER sample is not a full historical order log. It begins without the prior order state and drops orders outside its selected range. Missing ID operations are counted, and the resulting book is not treated as an exact exchange snapshot. The benchmark reports event-processing cost, not fidelity to the supplied orderbook file.

## Complexity

| Operation | Typical work | Worst case |
| --- | --- | --- |
| ID lookup | Expected O(1) | O(capacity) with severe collisions/tombstones |
| Add without match | Expected O(1) | ID lookup worst case |
| Cancel/reduce | Expected O(1) | ID lookup worst case; best-level removal scans occupancy words |
| Add with matches | Proportional to resting orders filled | Same, plus next-best scans |
| Best quote | O(1) | O(1), cached |

The fixed hash table does not resize. This avoids allocations in the hot path, but its probe length can grow with a high load factor or adversarial collisions. It is sized to at least twice `max_orders`. Deletion may shift entries in a probe cluster, so worst-case cancel work is not strictly constant.

## Correctness checks

The test suite checks FIFO fills and resting-price execution, aggregate quantities, cancel/reduce/modify semantics, invalid operations, LOBSTER event mapping, and capacity handling. A 5,000-step deterministic randomized run compares each operation, trade sequence, best quote, and resting order count with a simple reference model. `check_invariants()` also walks every queue and checks links, quantities, bitsets, cached best prices, and ID lookup consistency. A separate test counts heap allocations during core operations. Address and undefined-behavior sanitizers run with `make sanitize`.
