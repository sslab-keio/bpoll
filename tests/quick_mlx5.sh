#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
run="$root/test-result/$(date '+%Y%m%dT%H%M%S%:z')/quick-mlx5"
mkdir -p "$run"
server_log="$run/server.log"
client_log="$run/client.log"
server_pid=
success=0

exec 3>&1
exec >"$run/experiment.log" 2>&1
set -x

stop_server() {
	status=$?

	trap - EXIT
	set +e
	if [ -n "$server_pid" ]; then
		sudo kill -TERM "$server_pid" 2>/dev/null || true
	fi

	if [ "$success" = 1 ]; then
		status=complete
	else
		status=failed
	fi
	printf '{\n  "status": "%s",\n  "test": "quick-mlx5",\n  "result": "%s"\n}\n' \
		"$status" "$run" >"$run/manifest.json"
	cat "$run/experiment.log" >&3
	[ "$status" = complete ]
}

trap stop_server EXIT
trap 'exit 130' INT TERM

{
	echo '===== server ====='
	date -Is
	uname -a
	nproc
	lscpu
	lspci -d 15b3: -D
	awk '/HugePages_Total|HugePages_Free|HugePages_Rsvd|Hugepagesize/ { print }' /proc/meminfo
	echo '===== berlin ====='
	ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
		'date -Is; uname -a; nproc; lscpu; lspci -d 15b3: -D; awk '\''/HugePages_Total|HugePages_Free|HugePages_Rsvd|Hugepagesize/ { print }'\'' /proc/meminfo'
} >"$run/nic-config.txt" 2>&1

sudo rm -f /dev/shm/usched
sudo rm -rf /var/run/dpdk/quick-server

sudo env \
	LD_LIBRARY_PATH="$root/result/dpdk/lib/x86_64-linux-gnu" \
	LD_PRELOAD="$root/src/libbpoll.so" \
	taskset -c 0 chrt -f 4 "$root/result/bench-iip/bin/bench-iip" \
	-l 0 --proc-type=primary --file-prefix=quick-server --allow 0000:18:00.2 \
	-- -a 0,10.0.10.110 -- -p 10000 -l 1 >"$server_log" 2>&1 &
server_pid=$!

for _ in $(seq 1 50); do
	if grep -q 'server listens on 10000' "$server_log"; then
		break
	fi
	sleep 0.1
done
grep -q 'layout=normal' "$server_log"
grep -q 'server listens on 10000' "$server_log"

ssh -o BatchMode=yes root@berlin.i.sslab.ics.keio.ac.jp \
"rm -rf /var/run/dpdk/quick-client && /home/tsujimoto/client-load-bundle/bin/bench-iip \
-d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_bus_pci.so \
-d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_mempool_ring.so \
-d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_common_mlx5.so \
-d /home/tsujimoto/client-load-bundle/lib/dpdk/pmds-23.2/librte_net_mlx5.so \
-l 0 --proc-type=primary --file-prefix=quick-client --allow 0000:18:00.2 \
-- -a 0,10.0.10.210 -- -s 10.0.10.110 -p 10000 -l 1 -c 1 -v 1 -t 15" \
>"$client_log" 2>&1

line=$(grep '^throughput rx ' "$client_log")
pps=$(printf '%s\n' "$line" | awk '{ print $5 }')
p50=$(printf '%s\n' "$line" | awk '{ for (i = 1; i <= NF; i++) if ($i == "50%-ile") { value = $(i + 1); sub("ns$", "", value); print value } }')
p99=$(printf '%s\n' "$line" | awk '{ for (i = 1; i <= NF; i++) if ($i == "99%-ile") print $(i + 1) }')
p999=$(printf '%s\n' "$line" | awk '{ for (i = 1; i <= NF; i++) if ($i == "99.9%-ile") { value = $(i + 1); sub("ns$", "", value); print value } }')

test "$pps" -ge 140000
test "$p99" -le 200000
printf 'test,pairs,connections,rate_limit,server_cores,client_cores,pps,p50_ns,p99_ns,p999_ns\n' >"$run/summary.csv"
printf 'quick-mlx5,1,1,none,0,0,%s,%s,%s,%s\n' "$pps" "$p50" "$p99" "$p999" >>"$run/summary.csv"
printf 'quick mlx5: %s pps, p99 %s ns\n' "$pps" "$p99"
success=1
