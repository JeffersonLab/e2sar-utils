# HAIDIS Run Controller — Implementation Plan

**Status:** Draft v0.2 — updated with concrete config structures
**Target package:** `haidis-run/` (new subdirectory, parallel to `iri-run/`)

---

## Guiding Principles

- **Fabric over raw SSH**: All remote execution and file transfer goes through Fabric's `Connection` API. This gives us a clean abstraction that maps directly to the IRI swapout.
- **Backend abstraction from day one**: Define a `JobBackend` Protocol before writing any SSH or IRI code, so Phase 2 (IRI) is a mechanical swap, not a refactor.
- **Config snapshot is the audit trail**: Every run produces a self-contained directory that can reconstruct what was run, with what config, and why it failed.
- **Mirror iri-run's conventions**: Same Python 3.9+ target, same pyproject.toml layout, same `src/` layout, same testing approach.

---

## Package Structure

```text
haidis-run/
├── pyproject.toml
├── README.md
├── CONTEXT.md
├── configs/
│   ├── default_run_profile.yaml        # default run parameters (no secrets)
│   ├── default_ersap_services.yaml     # default ERSAP services.yaml template
│   └── default_sagips.yaml             # reference SAGIPS Hydra config (docs only)
├── src/
│   └── haidis_run/
│       ├── __init__.py
│       ├── cli.py                      # Entry point; orchestrates all phases
│       ├── config.py                   # Profile loading, config merging, snapshot
│       ├── backends/
│       │   ├── __init__.py
│       │   ├── base.py                 # JobBackend Protocol
│       │   ├── ssh.py                  # Fabric-based backend (Phase 1)
│       │   └── iri.py                  # IRI API backend (Phase 2, stub initially)
│       ├── perlmutter.py               # Perlmutter-specific: submit, poll, log tail
│       ├── sender.py                   # Sender host: start, liveness, stop
│       ├── monitor.py                  # Log scanning, pattern matching, readiness
│       └── report.py                   # Failure collection and report formatting
└── tests/
    ├── test_config.py
    ├── test_monitor.py
    └── test_report.py

sbatch/
├── haidis_slurm.sh                     # existing GPU script (modified for CR-005)
└── haidis_cpu_test.sh                  # new: ERSAP + Python reader on CPU nodes (Phase 0)

scripts/
├── start-gluex-sender.sh               # existing
├── stop-gluex-sender.sh                # existing
└── shmem_reader.py                     # new: simple shmem consumer replacing SAGIPS (Phase 0)
```

---

## Module Responsibilities

### `cli.py`

Single entry point `haidis-run`. Parses arguments, constructs the backend,
and calls each phase function in sequence. On any exception it calls
`report.collect_and_print()` before exiting non-zero.

Key CLI flags:

```text
haidis-run [--profile YAML] [--ejfat-uri URI | env:EJFAT_URI]
           [--nodes N] [--account ACCT] [--queue QOS] [--time MIN]
           [--max-epochs N] [--timeout SECS] [--backend ssh|iri]
           [--sender-host HOST] [--dataset PATH]
           [--ersap-override KEY=VALUE ...] [--sagips-override KEY=VALUE ...]
           [--sbatch-script PATH] [--dry-run]
```

`--dry-run` prints the merged config and the sbatch command that would be
run, then exits — useful for validating profiles.

### `config.py`

```python
load_profile(path: str | None) -> dict
build_ersap_services(profile: dict) -> str
build_sagips_overrides(profile: dict) -> str
validate_config_consistency(ersap_cfg, sagips_overrides)
save_snapshot(profile, run_id, run_dir) -> Path
sync_snapshot_to_nersc(conn, run_dir, nersc_dir)
```

**ERSAP merging** (deep-merge, nested dicts merged, lists replaced):

- Template: `configs/default_ersap_services.yaml`
- Profile section `ersap_overrides:` is a partial YAML tree — deep-merged onto the template
- Result written to `runs/<run-id>/ersap/config/services.yaml` and synced to NERSC
- The sbatch script receives `ERSAP_CONFIG_DIR=<nersc_job_dir>/ersap/config` and adds `-v $ERSAP_CONFIG_DIR:/user_data/config` to the ERSAP `podman-hpc run` call

