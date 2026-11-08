# Building and running bpoll

This example shows how bpoll enables DPDK applications to share one CPU core without significant performance degradation, using a simple TCP echo server workload.

The authors tested this procedure with the following environment:

<table>
<tr><td>CPU</td><td>Intel Xeon Gold 5318Y (24 logical CPUs)</td></tr>
<tr><td>NIC</td><td>Intel X540-AT2</td></tr>
<tr><td>Distribution</td><td>Ubuntu 24.04.4 LTS</td></tr>
<tr><td>Kernel</td><td>Linux 6.14.0-37-generic</td></tr>
</table>

## Build

Clone this repository together with its submodules.

```sh
git clone --recurse-submodules --jobs $(nproc) https://github.com/sslab-keio/bpoll.git
cd bpoll/examples

ln -s ../iip bench-iip/iip
ln -s ../iip-dpdk bench-iip/iip-dpdk
```

Install the following build dependencies.

```text
sudo apt install build-essential make meson ninja-build pkg-config python3-pyelftools wget libnuma-dev libbsd-dev
```

Build bench-iip and its DPDK backend first, followed by bpoll and the helper program.

```sh
make -C bench-iip IOSUB_DIR=./iip-dpdk

PKG_CONFIG_PATH="$(realpath ./iip-dpdk/dpdk/install/lib/x86_64-linux-gnu/pkgconfig)" \
    DPDK_SRC="$(realpath ./iip-dpdk/dpdk/dpdk-23.07)" \
    make -C ..

PKG_CONFIG_PATH="$(realpath ./iip-dpdk/dpdk/install/lib/x86_64-linux-gnu/pkgconfig)" \
    make -C misc
```

## Run with memif (no PCI devices)

This setup runs two bpoll-applied DPDK applications on CPU 0 and one client program per server on CPUs 1 and 2.

Load the `msr` module first so bpoll can read the CPU's TSC frequency.

```sh
sudo modprobe msr
```

Start the first server in one terminal.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libbpoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=memif-server-0 \
    --no-pci \
    --vdev "net_memif0,role=server,id=0,socket=/tmp/memif-0.sock,socket-abstract=no" \
    -- -a 0,10.0.10.110 \
    -- -p 10000 -l 1
```

Start the second server in another terminal.
It shares CPU 0 with the first server.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libbpoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=memif-server-1 \
    --no-pci \
    --vdev "net_memif0,role=server,id=0,socket=/tmp/memif-1.sock,socket-abstract=no" \
    -- -a 0,10.0.10.111 \
    -- -p 10000 -l 1
```

After both servers are ready, start the first client on CPU 1 in the same machine.

```sh
sudo env LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    bench-iip/a.out \
    -l 1 \
    --proc-type=primary \
    --file-prefix=memif-client-0 \
    --no-pci \
    --vdev "net_memif0,role=client,id=0,socket=/tmp/memif-0.sock,socket-abstract=no" \
    -- -a 0,10.0.10.210 \
    -- -s 10.0.10.110 -p 10000 -l 1 -r 0 -c 1
```

Start the second client on CPU 2.

```sh
sudo env LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    bench-iip/a.out \
    -l 2 \
    --proc-type=primary \
    --file-prefix=memif-client-1 \
    --no-pci \
    --vdev "net_memif0,role=client,id=0,socket=/tmp/memif-1.sock,socket-abstract=no" \
    -- -a 0,10.0.10.211 \
    -- -s 10.0.10.111 -p 10000 -l 1 -r 0 -c 1
```


After the clients have printed stable statistics, stop the clients and servers with `Ctrl-C`.


<details>
<summary>Running two default DPDK applications on one CPU core</summary>

Stop the bpoll-applied applications first.
Then start the first default DPDK server on CPU 0.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=memif-server-0 \
    --no-pci \
    --vdev "net_memif0,role=server,id=0,socket=/tmp/memif-0.sock,socket-abstract=no" \
    -- -a 0,10.0.10.110 \
    -- -p 10000 -l 1
```

Start the second server on the same CPU.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=memif-server-1 \
    --no-pci \
    --vdev "net_memif0,role=server,id=0,socket=/tmp/memif-1.sock,socket-abstract=no" \
    -- -a 0,10.0.10.111 \
    -- -p 10000 -l 1
```

Then run the same clients shown above.

</details>


The following aggregate results were measured with the previous setup in the environment listed above.
They are reference results and have not been remeasured with the current implementation.
Throughput is summed across both clients, while latency percentiles are arithmetic means.

| Setup | Total throughput | p50 latency (average) | p99 latency (average) | p99.9 latency (average) |
| --- | ---: | ---: | ---: | ---: |
| naive | 301,393 pps (2,411,144 bps) | 3.03 µs | 3.34 µs | **3.00 ms** |
| bpoll | 209,074 pps (1,672,592 bps) | 9.17 µs | 10.2 µs | 11.8 µs |

Because the default DPDK applications do not voluntarily yield, one server may starve the other; the bold p99.9 value above shows the resulting tail-latency outlier.


## Using a physical NIC

### Requirements

The physical-NIC experiment requires two machines connected through SR-IOV-capable NICs.
Tested devices are:

- Intel X540 NIC (ixgbe driver)
- Mellanox ConnectX-6 NIC (mlx5 driver)

### Build

Please repeat the Build step above on the client machine.

### NIC preparation

#### Intel NIC

Create two VFs from the PF selected by `PF_BDF`.

