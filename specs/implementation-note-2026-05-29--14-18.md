# Implementation Note — 2026-05-29 (session 2)

## Track 1 — Investigating repeated values in shared memory output

### What was investigated

Reviewed all available source for the ERSAP → shmem pipeline:
- `ShmemWriter::write_data()` in `haidis_connectors/source/src/shmem_writer.cpp`
- `ShmemReader.read_data()` in `haidis_connectors/destination/src/shmem_reader/reader.py`
- Header files for all actors on `origin/ersap-et-support`:
  `HaidisActor.hpp`, `HaidisGluexActor.hpp`, `HaidisLinkActor.hpp`,
  `HaidisGluexLinkActor.hpp`
- Actual production `ersap-data/config/services.yaml` (recovered from git history,
  commit `4cbe587`)

### Key findings

**The producer–consumer protocol is correct.**
`ShmemWriter::write_data()` acquires `sem_ack_` before writing and posts `sem_`
after — a proper single-slot handshake. The Python `ShmemReader` inverts this
correctly. `write_data()` does an immediate deep-copy `memcpy` before returning,
so there is no dangling-reference issue.

**Documentation bug in actor headers.**
`HaidisGluexLinkActor.hpp` (and `HaidisLinkActor.hpp`) document the shmem layout
as `[data_size(8)][ndim(4)][dims…]`, but `ShmemWriter::write_data()` actually
writes `[data_size(8)][data_id(2)][ndim(4)][dims…]`. The 2-byte `data_id` field
is missing from the comment, shifting all documented offsets by 2 bytes past
offset 8. The Python reader is correct; the comments are wrong. Not the cause of
the bug but worth fixing.

**Actor source unavailable — cannot confirm root cause.**
The `execute()` implementations for all four actors exist only as compiled `.so`
files on the branch. No source is present in any locally accessible repository
(`JeffersonLab/e2sar-utils`, `gurjyan/haidis_connectors`, `gurjyan/ersap-engine`).

**Service chain (from actual services.yaml):**
```
UniAdapterSourceEngine (Java)  →  HaidisActor (C++)  →  HaidisLinkActor (C++)
```
There is no separate dedicated batching actor. The `PLAN.md` design lists a
`batch_size: 10` config key under `hlink`, but the current services.yaml does not
include it and neither actor header shows a corresponding accumulation buffer.

**Most likely bug location — `HaidisActor::execute()` (source not available).**
Given the symptom (values repeat across batches) and the pipeline structure,
the most probable cause is one of:
1. The ET ring buffer event is not advanced after reading — the same physics event
   is returned on every `execute()` call, producing identical kinematic values.
2. An internal accumulation vector inside `execute()` is not cleared after writing
   to shmem, so each subsequent write prepends all prior data.

To confirm, the source of `HaidisActor::execute()` (or its GlueX counterpart)
needs to be obtained from the original development repository.

---

## Track 2 — Docker container for shmem_reader.py

### What was created

**`docker/Dockerfile.shmem-reader`** — new file. Packages `scripts/shmem_reader.py`
into a minimal `python:3.11-slim` image.

```dockerfile
FROM python:3.11-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir shmem-reader matplotlib

WORKDIR /app
COPY scripts/shmem_reader.py shmem_reader.py

ENV PYTHONUNBUFFERED=1

ENTRYPOINT ["python", "shmem_reader.py"]
```

`build-essential` is included because `posix-ipc` (a dependency of `shmem-reader`)
is a C extension that may need compilation on some platforms. `PYTHONUNBUFFERED=1`
ensures the readiness and completion log signals flush to stdout immediately so
`haidis-run`'s `monitor.py` can detect them.

### Building the image

Run from the **repo root** (the `COPY` path is relative to the build context):

```bash
podman build -f docker/Dockerfile.shmem-reader -t localhost/shmem-reader:dev .
```

### Running from the command line (standalone test)

```bash
podman run --rm \
    --ipc=host \
    --security-opt=label=disable \
    -v /path/to/output:/app/outputs \
    localhost/shmem-reader:dev \
    --shmem-name haidis_shmem \
    --sem-name haidis_sem \
    --sem-ack-name haidis_sem_ack \
    --histogram \
    --plot /app/outputs/histogram.png
```

`--ipc=host` is required so the container shares the host IPC namespace and can
open the POSIX shared memory segment and semaphores created by the ERSAP container.

### Running from inside the sbatch script (replacing SAGIPS Phase 3)

In `haidis_cpu_test.sh`, replace the SAGIPS `srun` block with:

```bash
SHMEMREADERIMAGE="localhost/shmem-reader:dev"

srun --ntasks=${SLURM_NNODES} \
     --ntasks-per-node=1 \
     --overlap \
     podman-hpc run --rm \
         --ipc=host \
         --security-opt=label=disable \
         --group-add keep-groups \
         -v ${JOB_DIR}:/app/outputs \
         ${SHMEMREADERIMAGE} \
         --shmem-name haidis_shmem \
         --sem-name haidis_sem \
         --sem-ack-name haidis_sem_ack \
         --histogram \
         --plot /app/outputs/histogram_$(hostname).png

READER_EXIT=$?
echo "shmem-reader exited with code $READER_EXIT"
```

Key differences from the SAGIPS launch in `haidis_slurm.sh`:
- No `--mpi` flag (shmem_reader is a single process, not MPI)
- No `--gpus-per-node` / `--gpu-bind` / `--gpus all` flags
- `--ntasks-per-node=1` not `${GPUS_PER_NODE}` (one reader per node)
- `--ipc=host` retained (same reason as SAGIPS — must share IPC with ERSAP)
- Per-node output file named with `$(hostname)` so multi-node runs don't collide

### Shmem name note

The default shmem/semaphore names differ between the Dalitz and GlueX actor chains:

| Chain | `--shmem-name` | `--sem-name` | `--sem-ack-name` |
|-------|---------------|-------------|-----------------|
| Dalitz (`HaidisLinkActor`) | `haidis_shmem` | `haidis_sem` | `haidis_sem_ack` |
| GlueX (`HaidisGluexLinkActor`) | `haidis_gluex_shmem` | `haidis_gluex_sem` | `haidis_gluex_sem_ack` |

Match the names to whichever ERSAP services.yaml is deployed.
