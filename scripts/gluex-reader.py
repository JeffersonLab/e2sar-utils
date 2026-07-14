#!/usr/bin/env python3
"""Read ERSAP shared-memory output: save to CSV or accumulate per-axis histograms.

Emits the same log signals as SAGIPS so haidis-run monitor.py works unchanged:
  - "Waiting for data (sample 1)"  — once, before first read
  - "HAIDIS TRAINING COMPLETE: epochs=N/N"  — after --iterations batches
"""

import argparse
import signal
import sys
from datetime import datetime, timezone

import matplotlib
matplotlib.use("Agg")  # headless — no display required
import matplotlib.pyplot as plt

import numpy as np
from shmem_reader import ShmemReader

# Set by signal handler; main loop checks this flag each iteration.
_stop = False


def _shutdown(*_):
    global _stop
    _stop = True


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)

    p.add_argument("--shmem-name", required=True, metavar="NAME",
                   help="POSIX shared memory segment name (e.g. haidis_shmem)")
    p.add_argument("--sem-name", required=True, metavar="NAME",
                   help="Data-ready semaphore name (e.g. haidis_sem)")
    p.add_argument("--sem-ack-name", required=True, metavar="NAME",
                   help="Acknowledgment semaphore name (e.g. haidis_sem_ack)")
    p.add_argument("--shmem-size", type=int, default=10_485_760, metavar="BYTES",
                   help="Shared memory segment size in bytes (default: 10485760 = 10 MB)")
    p.add_argument("--iterations", type=int, default=None, metavar="N",
                   help="Stop after N batches and emit completion signal (default: run until SIGTERM)")

    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--save", metavar="FILE",
                      help="Append each batch to FILE as CSV rows (x,y per event)")
    mode.add_argument("--histogram", action="store_true",
                      help="Accumulate per-axis histograms; print summary on exit")

    p.add_argument("--bins", type=int, default=50, metavar="N",
                   help="Number of histogram bins per axis (default: 50; --histogram only)")
    p.add_argument("--out-stats", metavar="FILE",
                   help="Save histogram bin edges and counts as .npz to FILE (--histogram only)")
    p.add_argument("--plot", metavar="FILE",
                   help="Save a two-panel histogram PNG to FILE (--histogram only)")
    p.add_argument("--flush-every", type=int, default=10, metavar="N",
                   help="Re-save plot/stats every N batches (default: 10; 0 = only on exit)")
    p.add_argument("--filter-abs-max", type=float, default=None, metavar="X",
                   help="Discard events where abs(x) > X or abs(y) > X (default: no filter)")

    args = p.parse_args()

    if (args.out_stats or args.plot) and not args.histogram:
        p.error("--out-stats and --plot require --histogram")

    return args


def _extract_array(result) -> np.ndarray | None:
    """Unwrap (array, data_id) tuple or bare array; return None on empty batch."""
    if result is None:
        return None
    arr = result[0] if isinstance(result, tuple) else result
    if arr.size == 0:
        return None
    # Normalise to (N, 2) regardless of how the writer shaped it.
    if arr.ndim == 1:
        arr = arr.reshape(-1, 2)
    elif arr.shape[1] != 2:
        arr = arr.reshape(-1, 2)
    return arr


def _timestamped_path(path: str) -> str:
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%SZ")
    stem, _, ext = path.rpartition(".")
    return f"{stem}_{ts}.{ext}" if stem else f"{path}_{ts}"



def _print_histogram(label: str, edges: np.ndarray, counts: np.ndarray):
    total = int(counts.sum())
    max_count = max(int(counts.max()), 1)
    bar_width = 40
    print(f"\n=== {label} histogram ({len(counts)} bins) ===")
    for lo, hi, c in zip(edges[:-1], edges[1:], counts):
        bar = int(c / max_count * bar_width) * "█"
        print(f"  [{lo:10.4f}, {hi:10.4f}): {c:8d}  {bar}")
    print(f"  Total samples: {total}")


def _save_plot(plot_file: str,
               edges_x: np.ndarray, counts_x: np.ndarray,
               edges_y: np.ndarray, counts_y: np.ndarray):
    centers_x = 0.5 * (edges_x[:-1] + edges_x[1:])
    centers_y = 0.5 * (edges_y[:-1] + edges_y[1:])
    widths_x = edges_x[1:] - edges_x[:-1]
    widths_y = edges_y[1:] - edges_y[:-1]

    fig, (ax_x, ax_y) = plt.subplots(2, 1, figsize=(8, 8), tight_layout=True)

    ax_x.bar(centers_x, counts_x, width=widths_x, color="steelblue", edgecolor="none")
    ax_x.set_xlabel("X")
    ax_x.set_ylabel("Count")
    ax_x.set_title(f"X distribution  (total {int(counts_x.sum()):,})")

    ax_y.bar(centers_y, counts_y, width=widths_y, color="tomato", edgecolor="none")
    ax_y.set_xlabel("Y")
    ax_y.set_ylabel("Count")
    ax_y.set_title(f"Y distribution  (total {int(counts_y.sum()):,})")

    fig.savefig(plot_file, dpi=150)
    plt.close(fig)
    print(f"Histogram plot saved to {plot_file}", flush=True)


