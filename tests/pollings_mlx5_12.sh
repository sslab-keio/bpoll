#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run=${USCHED_RUN_DIR:-"$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/pollings-mlx5-12"}
mkdir -p "$run"

for hook in yield bpoll; do
	case "$hook" in
		yield) preload="$root/src/libyield.so" ;;
		bpoll) preload="$root/src/libbpoll.so" ;;
	esac

	USCHED_TEST_NAME="pollings-mlx5-12-$hook" \
	USCHED_HOOK_NAME="$hook" \
	USCHED_PRELOAD="$preload" \
	USCHED_RUN_DIR="$run/$hook" \
	USCHED_SERVER_CORE=13 \
	USCHED_DURATION=15 \
	USCHED_CONNECTIONS=3 \
	USCHED_RATE=0 \
	USCHED_SYNC_CLIENTS=1 \
	USCHED_WAIT_PING=1 \
	USCHED_CHECK_PERFORMANCE=0 \
	sh "$root/src/tests/quick_mlx5_many.sh" 12
done

printf 'hook,pps,p50_ns,p99_ns,p999_ns,result\n' >"$run/comparison.csv"
for hook in yield bpoll; do
	awk -F, 'NR == 2 { print $2 "," $10 "," $11 "," $12 "," $13 "," FILENAME }' \
		"$run/$hook/summary.csv" >>"$run/comparison.csv"
done

cat "$run/comparison.csv"
