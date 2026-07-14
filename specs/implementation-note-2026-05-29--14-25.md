# Implementation Note — 2026-05-29 (session 3)

## Session summary

Completed the two remaining Phase 0 deliverables from PLAN.md:

1. CR-005 changes to `sbatch/haidis_slurm.sh`
2. New `sbatch/haidis_cpu_test.sh`

---

## What was done

### `sbatch/haidis_slurm.sh` — CR-005 changes

Three changes applied:

**Header comment** — added two new environment variable entries:

```
ERSAP_CONFIG_DIR          Optional: path to per-run ERSAP config dir; mounted over /user_data/config
SAGIPS_HYDRA_OVERRIDES    Optional: space-separated Hydra key=value overrides passed to SAGIPS
```

**ERSAP config bind mount** — before the `ersap_launcher` heredoc, computes `ERSAP_CONFIG_MOUNT`:

```bash
ERSAP_CONFIG_MOUNT=""
if [[ -n "${ERSAP_CONFIG_DIR:-}" ]]; then
    ERSAP_CONFIG_MOUNT="-v ${ERSAP_CONFIG_DIR}:/user_data/config"
fi
```

`${ERSAP_CONFIG_MOUNT}` is then referenced inside the heredoc. Because the heredoc is unquoted (`<< EOF`), it expands at write time and the resulting launcher script contains either the bind-mount flag or an empty continuation line (which bash silently ignores).

**SAGIPS Hydra passthrough** — added to the Phase 3 `podman-hpc run` invocation:

```bash
-e SAGIPS_HYDRA_OVERRIDES="${SAGIPS_HYDRA_OVERRIDES:-}" \
```

### `sbatch/haidis_cpu_test.sh` — new file

Derived from `haidis_slurm.sh`. Key differences:

| Aspect | `haidis_slurm.sh` | `haidis_cpu_test.sh` |
|--------|-------------------|----------------------|
| `#SBATCH --constraint` | `gpu` | `cpu` |
| GPU SBATCH directives | `--gpus-per-node=4`, `--gpu-bind=none` | removed |
| Phase 3 | SAGIPS via MPI srun | shmem_reader.py via srun (no MPI, no GPU) |
| CLI arg | `--sagipsimage` | `--shmemreaderimage` |
| Additional CLI args | — | `--shmem-name`, `--sem-name`, `--sem-ack-name`, `--iterations` |
| `GPUS_PER_NODE` / `TOTAL_RANKS` | present | removed |

Phase 2 (ERSAP) is structurally identical to `haidis_slurm.sh` with `ERSAP_CONFIG_DIR` support included from the start. Phase 3 uses the same launcher-script pattern as the ERSAP phase so that `$(hostname)` evaluates on each compute node (not on the head node), producing per-node `histogram_<node>.png` output files.

Default image names:

- `--shmemreaderimage`: `localhost/shmem-reader:dev`
- `--ersapimage`: `docker.io/gurjyan/haidis-dp:latest`
- `--e2sarimage`: `docker.io/ibaldin/e2sar:0.3.1`

Default shmem names match the GlueX actor chain (`haidis_shmem` / `haidis_sem` / `haidis_sem_ack`). Override with `--shmem-name` / `--sem-name` / `--sem-ack-name` for other chains (see session-2 note for the Dalitz defaults).

---

## Phase 0 status

All Phase 0 deliverables are complete:

- [x] `scripts/shmem_reader.py` (session 1)
- [x] `docker/Dockerfile.shmem-reader` (session 2)
- [x] `sbatch/haidis_slurm.sh` CR-005 changes (this session)
- [x] `sbatch/haidis_cpu_test.sh` (this session)

## What remains

Phase 1 — `haidis-run/` package skeleton. Starting point per PLAN.md:

1. `pyproject.toml` + package skeleton
2. `config.py` — profile loading, deep-merge, snapshot, consistency validation
3. `backends/base.py` — `JobBackend` Protocol + `JobState` enum
4. `backends/ssh.py` — submit, poll, tail, put via Fabric
5. `perlmutter.py` — submit_haidis_job, wait_for_running, wait_for_readiness
6. `sender.py` — start, stop, liveness
7. `monitor.py` — pattern-matching functions + unit tests
8. `report.py` — log collection, formatting, save
9. `cli.py` — full orchestration, `--dry-run`, `--sbatch-script`
10. `configs/` templates
