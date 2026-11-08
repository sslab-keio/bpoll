#!/bin/sh
set -eu

pairs=$1
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test=${USCHED_TEST_NAME:-"quick-mlx5-$pairs"}
hook=${USCHED_HOOK_NAME:-bpoll}
preload=${USCHED_PRELOAD:-"$root/src/libbpoll.so"}
preempt_ns=${USCHED_PREEMPT_NS:-}
rt_class=${USCHED_RT_CLASS:-fifo}
trace=${USCHED_PERF_TRACE:-0}
plotter=${USCHED_PERF_PLOTTER:-}
server_core=${USCHED_SERVER_CORE:-0}
duration=${USCHED_DURATION:-15}
connections=${USCHED_CONNECTIONS:-1}
rate=${USCHED_RATE:-0}
sync_clients=${USCHED_SYNC_CLIENTS:-0}
wait_ping=${USCHED_WAIT_PING:-0}
[ -z "$preempt_ns" ] || test="$test-preempt-$preempt_ns"
run=${USCHED_RUN_DIR:-"$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/$test"}
mkdir -p "$run"
server_pids=
preempt_pid=
perf_pid=
success=0
old_rt_runtime=$(cat /proc/sys/kernel/sched_rt_runtime_us)
old_rt_period=$(cat /proc/sys/kernel/sched_rt_period_us)

exec 3>&1
exec >"$run/experiment.log" 2>&1
set -x

stop_clients() {
	for signal in TERM KILL; do
		for _ in $(seq 1 10); do
			client_processes=$(ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
				"pgrep -f -- '[b]ench-iip.*$test-client-' || true" || true)
			[ -z "$client_processes" ] && return
			ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
				"pkill -$signal -f -- '[b]ench-iip.*$test-client-' || true" || true
			sleep 0.1
		done
	done

	echo "client processes are still running" >&2
	ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
		"pgrep -af -- '[b]ench-iip.*$test-client-' || true" >&2 || true
}

stop_servers() {
	status=$?

	trap - EXIT
	set +e
	stop_clients
	for pid in $perf_pid $preempt_pid $server_pids; do
		sudo kill -TERM "$pid" 2>/dev/null || true
	done
	sleep 1
	for pid in $perf_pid $preempt_pid $server_pids; do
		sudo kill -KILL "$pid" 2>/dev/null || true
	done
	for _ in $(seq 1 10); do
		server_processes=$(pgrep -f -- "$test-server-" || true)
		[ -z "$server_processes" ] && break
		for pid in $server_processes; do
			sudo kill -KILL "$pid" 2>/dev/null || true
		done
		sleep 0.1
	done
	if pgrep -f -- "$test-server-" >/dev/null; then
		echo "server processes are still running" >&2
		pgrep -af -- "$test-server-" >&2 || true
	fi
	sudo sh -c "echo $old_rt_runtime > /proc/sys/kernel/sched_rt_runtime_us"
	sudo sh -c "echo $old_rt_period > /proc/sys/kernel/sched_rt_period_us"
	sudo rm -f /dev/shm/usched

	if [ "$success" = 1 ]; then
		status=complete
	else
		status=failed
	fi
	printf '{\n  "status": "%s",\n  "test": "%s",\n  "result": "%s"\n}\n' \
		"$status" "$test" "$run" >"$run/manifest.json"
	cat "$run/experiment.log" >&3
	[ "$status" = complete ]
}

case "$rt_class" in
	rr) chrt_class=-r ;;
	fifo) chrt_class=-f ;;
	*) exit 1 ;;
esac

start_server() {
	n=$1
	bdf=$2

	sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		LD_PRELOAD="$preload" \
		taskset -c "$server_core" chrt "$chrt_class" 4 "$root/result/bench-iip/bin/bench-iip" \
		-l "$server_core" --proc-type=primary --file-prefix="$test-server-$n" --allow "$bdf" \
		-- -a "0,10.0.10.$((110 + n))" -- -p 10000 -l 1 >"$run/server-$n.log" 2>&1 &
	server_pids="$server_pids $!"
}