def main():
    args = parse_args()

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    reader = ShmemReader(
        shmem_name=args.shmem_name,
        size=args.shmem_size,
        sem_name=args.sem_name,
        sem_ack_name=args.sem_ack_name,
    )

    if not reader.initialize():
        print("ERROR: ShmemReader.initialize() failed", file=sys.stderr)
        sys.exit(1)

    # Histogram state: edges fixed from first batch, counts accumulated incrementally.
    # Out-of-range events are dropped jointly (both axes together) so totals always match.
    hist_edges_x = hist_edges_y = None
    hist_counts_x = hist_counts_y = None
    out_of_range_count = 0

    out_file = None
    iteration = 0

    try:
        if args.save:
            out_file = open(args.save, "w")
            out_file.write("x,y\n")

        print("Waiting for data (sample 1)", flush=True)

        while not _stop:
            reader.wait_for_data()
            result = reader.read_data()
            reader.acknowledge_data()

            arr = _extract_array(result)
            if arr is None:
                continue

            if args.filter_abs_max is not None:
                mask = (np.abs(arr[:, 0]) <= args.filter_abs_max) & \
                       (np.abs(arr[:, 1]) <= args.filter_abs_max)
                arr = arr[mask]
                if arr.size == 0:
                    continue

            if args.save:
                for x, y in arr:
                    out_file.write(f"{x},{y}\n")
                out_file.flush()

            if args.histogram:
                xs, ys = arr[:, 0], arr[:, 1]
                if hist_edges_x is None:
                    _, hist_edges_x = np.histogram(xs, bins=args.bins)
                    _, hist_edges_y = np.histogram(ys, bins=args.bins)
                    hist_counts_x = np.zeros(args.bins, dtype=np.int64)
                    hist_counts_y = np.zeros(args.bins, dtype=np.int64)
                # Drop events where either axis is out of the established range.
                # Joint mask keeps X and Y totals identical.
                in_range = ((xs >= hist_edges_x[0]) & (xs <= hist_edges_x[-1]) &
                            (ys >= hist_edges_y[0]) & (ys <= hist_edges_y[-1]))
                out_of_range_count += int((~in_range).sum())
                xs_in, ys_in = xs[in_range], ys[in_range]
                if xs_in.size:
                    hist_counts_x += np.histogram(xs_in, bins=hist_edges_x)[0]
                    hist_counts_y += np.histogram(ys_in, bins=hist_edges_y)[0]

            if (iteration % 10 == 0):
                print(f"Iteration {iteration}", flush=True)

            iteration += 1

            if args.flush_every and iteration % args.flush_every == 0 and hist_edges_x is not None:
                if args.plot:
                    _save_plot(_timestamped_path(args.plot),
                               hist_edges_x, hist_counts_x, hist_edges_y, hist_counts_y)
                if args.out_stats:
                    np.savez(_timestamped_path(args.out_stats),
                             bins_x=hist_edges_x, counts_x=hist_counts_x,
                             bins_y=hist_edges_y, counts_y=hist_counts_y)

            if args.iterations is not None and iteration >= args.iterations:
                n = args.iterations
                print(f"HAIDIS TRAINING COMPLETE: epochs={n}/{n}", flush=True)
                break

    finally:
        reader.cleanup()
        if out_file is not None:
            out_file.close()

        if args.histogram and hist_edges_x is not None:
            _print_histogram("X", hist_edges_x, hist_counts_x)
            _print_histogram("Y", hist_edges_y, hist_counts_y)
            if out_of_range_count:
                print(f"  Events dropped (out of initial range): {out_of_range_count:,}", flush=True)

            if args.out_stats:
                out_stats_path = _timestamped_path(args.out_stats)
                np.savez(out_stats_path,
                         bins_x=hist_edges_x, counts_x=hist_counts_x,
                         bins_y=hist_edges_y, counts_y=hist_counts_y)
                print(f"Histogram data saved to {out_stats_path}", flush=True)

            if args.plot:
                _save_plot(_timestamped_path(args.plot),
                           hist_edges_x, hist_counts_x,
                           hist_edges_y, hist_counts_y)


if __name__ == "__main__":
    main()