**SAGIPS merging** (Hydra override string):

- Profile section `sagips_overrides:` is a flat dict of Hydra key=value pairs
- Converted to a space-separated string: `opt.n_epochs=100 dataloader.dataset.shm_name=my_shmem ...`
- Written to `runs/<run-id>/sagips_overrides.txt` for auditability
- Passed to sbatch via `SAGIPS_HYDRA_OVERRIDES` env var

**Consistency validation** (enforced before submission):

- `configuration.services.hlink.shmem_name` (ERSAP) must equal `dataloader.dataset.shm_name` (SAGIPS)
- `sem_name` and `sem_ack_name` pairs validated the same way
- On mismatch: print both values and the config keys, then exit

**run_params.json** — flat dict written to `runs/<run-id>/`: job_id, nodes, account, queue,
slurm_time, images, run_id, submit_time, start_time, end_time, outcome. No EJFAT URI.

### `backends/base.py`

```python
class JobBackend(Protocol):
    def submit_job(self, params: dict) -> str: ...
    def get_job_state(self, job_id: str) -> JobState: ...
    def get_node_list(self, job_id: str) -> list[str]: ...
    def tail_file(self, path: str, offset: int) -> str: ...
    def put_file(self, local: Path, remote: str): ...
    def cancel_job(self, job_id: str): ...

class JobState(str, Enum):
    PENDING = "PENDING"
    CONFIGURING = "CONFIGURING"
    RUNNING = "RUNNING"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    CANCELLED = "CANCELLED"
    TIMEOUT = "TIMEOUT"
    UNKNOWN = "UNKNOWN"
```

### `backends/ssh.py`

`SSHBackend(perlmutter_conn: fabric.Connection)`:

- `submit_job`: runs `cd <haidis_dir> && EJFAT_URI=... sbatch -N N -A acct -q qos ...`; parses `Submitted batch job <id>`
- `get_job_state`: runs `squeue --job <id> --format="%T" --noheader`; maps to `JobState`
- `get_node_list`: runs `squeue --job <id> --format="%N" --noheader`; expands with `scontrol show hostname`
- `tail_file`: uses SFTP `conn.sftp().open(path).read()` from a saved offset, or falls back to `tail -c +<offset>`
- `put_file`: `conn.put(local, remote)`

Connection is created once in `cli.py` and passed to the backend. Fabric
handles SSH connection reuse and keepalives.

### `backends/iri.py`

`IRIBackend(token: str)` — wraps `iri_run.api`:

- `submit_job`: builds PSI/J `JobSpec` with `executable=sbatch`, calls `api.submit_job()`
- `get_job_state`: calls `GET /compute/status/{resource}/{job_id}`; maps IRI `JobState` to local enum
- `tail_file`: calls `api.read_file_chunk()` with byte offset
- `put_file`: calls `PUT /filesystem/upload/{resource}`

Initially a stub with `NotImplementedError`; filled in during Phase 2.

### `perlmutter.py`

```python
def submit_haidis_job(backend, params) -> str
def wait_for_running(backend, job_id, timeout) -> list[str]
def wait_for_readiness(backend, job_id, node_list, timeout) -> None
def tail_log(backend, path, state: TailState) -> tuple[str, TailState]
```

`TailState` is a dataclass holding the current byte offset per file; passed
back and forth each poll cycle so we read only new content.

### `sender.py`

```python
def start_sender(conn: fabric.Connection, script_path, params) -> str
def stop_sender(conn, marker)
def sender_is_alive(conn, marker) -> bool
```

The sender script is started via `nohup <script> <args> &` and the PID is
captured. `sender_is_alive` checks `kill -0 <pid>`. On stop, `kill <pid>` +
`pkill -f <script_name>` as fallback.

### `monitor.py`

Pure functions operating on log text strings — no I/O, easy to test.

```python
def check_ersap_ready(log_text: str, expected_nodes: list[str]) -> bool
def check_sagips_ready(stdout_text: str) -> bool
def check_epoch_complete(stdout_text: str, target_epochs: int) -> tuple[bool, int]
def extract_errors(text: str, patterns: list[str]) -> list[str]
def check_sagips_srun_done(stdout_text: str) -> bool
```

Error pattern defaults (overridable in profile):