```sh
export PF_BDF='0000:18:00.0'
sudo sh -c "echo 0 > /sys/bus/pci/devices/$PF_BDF/sriov_numvfs"
sudo sh -c "echo 2 > /sys/bus/pci/devices/$PF_BDF/sriov_numvfs"
readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn0"
readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn1"
```

Bind both VFs to `vfio-pci`.

```sh
sudo modprobe vfio-pci
for index in 0 1; do
    bdf=$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn$index")")
    driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$bdf/driver")")
    sudo sh -c "echo $bdf > /sys/bus/pci/drivers/$driver/unbind"
    sudo sh -c "echo vfio-pci > /sys/bus/pci/devices/$bdf/driver_override"
    sudo sh -c "echo $bdf > /sys/bus/pci/drivers_probe"
done
```

#### Mellanox NIC

Install NVIDIA MFT/mstflint, then enable SR-IOV and reserve at least two VFs in firmware.
Reboot the machine to apply the changes.

```sh
export PF_BDF='0000:18:00.0'
sudo apt install mstflint
sudo mstconfig -d "$PF_BDF" query | grep -E 'SRIOV_EN|NUM_OF_VFS'
sudo mstconfig -d "$PF_BDF" set SRIOV_EN=1 NUM_OF_VFS=2
sudo reboot
```

After the reboot, create the runtime VFs with the same sysfs commands used for the Intel NIC.
Leave its VFs bound to the kernel mlx5 driver rather than binding them to `vfio-pci`.

```sh
sudo sh -c "echo 0 > /sys/bus/pci/devices/$PF_BDF/sriov_numvfs"
sudo sh -c "echo 2 > /sys/bus/pci/devices/$PF_BDF/sriov_numvfs"
readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn0"
readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn1"
```

### Run

On the server host, start the first bpoll server on CPU 0.

```sh
# Set PF_BDF to the server host's PF bus number first
export PF_BDF='0000:18:00.0'
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libbpoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=physical-server-0 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn0")")" \
    -- -a 0,10.0.10.110 \
    -- -p 10000 -l 1
```

Start the second server on the same CPU in another terminal.

```sh
export PF_BDF='0000:18:00.0'
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libbpoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=physical-server-1 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn1")")" \
    -- -a 0,10.0.10.111 \
    -- -p 10000 -l 1
```

On the peer host, run one client for each server.
The Intel X540 setup should preload `misc/libnorss.so` to bypass the unsupported RSS RETA query.

Start the first client on CPU 1.

```sh
# Set `PF_BDF` to the client host's PF BDF first.
export PF_BDF='0000:18:00.0'
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=./misc/libnorss.so \
    bench-iip/a.out \
    -l 1 \
    --proc-type=primary \
    --file-prefix=physical-client-0 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn0")")" \
    -- -a 0,10.0.10.210 \
    -- -s 10.0.10.110 -p 10000 -l 1 -r 0 -c 1
```

Start the second client on CPU 2.

```sh
export PF_BDF='0000:18:00.0'
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=./misc/libnorss.so \
    bench-iip/a.out \
    -l 2 \
    --proc-type=primary \
    --file-prefix=physical-client-1 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn1")")" \
    -- -a 0,10.0.10.211 \
    -- -s 10.0.10.111 -p 10000 -l 1 -r 0 -c 1
```

After the clients have printed stable statistics, stop the clients and servers with `Ctrl-C`.


<details>
<summary>Run the same servers with RX interrupts and epoll</summary>

Stop the bpoll servers first, then start the first epoll server on CPU 0.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libepoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=physical-epoll-server-0 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn0")")" \
    -- -a 0,10.0.10.110 \
    -- -p 10000 -l 1
```

Start the second epoll server on the same CPU.

```sh
sudo env \
    LD_LIBRARY_PATH=iip-dpdk/dpdk/install/lib/x86_64-linux-gnu \
    LD_PRELOAD=../libepoll.so \
    chrt -f 4 bench-iip/a.out \
    -l 0 \
    --proc-type=primary \
    --file-prefix=physical-epoll-server-1 \
    --allow "$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn1")")" \
    -- -a 0,10.0.10.111 \
    -- -p 10000 -l 1
```

After both servers have initialized, route each VF's interrupts to the same CPU as the applications so the consolidated core accounts for interrupt-processing cost.

```sh
sudo systemctl stop irqbalance

# Set `PF_BDF` to the server host's PF BDF
export PF_BDF='0000:18:00.0'
for index in 0 1; do
    bdf=$(basename "$(readlink -f "/sys/bus/pci/devices/$PF_BDF/virtfn$index")")

    for irq_path in "/sys/bus/pci/devices/$bdf/msi_irqs/"*; do
        [ -e "$irq_path" ] || continue
        irq=${irq_path##*/}
        echo 0 | sudo tee "/proc/irq/$irq/smp_affinity_list"
    done
done
```

</details>

The following reference results were measured with the previous setup using the same hardware, client commands, and measurement duration.

| Setup | Total throughput | p50 latency (average) | p99 latency (average) | p99.9 latency (average) |
| --- | ---: | ---: | ---: | ---: |
| bpoll | 110,739 pps (885,912 bps) | 17.8 µs | 19.1 µs | 20.2 µs |
| epoll | 90,522 pps (724,176 bps) | 21.5 µs | 23.5 µs | 25.1 µs |

In that measurement, bpoll achieved 22.3% higher total throughput than epoll.
