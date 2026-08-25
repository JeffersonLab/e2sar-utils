#!/usr/bin/env python3
"""Plot rate and cumulative pairs from one or more gluex-reader log files.

Each log file should come from a separate run (e.g. different --bufsize settings).
Parses lines of the form:
  Iteration N | R pairs/s | T total | elapsed=E.EEs | ts=U.UUU

Usage:
  python plot_rate.py run1.log run2.log [--label "batch=1MB" "batch=4MB"] --output out.png
"""

import argparse
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

LINE_RE = re.compile(
    r"Iteration\s+\d+\s*\|"
    r"\s*([\d,]+)\s*pairs/s\s*\|"
    r"\s*([\d,]+)\s*total\s*\|"
    r"\s*elapsed=([\d.]+)s\s*\|"
    r"\s*ts=([\d.]+)"
)


def parse_log(path: str) -> tuple[list[float], list[float], list[float]]:
    elapsed, rates, totals = [], [], []
    with open(path) as f:
        for line in f:
            m = LINE_RE.search(line)
            if m:
                rate = float(m.group(1).replace(",", ""))
                total = float(m.group(2).replace(",", ""))
                e = float(m.group(3))
                elapsed.append(e)
                rates.append(rate)
                totals.append(total)
    if not elapsed:
        print(f"WARNING: no matching lines found in {path}", file=sys.stderr)
    else:
        t0 = elapsed[0]
        elapsed = [e - t0 for e in elapsed]
    return elapsed, rates, totals


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("logs", nargs="+", metavar="LOG", help="Log file(s) to plot")
    p.add_argument("--label", action="append", metavar="LABEL", dest="label",
                   help="Series label; repeat once per log file (defaults to filename stems)")
    p.add_argument("--output", default="rate_comparison.png", metavar="FILE",
                   help="Output PNG path (default: rate_comparison.png)")
    p.add_argument("--title", default="gluex-reader throughput comparison", metavar="TEXT",
                   help="Plot title")
    args = p.parse_args()
    if args.label and len(args.label) != len(args.logs):
        p.error(f"--label count ({len(args.label)}) must match log file count ({len(args.logs)})")
    if not args.label:
        args.label = [Path(f).stem for f in args.logs]
    return args


def main():
    args = parse_args()

    # Colour cycle — enough for many series
    colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]

    fig, ax_rate = plt.subplots(figsize=(10, 5), tight_layout=True)
    ax_total = ax_rate.twinx()

    for i, (log_path, label) in enumerate(zip(args.logs, args.label)):
        color = colors[i % len(colors)]
        elapsed, rates, totals = parse_log(log_path)
        if not elapsed:
            continue
        ax_rate.plot(elapsed, rates, color=color, linewidth=1.5,
                     label=f"{label} — rate")
        ax_total.plot(elapsed, totals, color=color, linewidth=1.5,
                      linestyle="--", label=f"{label} — total")

    ax_rate.set_xlabel("Elapsed time (s)")
    ax_rate.set_ylabel("Pairs / second")
    ax_total.set_ylabel("Cumulative X,Y pairs received")

    ax_rate.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax_total.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))

    ax_rate.set_title(args.title)

    # Combined legend from both axes
    lines_rate, labels_rate = ax_rate.get_legend_handles_labels()
    lines_total, labels_total = ax_total.get_legend_handles_labels()
    ax_rate.legend(lines_rate + lines_total, labels_rate + labels_total,
                   loc="upper left", fontsize=8)

    fig.savefig(args.output, dpi=150)
    print(f"Saved to {args.output}", flush=True)


if __name__ == "__main__":
    main()
