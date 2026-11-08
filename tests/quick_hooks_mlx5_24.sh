#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run="$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/quick-hooks-mlx5-24"
mkdir -p "$run"

for hook in epoll yield bpoll; do
	case "$hook" in
		epoll) preload="$root/src/libepoll.so" ;;
		yield) preload="$root/src/libyield.so" ;;
		bpoll) preload="$root/src/libbpoll.so" ;;
	esac

	USCHED_HOOK_NAME="$hook" \
	USCHED_PRELOAD="$preload" \
	USCHED_RUN_DIR="$run/$hook" \
	USCHED_CHECK_PERFORMANCE=0 \
	sh "$root/src/tests/quick_mlx5_many.sh" 24
done

printf 'hook,pps,p50_ns,p99_ns,p999_ns,result\n' >"$run/comparison.csv"
for hook in epoll yield bpoll; do
	awk -F, 'NR == 2 { print $2 "," $9 "," $10 "," $11 "," $12 "," FILENAME }' \
		"$run/$hook/summary.csv" >>"$run/comparison.csv"
done

cat "$run/comparison.csv"
