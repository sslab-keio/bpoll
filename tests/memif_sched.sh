#!/bin/sh
set -eu

binary=$1
plotter=$2
selected_case=${3:-all}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run="$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/memif-sched"
old_runtime=$(cat /proc/sys/kernel/sched_rt_runtime_us)
old_period=$(cat /proc/sys/kernel/sched_rt_period_us)
server_pids=
client_pids=
perf_pid=
control=
preempt_pid=
preempt_trace_pid=

mkdir -p "$run"
exec 3>&1
exec >"$run/experiment.log" 2>&1
set -x

cleanup() {
	status=$?

	trap - EXIT
	set +e
	stop_groups $server_pids $client_pids $preempt_pid $perf_pid
	if [ -n "$control" ]; then
		sudo rm -f "/dev/shm/${control#/}"
	fi
	sudo sh -c "echo $old_runtime > /proc/sys/kernel/sched_rt_runtime_us"
	sudo sh -c "echo $old_period > /proc/sys/kernel/sched_rt_period_us"
	sudo rm -f /dev/shm/usched
	cat "$run/experiment.log" >&3
	exit "$status"
}

stop_groups() {
	for pid in "$@"; do
		[ -n "$pid" ] && sudo kill -TERM -- "-$pid" 2>/dev/null || true
	done
	sleep 1
	for pid in "$@"; do
		[ -n "$pid" ] && sudo kill -KILL -- "-$pid" 2>/dev/null || true
	done
	for _ in $(seq 1 10); do
		sudo pkill -KILL memif_sched 2>/dev/null || true
		sudo pkill -KILL preempt 2>/dev/null || true
		sleep 1
		if ! pgrep -x memif_sched >/dev/null; then
			return 0
		fi
		pgrep -a memif_sched >&2 || true
	done
	echo "memif_sched is still running" >&2
	pgrep -a memif_sched >&2 || true
	return 1
}

start_server() {
	case_run=$1
	case_name=$2
	index=$3
	spin_us=$4

	setsid sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		LD_PRELOAD="$root/src/libbpoll.so:$root/src/libusched-memif.so" \
		taskset -c 13 chrt -f 4 "$binary" \
		-l 13 --proc-type=primary --file-prefix="$case_name-server-$index" --no-pci \
		-d "$root/result/dpdk/lib/x86_64-linux-gnu/dpdk/pmds-23.2/librte_net_memif.so.23.2" \
		--vdev "net_memif0,role=server,id=0,socket=$case_run/$index.sock,socket-abstract=no" \
		-- --role server --control "$control" --index "$index" --spin-us "$spin_us" >"$case_run/server-$index.log" 2>&1 &
	server_pids="$server_pids $!"
}

start_preempt() {
	case_run=$1
	quantum_ns=$2

	[ -n "$quantum_ns" ] || return 0
	setsid sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		taskset -c 13 chrt -f 4 "$root/src/preempt" -n "$quantum_ns" \
		>"$case_run/preempt.log" 2>&1 &
	preempt_pid=$!
	sleep 0.1
	kill -0 "$preempt_pid"
	preempt_trace_pid=$(pgrep -P "$preempt_pid")
	test -n "$preempt_trace_pid"
	if setsid sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		taskset -c 13 chrt -f 4 "$root/src/preempt" -n "$quantum_ns" \
		>"$case_run/preempt-duplicate.log" 2>&1; then
		return 1
	fi
}

start_client() {
	case_run=$1
	case_name=$2
	index=$3
	active=$4
	on_ms=$5
	off_ms=$6
	duration=$7

	setsid sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		taskset -c "$((index + 1))" chrt -f 4 "$binary" \
		-l "$((index + 1))" --proc-type=primary --file-prefix="$case_name-client-$index" --no-pci \
		-d "$root/result/dpdk/lib/x86_64-linux-gnu/dpdk/pmds-23.2/librte_net_memif.so.23.2" \
		--vdev "net_memif0,role=client,id=0,socket=$case_run/$index.sock,socket-abstract=no" \
		-- --role client --control "$control" --index "$index" --active "$active" \
		--on-ms "$on_ms" --off-ms "$off_ms" --duration "$duration" >"$case_run/client-$index.log" 2>&1 &
	client_pids="$client_pids $!"
}