```text
sagips_stderr: [Traceback, ValueError, IndexError, "srun: error", "Error executing job"]
ersap_log:     [ERROR, Exception, "BIND MOUNT FAILED", Unable, "Segmentation fault"]
```

### `report.py`

```python
def collect_failure_logs(backend, job_id, node_list, nersc_base) -> dict[str, str]
def format_report(phase, error, logs, params, run_id) -> str
def save_report(report_str: str, run_dir: Path) -> Path
```

Fetches last 100 lines of each log file via `tail_file`. Formats as:

```text
=== HAIDIS Run Failure Report ===
Run ID:    20260526T1430-a3f2b1
Phase:     Phase 4 — Container readiness
Time:      2026-05-26T14:37:22Z
Error:     Timeout waiting for ERSAP readiness (nodes: nid001234)

--- SAGIPS stdout (last 100 lines) ---
...
--- SAGIPS stderr (last 100 lines) ---
...
--- ERSAP log: nid001234 (last 100 lines) ---
...

Log paths on NERSC:
  /global/cfs/cdirs/amsc016/haidis/runs/slurm-JOB_ID.out
  /global/cfs/cdirs/amsc016/haidis/runs/slurm-JOB_ID.err
  /global/cfs/cdirs/amsc016/haidis/runs/slurm_job_JOB_ID/ersap_nid001234.log
```

---

## Configuration Design

### `default_run_profile.yaml`

```yaml
# Perlmutter
perlmutter_host: perlmutter.nersc.gov
perlmutter_user: null          # defaults to local $USER
nersc_account: amsc016
nersc_queue: debug
nersc_time_minutes: 30
nersc_nodes: 1
nersc_haidis_dir: /global/cfs/cdirs/amsc016/haidis

# Containers
sagips_image: localhost/haidis-ips:dev
ersap_image: docker.io/gurjyan/haidis-dp:latest
e2sar_image: docker.io/ibaldin/e2sar:0.3.1
gpus_per_node: 4

# Sender
sender_host: null              # required; no default
sender_user: null              # defaults to local $USER
sender_script: /path/to/start-sender.sh
sender_args: {}

# Run control
max_epochs: null               # null = no epoch-based termination
run_timeout_secs: null         # null = no wall-time termination
queue_wait_timeout_secs: 600
container_start_timeout_secs: 180
poll_interval_secs: 30

# Logging
log_tail_lines: 100
error_patterns_sagips_stderr:
  - "Traceback"
  - "ValueError"
  - "IndexError"
  - "srun: error"
  - "Error executing job"
error_patterns_ersap:
  - "ERROR"
  - "Exception"
  - "BIND MOUNT FAILED"
  - "Unable"
  - "Segmentation fault"
```

### `default_ersap_services.yaml`

Matches the format of the production `services.yaml` in `ersap-data/config/`:

```yaml
io-services:
  reader:
    class: org.jlab.ersap.actor.coda.engine.UniAdapterSourceEngine
    name: UniAdapter
  writer:
    class: org.jlab.ersap.actor.coda.engine.CodaSinkEngine
    name: Sink
services:
  - class: HaidisGluexActor
    name: haidis
    lang: cpp
  - class: HaidisGluexLinkActor
    name: hlink
    lang: cpp
configuration:
  io-services:
    reader:
    writer:
  services:
    haidis:
      et_filename: "/tmp/et_sys"
      et_host: "localhost"
      et_port: 23911
      station_name: "haidis"
      verbose: false
    hlink:
      enable_shmem_write: true
      batch_size: 10
      data_id_number: 3
      shmem_name: haidis_shmem
      sem_name: haidis_sem
      sem_ack_name: haidis_sem_ack
      verbose: true
mime-types:
  - binary/sint32
  - binary/array-double
```

A run profile can override any nested key under `configuration.services`.

### `default_sagips.yaml`

Kept in sync with `src/haidis_ips/cfg/sagips.yaml` in `haidis-ips`. Not
mounted into the container — serves as documentation of defaults so a user
can see what `sagips_overrides` is changing.

Commonly overridden keys via `sagips_overrides` in the run profile:

```yaml
sagips_overrides:
  opt.n_epochs: 50
  opt.noise_dim: 100
  dataloader.dataset.shm_name: haidis_shmem     # must match ERSAP hlink.shmem_name
  dataloader.dataset.sem_name: haidis_sem        # must match ERSAP hlink.sem_name
  dataloader.dataset.sem_ack_name: haidis_sem_ack
  dataloader.dataset.selection: "[2, 1]"
```

---

## Required Changes to Sibling Repos

### 1. SAGIPS (`haidis-ips`) — epoch-completion signal

Add one line to `dalitz_shmem_workflow.py` after `opt.fit()` completes,
gated on rank 0:

```python
if rank == 0:
    logger.info(
        f"HAIDIS TRAINING COMPLETE: epochs={config.opt.n_epochs}/{config.opt.n_epochs}"
    )
```

`monitor.py` detects it with:

```python
re.search(r'HAIDIS TRAINING COMPLETE: epochs=(\d+)/(\d+)', text)
```

### 2. SAGIPS (`haidis-ips`) — Hydra override passthrough in `perlmutter_cmd.sh`

Required one-line change in the container entrypoint:

```bash
uv run src/haidis_ips/dalitz_shmem_workflow.py -cn sagips ${SAGIPS_HYDRA_OVERRIDES:-}
```

### 3. `haidis_slurm.sh` — accept per-run config env vars

**ERSAP container launch** — add conditional bind mount:

```bash
ERSAP_CONFIG_MOUNT=""
if [[ -n "${ERSAP_CONFIG_DIR:-}" ]]; then
    ERSAP_CONFIG_MOUNT="-v ${ERSAP_CONFIG_DIR}:/user_data/config"
fi

podman-hpc run --network=host ... $ERSAP_CONFIG_MOUNT \
    -v ${SCRIPT_DIR}/ersap-data:/user_data \
    ...
```

The per-run config dir mounts on top of the default; podman resolves
path precedence by mount order — the more specific path wins.

**SAGIPS container launch** — pass `SAGIPS_HYDRA_OVERRIDES` explicitly:

```bash
podman-hpc run ... -e SAGIPS_HYDRA_OVERRIDES="${SAGIPS_HYDRA_OVERRIDES:-}" \
    ${SAGIPSIMAGE} bash -c /app/scripts/perlmutter_cmd.sh
```

---

## Implementation Phases

### Phase 0 — Test infrastructure (CPU-only, no SAGIPS)

Goal: a lightweight end-to-end test vehicle that exercises ERSAP + the full
`haidis-run` monitoring stack on CPU nodes, without requiring GPU allocation
or a trained SAGIPS model.

**Deliverable 1: `sbatch/haidis_cpu_test.sh`**

A new sbatch script, derived from `haidis_slurm.sh`, with these differences:

| Aspect | `haidis_slurm.sh` | `haidis_cpu_test.sh` |
| --- | --- | --- |
| Node constraint | `--constraint=gpu` | `--constraint=cpu` |
| GPU directives | `--gpus-per-node=4`, `--gpu-bind=none` | removed |
| Phase 3 | SAGIPS via multi-node GPU srun | Python shmem reader via srun (container or direct process — TBD) |
| MPI / `--mpi` flag | yes | no |
| Typical queue | `debug` (GPU) | `debug` or `shared` (CPU) |

The script keeps the same structure as `haidis_slurm.sh`:

- Phase 1: LB reservation check (identical)
- Phase 2: ERSAP container launch via srun (identical, minus GPU flags)
- Phase 3: Python shmem reader via `srun --ntasks=1 --ntasks-per-node=1` — whether it runs as a container (`podman-hpc run` without `--mpi`) or as a direct process is TBD
- Same `ERSAP_CONFIG_DIR` injection mechanism
- Same log paths and job directory layout so `haidis-run` monitoring is unchanged

**Deliverable 2: `scripts/shmem_reader.py`**

A Python script that connects to the ERSAP shared memory segment and reads
data in a loop. It emits the same log signals as SAGIPS so that `monitor.py`
works unchanged:

- Readiness signal: `Waiting for data (sample 1)` — emitted once the shmem segment is attached and before the first read
- Completion signal: `HAIDIS TRAINING COMPLETE: epochs=N/N` — emitted after `--iterations` batches have been read

Implementation details will be provided separately.

**Impact on `haidis-run`:**

