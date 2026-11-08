#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run="$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/quick-mlx5-24-policies"
mkdir -p "$run"

run_case() {
	name=$1
	hook=$2
	rt_class=$3
	preload=$4

	USCHED_HOOK_NAME="$hook" \
	USCHED_PRELOAD="$preload" \
	USCHED_RT_CLASS="$rt_class" \
	USCHED_PERF_TRACE=1 \
	USCHED_PERF_PLOTTER="$root/src/tests/plot_memif_sched.py" \
	USCHED_CHECK_PERFORMANCE=0 \
	USCHED_RUN_DIR="$run/$name" \
	sh "$root/src/tests/quick_mlx5_many.sh" 24
}

run_case fifo-yield yield fifo "$root/src/libyield.so"
run_case rr-yield yield rr "$root/src/libyield.so"
run_case fifo-bpoll bpoll fifo "$root/src/libbpoll.so"
run_case rr-bpoll bpoll rr "$root/src/libbpoll.so"

printf 'setup,pps,p50_ns,p99_ns,p999_ns,result\n' >"$run/comparison.csv"
for setup in fifo-yield rr-yield fifo-bpoll rr-bpoll; do
	awk -F, -v setup="$setup" 'NR == 2 { print setup "," $10 "," $11 "," $12 "," $13 "," FILENAME }' \
		"$run/$setup/summary.csv" >>"$run/comparison.csv"
done

cat "$run/comparison.csv"
