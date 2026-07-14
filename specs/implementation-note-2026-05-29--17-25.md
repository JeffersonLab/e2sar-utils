# Implementation Note — 2026-05-29 (session 5)

## Session summary

Cleanup and renaming: `shmem_reader.py` → `gluex-reader.py` to avoid confusion with
the PyPI package of the same name, and because the reader is specific to GlueX data.
All references updated across the repo.

---

## What was done

### Rename: `scripts/shmem_reader.py` → `scripts/gluex-reader.py`

Done by user. The old name was ambiguous — `shmem_reader` is also a PyPI package
(the one we import), so the filename created a false collision. The new name makes
the script's purpose explicit.

### `Dockerfile.shmem-reader` → `Dockerfile.gluex-reader`

Done by user. Stale `Dockerfile.shmem-reader` (partial rename artifact) deleted.
`Dockerfile.gluex-reader` updated to consistently reference `gluex-reader.py` in
both `COPY` and `ENTRYPOINT`.

### `sbatch/haidis_cpu_test.sh` — naming cleanup

- `--shmemreaderimage` CLI option renamed to `--gluexreaderimage`
- `SHMEMREADERIMAGE` variable renamed to `GLUEXREADERIMAGE`
- Banner label updated to `GLUEX READER IMAGE`
- Phase 3 comments and log messages updated from `shmem-reader` to `gluex-reader`
- `--shmem-name`, `SHMEM_NAME`, and `Shmem name:` left unchanged — these refer to
  the POSIX shared memory segment name, not the reader script

### Dockerfile consolidation decision

Considered merging `Dockerfile.gluex-reader` into `Dockerfile.cli` as an additional
build target. Decided against it: the two builds have almost no dependency overlap
(`build-essential` only, and only in the CLI's build stage), they run on different
machines (sender host vs Perlmutter compute nodes), and a merged image would either
bloat the gluex-reader image with ROOT/E2SAR (~1.5 GB overhead) or add no value.
Two separate Dockerfiles is the right layout.

---

## Current state of Phase 0 artifacts

| File | Status |
|------|--------|
| `scripts/gluex-reader.py` | Complete |
| `Dockerfile.gluex-reader` | Complete |
| `sbatch/haidis_cpu_test.sh` | Complete |
| `sbatch/haidis_slurm.sh` | CR-005 applied |

---

## Ready to test

The workflow is ready for a first run on Perlmutter. The primary goals are:

1. **Validate end-to-end connectivity** — sender → EJFAT LB → ERSAP → shmem → gluex-reader
2. **Investigate the repeating data bug** — values from `HaidisActor::execute()` repeat
   across shmem batches (see `implementation-note-2026-05-29--14-18.md` for root-cause
   analysis). The gluex-reader is the first tool that can capture raw output for inspection.

### Prerequisites before first run

1. **Build the gluex-reader image on Perlmutter** (login node, from repo root):
   ```bash
   podman-hpc build -f Dockerfile.gluex-reader -t localhost/gluex-reader:dev .
   ```

2. **Obtain a valid EJFAT LB reservation** and export `EJFAT_URI`.

3. **Start the GlueX sender** on the sender host:
   ```bash
   scripts/start-gluex-sender.sh
   ```

### Recommended first submissions

```bash
# Histogram run — confirms data flows and produces a distribution plot
EJFAT_URI=... sbatch -N 1 -A amsc016 haidis_cpu_test.sh --iterations 20

# CSV run — captures raw (x,y) rows to check for repeated values
EJFAT_URI=... sbatch -N 1 -A amsc016 haidis_cpu_test.sh \
    --save raw_data.csv --iterations 10
```

For the repeating data bug: inspect `raw_data.csv` from the CSV run. If the bug is
present, consecutive rows (or entire batches) will be identical. The batch boundary
can be identified by counting rows per `ShmemReader.read_data()` call — each call
returns one batch, and the CSV is flushed after each batch.

### What remains (Phase 1)

`haidis-run/` workflow management package — automates all of the above: image build
check, sender start/stop, job submission, log monitoring, and report generation.
