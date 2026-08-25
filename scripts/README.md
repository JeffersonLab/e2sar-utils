# scripts/

Operational helper scripts for running GlueX data senders and analysing
output from the HAIDIS/ERSAP shared-memory pipeline.

---

## start-gluex-sender.sh

Continuously sends all GlueX ROOT files in a data volume to an EJFAT load
balancer using a Podman container.  The loop restarts `e2sar-root` after each
run and exits only when a stop-file is created (or `stop-gluex-sender.sh` is
called).

**Usage:**

```bash
./start-gluex-sender.sh [--lbid 1|2] [--parallel N] [--eventsize MB]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--lbid N` | `1` | Load balancer to use. `1` = production/stable, `2` = testing only. Both sender and receiver must target the same LB. |
| `--parallel N` | `1` | Number of concurrent file-reader child processes passed to `e2sar-root --parallel`. Values of 5–10 improve throughput significantly. |
| `--eventsize MB` | `1` | EJFAT event (batch) size in MB (`--bufsize-mb` in `e2sar-root`). |

**Stop gracefully:**

```bash
touch /tmp/stop-e2sar-loop-<lbid>   # e.g. /tmp/stop-e2sar-loop-1
```

The loop checks for this file after each `e2sar-root` run and exits cleanly
when it is present.  For an immediate kill, use `stop-gluex-sender.sh`.

**Notes:**

- Requires Podman and the `docker.io/ibaldin/e2sar-utils:latest` image.
- Data volume is hard-coded to `/nvme/haidis/gluex/eta3pi_trees/data2017`
  inside the container (mounted from the host path in the script).
- A singleton file `/tmp/e2sar-loop-running-<lbid>` prevents two instances
  from running against the same LB simultaneously.

---

## stop-gluex-sender.sh

Stops a sender loop started by `start-gluex-sender.sh`.

**Usage:**

```bash
./stop-gluex-sender.sh [--lbid 1|2]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--lbid N` | `1` | Which sender instance to stop (must match the `--lbid` used at start). |

The script:
1. Creates the stop-file so the loop exits after the current run.
2. Sends `SIGKILL` to the `start-gluex-sender.sh` process (PID stored in the
   singleton file).
3. Stops or kills the Podman container (`e2sar-root-<lbid>`).
4. Removes the singleton and stop files.

---

## gluex-reader.py

Reads (x, y) event pairs produced by ERSAP from a POSIX shared-memory segment
and either saves them to a CSV file or accumulates per-axis histograms.
Emits the same log signals as SAGIPS so the HAIDIS monitor script works
unchanged.

**Usage:**

```bash
python gluex-reader.py \
    --shmem-name <NAME> \
    --sem-name <NAME> \
    --sem-ack-name <NAME> \
    (--save FILE | --histogram) \
    [OPTIONS]
```

**Required arguments:**

| Argument | Description |
|----------|-------------|
| `--shmem-name NAME` | POSIX shared memory segment name (e.g. `haidis_shmem`) |
| `--sem-name NAME` | Data-ready semaphore name (e.g. `haidis_sem`) |
| `--sem-ack-name NAME` | Acknowledgment semaphore name (e.g. `haidis_sem_ack`) |
| `--save FILE` or `--histogram` | Output mode — exactly one required |

**Optional arguments:**

| Option | Default | Description |
|--------|---------|-------------|
| `--shmem-size BYTES` | `10485760` (10 MB) | Shared memory segment size |
| `--iterations N` | run until `SIGTERM` | Stop after N batches and emit `HAIDIS TRAINING COMPLETE` |
| `--bins N` | `50` | Histogram bins per axis (`--histogram` only) |
| `--out-stats FILE` | — | Save histogram edges and counts as `.npz` (`--histogram` only) |
| `--plot FILE` | — | Save a two-panel histogram PNG (`--histogram` only) |
| `--flush-every N` | `10` | Re-save plot/stats every N batches; `0` = only on exit |
| `--filter-abs-max X` | — | Discard events where `abs(x) > X` or `abs(y) > X` |

**Examples:**

```bash
# Save raw pairs to CSV
python gluex-reader.py \
    --shmem-name haidis_shmem --sem-name haidis_sem \
    --sem-ack-name haidis_sem_ack \
    --save output.csv

# Accumulate histograms and save a plot every 20 batches
python gluex-reader.py \
    --shmem-name haidis_shmem --sem-name haidis_sem \
    --sem-ack-name haidis_sem_ack \
    --histogram --bins 100 --plot rates.png --flush-every 20

# Run for exactly 50 batches
python gluex-reader.py \
    --shmem-name haidis_shmem --sem-name haidis_sem \
    --sem-ack-name haidis_sem_ack \
    --histogram --iterations 50
```

**Dependencies:** `numpy`, `matplotlib`, `shmem_reader` (ERSAP Python binding).

---

## plot_rate.py

Plots instantaneous throughput (pairs/s) and cumulative pair counts from one
or more `gluex-reader.py` log files on a dual-y-axis chart.  Useful for
comparing runs with different `--bufsize` or `--parallel` settings.

**Usage:**

```bash
python plot_rate.py run1.log run2.log \
    [--label "batch=1MB" "batch=4MB"] \
    [--output out.png] \
    [--title "My comparison"]
```

| Option | Default | Description |
|--------|---------|-------------|
| `LOG ...` | — | One or more log files produced by `gluex-reader.py` |
| `--label LABEL` | filename stems | Series label; repeat once per log file |
| `--output FILE` | `rate_comparison.png` | Output PNG path |
| `--title TEXT` | `gluex-reader throughput comparison` | Chart title |

**Log format expected:**

Each log file must contain lines of the form emitted by `gluex-reader.py`:

```
Iteration N | R pairs/s | T total | elapsed=E.EEs | ts=U.UUU
```

**Example:**

```bash
# Compare two batch sizes
python plot_rate.py batch1mb.log batch4mb.log \
    --label "bufsize=1MB" "bufsize=4MB" \
    --output comparison.png
```

**Dependencies:** `matplotlib`.
