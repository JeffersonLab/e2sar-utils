# Implementation Note — 2026-05-29

## Session summary

First implementation session for Phase 0 of the HAIDIS Run Controller (see PLAN.md).

## What was done

### `scripts/shmem_reader.py` (new file)

Standalone Python 3.11+ script that consumes ERSAP shared-memory output and either
saves raw event data to CSV or accumulates per-axis histograms.

**Key design decisions:**

- Uses `shmem-reader` PyPI package (`ShmemReader` class from `haidis_connectors` repo).
  Source: https://github.com/JeffersonLab/haidis_connectors
- Data layout confirmed from `ersap-et-support` branch (`HaidisGluexLinkActor.hpp`):
  `write_data(in, 2, {duplet_count, 2})` → numpy float64 array, shape `(N, 2)`,
  col 0 = X, col 1 = Y. Default shmem size: 10 MB (10_485_760 bytes).
- Emits verbatim log signals required by `monitor.py`:
  - `Waiting for data (sample 1)` — before first read
  - `HAIDIS TRAINING COMPLETE: epochs=N/N` — after `--iterations` batches
- SIGTERM and SIGINT handled gracefully; `reader.cleanup()` always called via `try/finally`.

**CLI:**
```
shmem_reader.py --shmem-name NAME --sem-name NAME --sem-ack-name NAME
               [--shmem-size BYTES]
               (--save FILE | --histogram)
               [--iterations N]
               [--bins N]
               [--out-stats FILE]
               [--plot FILE]
```

**Modes:**
- `--save FILE`: writes CSV (`x,y` per event), flushed after each batch
- `--histogram`: accumulates numpy histograms; prints ASCII bar chart to stdout on exit;
  `--plot FILE` saves a two-panel matplotlib PNG (Agg backend, headless-safe);
  `.npz` data is always saved alongside `--plot` output (derived as `<stem>.npz`);
  `--out-stats FILE` overrides the `.npz` path independently

## What remains for Phase 0

Per PLAN.md Phase 0:

1. **`sbatch/haidis_cpu_test.sh`** — new sbatch script derived from `haidis_slurm.sh`,
   CPU nodes only (no GPU), Phase 3 replaced by `shmem_reader.py` via srun.
2. **`haidis_slurm.sh` changes** (CR-005): add `ERSAP_CONFIG_DIR` conditional bind mount
   and `SAGIPS_HYDRA_OVERRIDES` passthrough to the SAGIPS container launch.
3. **`haidis-run/` package skeleton** — Phase 1 work, not started yet.

## Open questions / risks

- `shmem-reader` requires Python 3.11+. The `iri-run` package targets 3.9+. The container
  for `shmem_reader.py` must use a 3.11+ base image.
- Bin edges are fixed from the first batch in `--histogram` mode. If the first batch is
  not representative of the full distribution (e.g. warm-up data), the fixed edges may
  clip later values. Consider adding a `--range X_MIN X_MAX Y_MIN Y_MAX` option if this
  becomes a problem in practice.
- The `matplotlib` import is unconditional (needed for `matplotlib.use("Agg")` to precede
  pyplot). This means `matplotlib` must be installed even when only `--save` mode is used.
  Pylance reports unresolved import warnings in the dev environment because matplotlib is
  not installed in the local venv — these are harmless at runtime on the target container.