start_preempt() {
	[ -n "$preempt_ns" ] || return 0

	sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		taskset -c "$server_core" chrt "$chrt_class" 4 "$root/src/preempt" -n "$preempt_ns" \
		>"$run/preempt.log" 2>&1 &
	preempt_pid=$!
	sleep 0.1
	kill -0 "$preempt_pid"
}

trap stop_servers EXIT
trap 'exit 130' INT TERM

{
	echo '===== server ====='
	date -Is
	echo "sched_rt_runtime_us before: $old_rt_runtime"
	echo "sched_rt_period_us before: $old_rt_period"
	sudo sh -c 'echo 1000000 > /proc/sys/kernel/sched_rt_period_us'
	if [ "$trace" = 1 ]; then
		sudo sh -c 'echo 990000 > /proc/sys/kernel/sched_rt_runtime_us'
	else
		sudo sh -c 'echo 1000000 > /proc/sys/kernel/sched_rt_runtime_us'
	fi
	uname -a
	nproc
	lscpu
	lspci -d 15b3: -D
	awk '/HugePages_Total|HugePages_Free|HugePages_Rsvd|Hugepagesize/ { print }' /proc/meminfo
	echo '===== berlin ====='
	ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
		'date -Is; uname -a; nproc; lscpu; lspci -d 15b3: -D; awk '\''/HugePages_Total|HugePages_Free|HugePages_Rsvd|Hugepagesize/ { print }'\'' /proc/meminfo'
} >"$run/nic-config.txt" 2>&1

bdfs=$(lspci -D -d 15b3:101e | awk '{ print $1 }' | head -n "$pairs")
test "$(printf '%s\n' "$bdfs" | wc -l)" -eq "$pairs"

sudo rm -f /dev/shm/usched
for n in $(seq 0 $((pairs - 1))); do
	sudo rm -rf "/var/run/dpdk/$test-server-$n"
done
stop_clients

if [ "$trace" = 1 ]; then
	setsid sudo perf record -q -e sched:sched_switch -C 0 -o "$run/perf.data" -- sleep 3600 &
	perf_pid=$!
	sleep 0.2
fi

n=0
for bdf in $bdfs; do
	start_server "$n" "$bdf"
	n=$((n + 1))
done

for n in $(seq 0 $((pairs - 1))); do
	for _ in $(seq 1 100); do
		if grep -q 'configuring port 0 with 1 queues' "$run/server-$n.log"; then
			break
		fi
		sleep 0.1
	done
	grep -q 'configuring port 0 with 1 queues' "$run/server-$n.log"
done

if [ "$wait_ping" = 1 ]; then
	for n in $(seq 0 $((pairs - 1))); do
		for _ in $(seq 1 100); do
			if ping -c 1 -W 1 "10.0.10.$((110 + n))" >/dev/null 2>&1; then
				break
			fi
			sleep 0.1
		done
		ping -c 1 -W 1 "10.0.10.$((110 + n))" >/dev/null
	done
fi

start_preempt

if [ "$trace" = 1 ]; then
	printf 'index,pid\n' >"$run/servers.csv"
	for n in $(seq 0 $((pairs - 1))); do
		for _ in $(seq 1 100); do
			server_pid=$(pgrep -f -- "$root/result/bench-iip/bin/bench-iip.*--file-prefix=$test-server-$n" | tail -n 1 || true)
			[ -n "$server_pid" ] && break
			sleep 0.1
		done
		test -n "$server_pid"
		printf '%s,%s\n' "$n" "$server_pid" >>"$run/servers.csv"
	done
	if [ -n "$preempt_ns" ]; then
		preempt_trace_pid=$(pgrep -x preempt | tail -n 1 || true)
		test -n "$preempt_trace_pid"
		printf 'preemption,%s\n' "$preempt_trace_pid" >>"$run/servers.csv"
	fi
	python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))' >"$run/control-start-ns.txt"
