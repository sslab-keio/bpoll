#!/usr/bin/env python3
import csv
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def events(path, pids):
    pattern = re.compile(r"\s(\d+\.\d+):.*sched:sched_switch:.*prev_pid=(\d+).*==> next_comm=.* next_pid=(\d+)")
    for line in path.read_text(errors="replace").splitlines():
        match = pattern.search(line)
        if match:
            yield float(match.group(1)), int(match.group(2)), int(match.group(3))


def intervals(path, pids, start, end):
    running = None
    previous = None
    result = {pid: [] for pid in pids}
    count = {pid: 0 for pid in pids}
    for now, _, next_pid in events(path, pids):
        if previous is not None and running in result:
            left = max(previous, start)
            right = min(now, end)
            if right > left:
                result[running].append((left - start, right - left))
        if start <= now < end and next_pid in count:
            count[next_pid] += 1
        running = next_pid
        previous = now
    return result, count


def labels(rows, durations, counts):
    return [f"{row['index']}  {counts[int(row['pid'])]}  {durations[int(row['pid'])] * 1000:.2f} ms" for row in rows]


def draw_zoom(axis, data, rows, labels, begin, end):
    for y, row in enumerate(rows):
        for left, width in data[int(row["pid"])]:
            clipped_left = max(left, begin)
            clipped_right = min(left + width, end)
            if clipped_right > clipped_left:
                axis.broken_barh([(clipped_left * 1000, (clipped_right - clipped_left) * 1000)], (y - 0.4, 0.8), facecolors="#d62728")
    axis.set(xlim=(begin * 1000, end * 1000), ylim=(-0.5, len(rows) - 0.5), xlabel="elapsed time [ms]")
    axis.set_yticks(range(len(rows)), labels)
    axis.grid(axis="x", linestyle=":")


def main():
    result = Path(sys.argv[1])
    duration = float(sys.argv[2])
    start = int((result / "control-start-ns.txt").read_text().strip()) / 1e9
    rows = list(csv.DictReader((result / "servers.csv").open()))
    pids = {int(row["pid"]) for row in rows}
    data, counts = intervals(result / "perf.script", pids, start, start + duration)
    durations = {pid: sum(width for _, width in segments) for pid, segments in data.items()}
    labels_text = labels(rows, durations, counts)
    with (result / "schedule-summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["index", "pid", "switch_in", "cpu_time_s"])
        for row in rows:
            pid = int(row["pid"])
            writer.writerow([row["index"], pid, counts[pid], f"{durations[pid]:.9f}"])

    bins = int(duration * 10)
    heat = np.zeros((len(rows), bins))
    for y, row in enumerate(rows):
        for left, width in data[int(row["pid"])]:
            first = max(0, int(left * 10))
            last = min(bins - 1, int((left + width) * 10))
            for index in range(first, last + 1):
                bin_left = index / 10
                bin_right = bin_left + 0.1
                heat[y, index] += max(0, min(left + width, bin_right) - max(left, bin_left)) * 1000

    if duration == 10:
        zooms = [1, 5, 9]
    elif duration == 15:
        zooms = [1, 7, 14]
    else:
        zooms = [1, 15, 29]
    fig, axes = plt.subplots(4, 1, figsize=(18, 13), sharey=True)
    cmap = plt.colormaps["Reds"].copy()
    cmap.set_bad("white")
    image = axes[0].pcolormesh(
        np.arange(bins + 1) * 100,
        np.arange(len(rows) + 1) - 0.5,
        np.ma.masked_equal(heat, 0),
        cmap=cmap,
        shading="flat",
        edgecolors="none",
        antialiased=False)
    axes[0].set(xlabel="elapsed time [ms]", ylabel="server index  switch-ins  CPU time", yticks=range(len(rows)), yticklabels=labels_text, title="CPU time per 100 ms bin [ms]")
    fig.colorbar(image, ax=axes[0], label="CPU time [ms]")
    for axis, begin in zip(axes[1:], zooms):
        draw_zoom(axis, data, rows, labels_text, begin, begin + 0.001)
        axis.set_title(f"scheduled intervals: {begin * 1000:.0f}–{(begin + 0.001) * 1000:.0f} ms")
    fig.tight_layout()
    fig.savefig(result / "schedule-timeline.png", dpi=180)
    fig.savefig(result / "schedule-timeline.pdf")


if __name__ == "__main__":
    main()
