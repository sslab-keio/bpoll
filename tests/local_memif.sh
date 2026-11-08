#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
preempt_ns=${USCHED_PREEMPT_NS:-}
test=local-memif
[ -z "$preempt_ns" ] || test="$test-preempt-$preempt_ns"
run=${USCHED_RUN_DIR:-"$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/$test"}
mkdir -p "$run"
server_pids=
client_pids=
preempt_pid=
success=0
old_rt_runtime=$(cat /proc/sys/kernel/sched_rt_runtime_us)
old_rt_period=$(cat /proc/sys/kernel/sched_rt_period_us)

exec 3>&1
exec >"$run/experiment.log" 2>&1
set -x

stop_all() {
	status=$?

	trap - EXIT
	set +e
	for pid in $preempt_pid $server_pids $client_pids; do
		sudo kill -TERM "$pid" 2>/dev/null || true
	done
	sleep 1
	for pid in $preempt_pid $server_pids $client_pids; do
		sudo kill -KILL "$pid" 2>/dev/null || true
	done
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

start_server() {
	n=$1
	server_ip=$2
	sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		LD_PRELOAD="$root/src/libbpoll.so:$root/src/libusched-memif.so" \
		taskset -c 13 chrt -r 4 "$root/result/bench-iip/bin/bench-iip" \
		-l 13 --proc-type=primary --file-prefix="local-memif-server-$n" --no-pci \
		-d "$root/result/dpdk/lib/x86_64-linux-gnu/dpdk/pmds-23.2/librte_net_memif.so.23.2" \
		--vdev "net_memif0,role=server,id=0,socket=$run/$n.sock,socket-abstract=no" \
		-- -a "0,$server_ip" -- -p 10000 -l 1 >"$run/server-$n.log" 2>&1 &
	server_pids="$server_pids $!"
}

start_client() {
	n=$1
	server_ip=$2
	client_ip=$3
	client_core=$((n + 1))
	sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		LD_PRELOAD="$root/src/libbpoll.so:$root/src/libusched-memif.so" \
		taskset -c "$client_core" chrt -r 4 "$root/result/bench-iip/bin/bench-iip" \
		-l "$client_core" --proc-type=primary --file-prefix="local-memif-client-$n" --no-pci \
		-d "$root/result/dpdk/lib/x86_64-linux-gnu/dpdk/pmds-23.2/librte_net_memif.so.23.2" \
		--vdev "net_memif0,role=client,id=0,socket=$run/$n.sock,socket-abstract=no" \
		-- -a "0,$client_ip" -- -s "$server_ip" -p 10000 -l 1 -c 1 -v 1 -t 15 >"$run/client-$n.log" 2>&1 &
	client_pids="$client_pids $!"
}

start_preempt() {
	[ -n "$preempt_ns" ] || return 0

	sudo env \
		LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
		taskset -c 13 chrt -r 4 "$root/src/preempt" -n "$preempt_ns" \
		>"$run/preempt.log" 2>&1 &
	preempt_pid=$!
	sleep 0.1
	kill -0 "$preempt_pid"
}

trap stop_all EXIT
trap 'exit 130' INT TERM

{
	date -Is
	echo "sched_rt_runtime_us before: $old_rt_runtime"
	echo "sched_rt_period_us before: $old_rt_period"
	sudo sh -c 'echo 1000000 > /proc/sys/kernel/sched_rt_period_us'
	sudo sh -c 'echo 1000000 > /proc/sys/kernel/sched_rt_runtime_us'
	uname -a
	nproc
	lscpu
	awk '/HugePages_Total|HugePages_Free|HugePages_Rsvd|Hugepagesize/ { print }' /proc/meminfo
} >"$run/nic-config.txt" 2>&1

sudo rm -f /dev/shm/usched
for n in $(seq 0 11); do
	sudo rm -rf "/var/run/dpdk/local-memif-server-$n"
	sudo rm -rf "/var/run/dpdk/local-memif-client-$n"
done

for pair in \
	'0 10.0.10.110 10.0.10.210' \
	'1 10.0.10.111 10.0.10.211' \
	'2 10.0.10.112 10.0.10.212' \
	'3 10.0.10.113 10.0.10.213' \
	'4 10.0.10.114 10.0.10.214' \
	'5 10.0.10.115 10.0.10.215' \
	'6 10.0.10.116 10.0.10.216' \
	'7 10.0.10.117 10.0.10.217' \
	'8 10.0.10.118 10.0.10.218' \
	'9 10.0.10.119 10.0.10.219' \
	'10 10.0.10.120 10.0.10.220' \
	'11 10.0.10.121 10.0.10.221'; do
	set -- $pair
	start_server "$1" "$2"
done

for n in $(seq 0 11); do
	for _ in $(seq 1 300); do
		if [ -S "$run/$n.sock" ]; then
			break
		fi
		sleep 0.1
	done
	test -S "$run/$n.sock"
done

start_preempt

for pair in \
	'0 10.0.10.110 10.0.10.210' \
	'1 10.0.10.111 10.0.10.211' \
	'2 10.0.10.112 10.0.10.212' \
	'3 10.0.10.113 10.0.10.213' \
	'4 10.0.10.114 10.0.10.214' \
	'5 10.0.10.115 10.0.10.215' \
	'6 10.0.10.116 10.0.10.216' \
	'7 10.0.10.117 10.0.10.217' \
	'8 10.0.10.118 10.0.10.218' \
	'9 10.0.10.119 10.0.10.219' \
	'10 10.0.10.120 10.0.10.220' \
	'11 10.0.10.121 10.0.10.221'; do
	set -- $pair
	start_client "$1" "$2" "$3"
done

for pid in $client_pids; do
	wait "$pid"
done

pps=$(awk '/^throughput rx / { sum += $5 } END { print sum + 0 }' "$run"/client-*.log)
p50=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "50%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run"/client-*.log)
p99=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "99%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run"/client-*.log)
p999=$(awk '/^throughput rx / { for (i = 1; i <= NF; i++) if ($i == "99.9%-ile") { value = $(i + 1); sub("ns$", "", value); if (value > max) max = value } } END { print max + 0 }' "$run"/client-*.log)

if [ -z "$preempt_ns" ]; then
	test "$pps" -ge 140000
	test "$p99" -le 200000
fi
printf 'test,preemption_ns,pairs,connections,rate_limit,server_cores,client_cores,pps,p50_ns,p99_ns,p999_ns\n' >"$run/summary.csv"
printf '%s,%s,12,1,none,13,1-12,%s,%s,%s,%s\n' "$test" "${preempt_ns:-none}" "$pps" "$p50" "$p99" "$p999" >>"$run/summary.csv"
printf 'local memif: %s pps, p99 %s ns\n' "$pps" "$p99"
success=1