fi

ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp 'sh -s' "$pairs" "$test" "$duration" "$connections" "$rate" "$sync_clients" <<'REMOTE' >"$run/client.log" 2>&1
set -eu
set -x
pairs=$1
test=$2
duration=$3
connections=$4
rate=$5
sync_clients=$6
run=$(mktemp -d /tmp/usched-mlx5-client.XXXXXX)
pids=
trap 'kill -CONT $pids 2>/dev/null || true; for pid in $pids; do kill -TERM "$pid" 2>/dev/null || true; done; rm -rf "$run"' EXIT INT TERM

bdfs=$(lspci -D -d 15b3:101e | awk '{ print $1 }' | head -n "$pairs")
test "$(printf '%s\n' "$bdfs" | wc -l)" -eq "$pairs"

for n in $(seq 0 $((pairs - 1))); do
 rm -rf "/var/run/dpdk/$test-client-$n"
done

n=0
for bdf in $bdfs; do
 taskset -c "$n" /home/tsujimoto/client-load-bundle/bin/bench-iip \
  -d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_bus_pci.so \
  -d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_mempool_ring.so \
  -d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_common_mlx5.so \
  -d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_net_mlx5.so \
  -l "$n" --proc-type=primary --file-prefix="$test-client-$n" --allow "$bdf" \
  -- -a "0,10.0.10.$((210 + n))" -- -s "10.0.10.$((110 + n))" -p 10000 -l 1 -r "$rate" -c "$connections" -v 1 -t "$duration" >"$run/$n.log" 2>&1 &
 pids="$pids $!"
 n=$((n + 1))
done

if [ "$sync_clients" = 1 ]; then
 for n in $(seq 0 $((pairs - 1))); do
  for _ in $(seq 1 100); do
   if grep -q 'client: ping-pong mode' "$run/$n.log"; then
    break
   fi
   sleep 0.1
  done
  grep -q 'client: ping-pong mode' "$run/$n.log"
 done
 kill -STOP $pids
 kill -CONT $pids
fi

for pid in $pids; do
 wait "$pid"
done
for n in $(seq 0 $((pairs - 1))); do
 grep -q 'connected peer port 10000' "$run/$n.log"
done
cat "$run"/*.log
REMOTE

if [ "$trace" = 1 ]; then
	sudo kill -INT -- "-$perf_pid" 2>/dev/null || true
	for _ in $(seq 1 50); do
		if ! sudo kill -0 -- "-$perf_pid" 2>/dev/null; then
			break
		fi
		sleep 0.1
	done
	sudo kill -KILL -- "-$perf_pid" 2>/dev/null || true
	wait "$perf_pid" || true
	perf_pid=
	sudo perf script -i "$run/perf.data" >"$run/perf.script"
	python3 "$plotter" "$run" 15
fi

pps=$(awk '/^throughput rx / { sum += $5 } END { print sum + 0 }' "$run/client.log")
p50=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "50%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run/client.log")
p99=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "99%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run/client.log")
p999=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "99.9%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run/client.log")

if [ "${USCHED_CHECK_PERFORMANCE:-1}" = 1 ] && [ -z "$preempt_ns" ]; then
	test "$pps" -ge 140000
	test "$p99" -le 200000
fi
printf 'test,hook,rt_class,preemption_ns,pairs,connections,rate_limit,server_cores,client_cores,pps,p50_ns,p99_ns,p999_ns\n' >"$run/summary.csv"
printf '%s,%s,%s,%s,%s,%s,%s,%s,0-%s,%s,%s,%s,%s\n' "$test" "$hook" "$rt_class" "${preempt_ns:-none}" "$pairs" "$connections" "$rate" "$server_core" "$((pairs - 1))" "$pps" "$p50" "$p99" "$p999" >>"$run/summary.csv"
printf '%s (%s, %s): %s pps, p99 %s ns\n' "$test" "$hook" "$rt_class" "$pps" "$p99"
success=1
