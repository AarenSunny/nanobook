#!/bin/sh
set -eu

events="${EVENTS:-1000000}"
symbols="${SYMBOLS:-256}"
warmup="${WARMUP:-50000}"

make build/nanobook-exchange-bench >/dev/null
for shards in 1 2 4 8; do
    if [ "$shards" -le "$symbols" ]; then
        ./build/nanobook-exchange-bench \
            --events "$events" --warmup "$warmup" \
            --symbols "$symbols" --shards "$shards" --json
    fi
done