run_case() {
	case_name=$1
	active=$2
	on_ms=$3
	off_ms=$4
	duration=$5
	spin_us=$6
	quantum_ns=$7
	case_run="$run/$case_name"

	mkdir -p "$case_run"
sudo rm -f /dev/shm/usched
	server_pids=
	client_pids=
	preempt_pid=
	preempt_trace_pid=
	control="/memif-sched-$(basename "$run")-$case_name"
	sudo env LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		"$binary" --control-create "$control"
	setsid sudo perf record \
		-q \
		-e sched:sched_switch \
		-C 13 \
		-o "$case_run/perf.data" \
		-- sleep 3600 &
	perf_pid=$!
	sleep 0.2
	for index in $(seq 0 11); do
		sudo rm -rf "/var/run/dpdk/$case_name-server-$index"
		sudo rm -rf "/var/run/dpdk/$case_name-client-$index"
		if [ "$index" = 0 ]; then
			start_server "$case_run" "$case_name" "$index" "$spin_us"
		else
			start_server "$case_run" "$case_name" "$index" 0
		fi
	done
	for index in $(seq 0 11); do
		for _ in $(seq 1 300); do
			[ -S "$case_run/$index.sock" ] && break
			sleep 0.1
		done
		test -S "$case_run/$index.sock"
		start_client "$case_run" "$case_name" "$index" "$active" "$on_ms" "$off_ms" "$duration"
	done
	for index in $(seq 0 11); do
		for _ in $(seq 1 300); do
			if grep -q "ready $index" "$case_run/client-$index.log" &&
				grep -q '^port ready server' "$case_run/server-$index.log"; then
				break
			fi
			sleep 0.1
		done
		grep -q "ready $index" "$case_run/client-$index.log"
		grep -q '^port ready server' "$case_run/server-$index.log"
	done
	start_preempt "$case_run" "$quantum_ns"
	printf 'index,pid\n' >"$case_run/servers.csv"
	for index in $(seq 0 11); do
		server_pid=$(awk '/^port ready server/ { print $NF }' "$case_run/server-$index.log")
		test -n "$server_pid"
		printf '%s,%s\n' "$index" "$server_pid" >>"$case_run/servers.csv"
	done
	if [ -n "$preempt_trace_pid" ]; then
		printf 'preemption,%s\n' "$preempt_trace_pid" >>"$case_run/servers.csv"
	fi
	env LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		"$binary" --control-arm "$control" 1000 >"$case_run/control-start-ns.txt"
	for pid in $client_pids; do
		wait "$pid"
	done
	sudo kill -INT -- "-$perf_pid"
	for _ in $(seq 1 50); do
		if ! sudo kill -0 -- "-$perf_pid" 2>/dev/null; then
			break
		fi
		sleep 0.1
	done
	sudo kill -KILL -- "-$perf_pid" 2>/dev/null || true
	wait "$perf_pid" || true
	perf_pid=
	sudo perf script -i "$case_run/perf.data" >"$case_run/perf.script"
	python3 "$plotter" "$case_run" "$duration"
	printf '{\n  "status": "complete",\n  "active": %s,\n  "on_ms": %s,\n  "off_ms": %s,\n  "duration_s": %s\n}\n' \
		"$active" "$on_ms" "$off_ms" "$duration" >"$case_run/manifest.json"
	stop_groups $server_pids $preempt_pid
	sudo rm -f "/dev/shm/${control#/}"
sudo rm -f /dev/shm/usched
	server_pids=
	client_pids=
	preempt_pid=
	preempt_trace_pid=
	control=
}

trap cleanup EXIT INT TERM

{
	echo "sched_rt_runtime_us before: $old_runtime"
	echo "sched_rt_period_us before: $old_period"
	sudo modprobe msr
	sudo sh -c 'echo 1000000 > /proc/sys/kernel/sched_rt_period_us'
	sudo sh -c 'echo 990000 > /proc/sys/kernel/sched_rt_runtime_us'
	awk '/HugePages_Total|HugePages_Free|HugePages_Rsvd/ { print }' /proc/meminfo
	lscpu
} >"$run/nic-config.txt" 2>&1

case "$selected_case" in
	all)
		run_case rr-12 12 1 0 10 0 ''
		run_case rr-5 5 1 0 10 0 ''
		run_case single 1 1 0 10 0 ''
		run_case onoff-1s 12 1000 1000 10 0 ''
		run_case onoff-2s 12 2000 2000 10 0 ''
		run_case onoff-10s 12 10000 10000 30 0 ''
		;;
	rr-12)
		run_case rr-12 12 1 0 10 0 ''
		;;
	rr-5)
		run_case rr-5 5 1 0 10 0 ''
		;;
	single)
		run_case single 1 1 0 10 0 ''
		;;
	onoff-1s)
		run_case onoff-1s 12 1000 1000 10 0 ''
		;;
	onoff-2s)
		run_case onoff-2s 12 2000 2000 10 0 ''
		;;
	onoff-10s)
		run_case onoff-10s 12 10000 10000 30 0 ''
		;;
	preempt)
		run_case preempt-none 12 1 0 10 10000 ''
		run_case preempt-4ms 12 1 0 10 10000 4000000
		;;
	*)
		exit 1
		;;
esac