Add a `--sbatch-script PATH` option (and a `sbatch_script` run profile key)
that overrides the default `sbatch/haidis_slurm.sh`. All other phases —
queue monitoring, readiness, sender, log tailing, report — work unchanged.

### Phase 1 — SSH backend (MVP, full SAGIPS)

Goal: fully working end-to-end run via SSH, all 8 run phases automated.
Uses Phase 0 infrastructure for development and integration testing before
GPU allocation is needed.

Steps:

1. `pyproject.toml` + package skeleton
2. `config.py`: profile loading, deep-merge, snapshot save, consistency validation
3. `backends/base.py`: `JobBackend` Protocol + `JobState` enum
4. `backends/ssh.py`: submit, poll, tail, put via Fabric
5. `perlmutter.py`: submit_haidis_job, wait_for_running, wait_for_readiness
6. `sender.py`: start, stop, liveness check
7. `monitor.py`: all pattern-matching functions + unit tests
8. `report.py`: log collection, formatting, save
9. `cli.py`: full orchestration, `--dry-run`, `--sbatch-script`
10. `configs/` templates: `default_run_profile.yaml`, `default_ersap_services.yaml`, `default_sagips.yaml`
11. Coordinate `haidis-ips` changes: epoch-completion signal, `perlmutter_cmd.sh` Hydra passthrough
12. Apply `haidis_slurm.sh` changes: `ERSAP_CONFIG_DIR` bind mount, `SAGIPS_HYDRA_OVERRIDES` passthrough
13. Integration test with Phase 0 test script (CPU nodes, fast)
14. Integration test with full GPU job (final validation)

### Phase 2 — IRI backend (option)

Goal: all Phase 1 operations work identically with `--backend iri`.

Dependencies:

- Resolve IRI API 401 errors (NERSC support ticket — see CONTEXT.md known issue)
- `iri_run.api` already has `submit_job`, `poll_job`, `read_file_chunk`

Steps:

1. `backends/iri.py`: implement all `JobBackend` methods using `iri_run.api`
2. `cli.py`: add `--backend iri` flag and `IRIBackend` instantiation
3. Integration test: run against debug queue via IRI

### Phase 3 — Polish

- Structured JSON progress output (`--json`) for machine consumption
- `haidis-run status <run-id>` to query a submitted but un-waited job
- `haidis-run logs <run-id>` to re-fetch logs for a completed run
- `haidis-run list` to show local run history

---

## Dependencies

```toml
[project]
dependencies = [
    "fabric>=3.2",       # SSH + SFTP; pulls in paramiko
    "pyyaml>=6.0",       # config parsing
    "requests>=2.28",    # for IRI backend (already a dep of iri-run)
]

[project.optional-dependencies]
dev = ["pytest>=7.0", "pytest-mock>=3.0"]
iri = ["globus-sdk>=3.0"]   # only needed for --backend iri
```

Fabric 3.x uses Paramiko under the hood; no direct Paramiko imports needed.

---

## Key Design Decisions

| Decision | Rationale |
| --- | --- |
| Fabric over asyncssh | Simpler API, better SFTP support, standard choice for automation scripts; async not needed |
| SSHBackend owns a single long-lived `Connection` | Avoids per-operation handshake overhead; Fabric reconnects automatically on timeout |
| `tail_file` via SFTP byte offset | More reliable than `tail -n` SSH; tracks exact position, avoids re-reading old content |
| Deep-merge for ERSAP YAML | ERSAP config is structured; only changed keys should be in the override |
| Hydra overrides as CLI string, not a mounted config file | SAGIPS config is in the bind-mounted source tree; Hydra CLI overrides via env var is idiomatic and requires no new bind mounts |
| Deep overlay bind mount for ERSAP config | Mounting only the config subdirectory is cleaner than duplicating the full ersap-data tree per run; confirmed working with podman-hpc |
| Epoch signal in SAGIPS stdout | stdout already flows to `slurm-JOB_ID.out` which is already polled; no extra log file needed |
| Phase 0 reader via srun (not head node) | Keeps the script structure parallel to the SAGIPS step, reducing variance between test and production paths |
| No rich/click — plain argparse + print | Keeps the dependency footprint minimal, consistent with iri-run |
