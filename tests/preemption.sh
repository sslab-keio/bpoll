#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run="$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/preemption"

mkdir -p "$run"
exec 3>&1
exec >"$run/experiment.log" 2>&1
set -x

run_test() {
	name=$1
	quantum_ns=$2
	script=$3
	shift 3

	if [ -n "$quantum_ns" ]; then
		USCHED_PREEMPT_NS="$quantum_ns" USCHED_RUN_DIR="$run/$name-$quantum_ns" sh "$script" "$@"
	else
		USCHED_RUN_DIR="$run/$name-none" sh "$script" "$@"
	fi
}

for quantum_ns in '' 4000000 50000 5000; do
	run_test local-memif "$quantum_ns" "$root/src/tests/local_memif.sh"
	run_test quick-mlx5-24 "$quantum_ns" "$root/src/tests/quick_mlx5_many.sh" 24
done

printf 'test,preemption_ns,pairs,connections,rate_limit,server_cores,client_cores,pps,p50_ns,p99_ns,p999_ns\n' >"$run/comparison.csv"
for summary in "$run"/*/summary.csv; do
	tail -n +2 "$summary" >>"$run/comparison.csv"
done

cat "$run/experiment.log" >&3
