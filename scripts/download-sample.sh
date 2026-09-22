#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target="$root/data/AMZN_2012-06-21_34200000_57600000_message_1.csv"
source_url='https://huggingface.co/datasets/totalorganfailure/lobster-data/resolve/main/LOBSTER_SampleFile_AMZN_2012-06-21_1/AMZN_2012-06-21_34200000_57600000_message_1.csv?download=true'
expected='9506cea0aab42b2815e13d2f2485b39ef6c0aa212d1bb68f344a52f0a24475f5'

mkdir -p "$root/data"
temporary=$(mktemp "$root/data/.sample.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM
curl -L --fail --silent --show-error "$source_url" -o "$temporary"
actual=$(shasum -a 256 "$temporary" | cut -d ' ' -f 1)
if [ "$actual" != "$expected" ]; then
  echo "sample checksum mismatch; refusing to use changed data" >&2
  exit 1
fi
mv "$temporary" "$target"
echo "$target"
