# HAIDIS Run Controller — Requirements

**Status:** Draft v0.2 — updated with concrete config structures  
**Scope:** `haidis-run` Python package in `haidis-run/` subdirectory of this repo

---

## Overview

`haidis-run` is a CLI tool that automates the full lifecycle of a HAIDIS
experimental run: submitting the SLURM job on Perlmutter, verifying container
readiness, activating the data sender, monitoring training progress, and
collecting logs on completion or failure. It is run from the user's laptop.

The existing `haidis-launch` Claude skill covers the same workflow
interactively; `haidis-run` replaces the human-in-the-loop with a scripted,
fully automated controller.

---

## Preconditions

These are assumed true before any run; `haidis-run` checks them at startup but
does not provision them.

| ID | Precondition |
|----|--------------|
| PC-001 | User has passwordless SSH access to a Perlmutter login node (key or sshproxy) |
| PC-002 | User has passwordless SSH key access to the sender host |
| PC-003 | NERSC CFS working directory `/global/cfs/cdirs/<account>/haidis/` exists and contains `sbatch/haidis_slurm.sh` and the `ersap-data/` tree, including `ersap-data/config/services.yaml` |
| PC-004 | Parameterized sender script already exists on the sender host at a known path |
| PC-005 | EJFAT load balancer reservation already exists (not managed by this tool) |
| PC-006 | SAGIPS (`dalitz_shmem_workflow.py`, rank 0) emits the epoch-completion log signal (see FR-029; requires a one-line change in `haidis-ips` — tracked separately) |
| PC-007 | Globus CLI is installed on the laptop if the IRI backend is used |
| PC-008 | `haidis_slurm.sh` has been updated to accept `ERSAP_CONFIG_DIR` and `SAGIPS_HYDRA_OVERRIDES` env vars (see CR-005; requires a sbatch script change — tracked separately) |

---

## Functional Requirements

### Phase 0 — Pre-flight checks

| ID | Requirement |
|----|-------------|
| FR-001 | Verify SSH connectivity to Perlmutter login node; fail with a clear error if not reachable |
| FR-002 | Query the NERSC status page and warn (but do not block) if Perlmutter is degraded; stop if it is fully down |
| FR-003 | Verify the EJFAT load balancer reservation is valid by running `lbadm --status` on Perlmutter via SSH |
| FR-004 | Verify SSH connectivity to the sender host |

### Phase 1 — Configuration preparation

| ID | Requirement |
|----|-------------|
| FR-005 | Load a run profile from a user-supplied YAML file; fall back to built-in defaults for any unspecified values |
| FR-006 | Accept per-run overrides as CLI arguments (EJFAT URI, node count, account, queue, SLURM time limit, epoch target, wall-time timeout, sender parameters) |
| FR-007 | Generate a unique run ID (ISO-8601 timestamp + 6-char hex, e.g. `20260526T1430-a3f2b1`) |
| FR-008 | Save a complete configuration snapshot to `runs/<run-id>/` locally: merged ERSAP YAML, merged SAGIPS `.env`, and a `run_params.json` with all job parameters |
| FR-009 | EJFAT URI must not be written to any local file; it must be passed only at runtime (CLI arg or `EJFAT_URI` env var) |
| FR-010 | Sync the configuration snapshot to Perlmutter CFS at `/global/cfs/cdirs/<account>/haidis/runs/<run-id>/` before job submission so the sbatch script can reference it |

### Phase 2 — Job submission

| ID | Requirement |
|----|-------------|
| FR-011 | Submit the SLURM job via SSH (primary) or IRI API (optional, selected by `--backend iri`); the interface between the controller and the backend is abstracted so both paths produce the same observable behavior |
| FR-012 | Pass job parameters to sbatch: node count, account, queue/QOS, time limit, EJFAT_URI env var, PODMANHPC_ADDITIONAL_STORES, config file paths |
| FR-013 | Extract and record the SLURM job ID from `Submitted batch job <id>` output |
| FR-014 | Record the submit timestamp in `runs/<run-id>/run_params.json` |

### Phase 3 — Queue monitoring

| ID | Requirement |
|----|-------------|
| FR-015 | Poll `squeue` every 10 seconds until the job reaches `RUNNING` state |
| FR-016 | Report each intermediate state (`PENDING`, `CONFIGURING`, etc.) to the user as it changes |
| FR-017 | Record the node list when the job enters `RUNNING` state |
| FR-018 | Fail with a diagnostic message if the job transitions to `FAILED`, `TIMEOUT`, or `CANCELLED` before `RUNNING` |
| FR-019 | Fail if the job has not reached `RUNNING` within a configurable queue-wait timeout (default: 600 s) |

### Phase 4 — Container readiness

| ID | Requirement |
|----|-------------|
| FR-020 | Tail ERSAP per-node logs (`runs/slurm_job_<id>/ersap_<node>.log`) and watch for `All services configured on <node>` — one match required per node |
| FR-021 | Tail SAGIPS stdout (`runs/slurm-<id>.out`) and watch for `Waiting for data (sample 1)` |
| FR-022 | Declare readiness only when all nodes' ERSAP signals are present AND the SAGIPS readiness signal is present |
| FR-023 | Poll both log sources every 15 seconds; ERSAP logs may not appear until ~30 s after the job enters RUNNING — tolerate absence without error for the first 60 s |
| FR-024 | Fail if readiness is not achieved within a configurable container-start timeout (default: 180 s after `RUNNING`) |

### Phase 5 — Sender activation

| ID | Requirement |
|----|-------------|
| FR-025 | SSH to the sender host and invoke the parameterized sender script with the relevant arguments (EJFAT URI, dataset path, rate, MTU, etc.) |
| FR-026 | The sender script path and default arguments are specified in the run profile; EJFAT URI is always passed at runtime |
| FR-027 | Capture or monitor the sender process so that its liveness can be checked during the run |
| FR-028 | Record the sender-start timestamp |

### Phase 6 — Run monitoring

| ID | Requirement |
|----|-------------|
| FR-029 | Detect epoch-completion by scanning SAGIPS stdout for the signal `HAIDIS TRAINING COMPLETE: epochs=<N>/<M>` where N == M. This signal is emitted by rank 0 in `dalitz_shmem_workflow.py` after `opt.fit()` completes. Proposed addition: `logger.info(f"HAIDIS TRAINING COMPLETE: epochs={config.opt.n_epochs}/{config.opt.n_epochs}")` (requires a `haidis-ips` change — tracked separately). |
| FR-030 | Detect wall-time expiry: terminate the run if `--timeout` seconds have elapsed since sender activation (default: no timeout) |
| FR-031 | On every poll cycle, scan SAGIPS stderr (`runs/slurm-<id>.err`) for error patterns: `Traceback`, `ValueError`, `IndexError`, `srun: error`, `Error executing job`; surface them immediately |
| FR-032 | On every poll cycle, scan ERSAP logs for error patterns: `ERROR`, `Exception`, `BIND MOUNT FAILED`, `Unable`, `Segmentation fault` |
| FR-033 | Check sender liveness on every poll cycle; alert if the sender process has died unexpectedly |
| FR-034 | Report progress to the user at a configurable interval (default: 30 s); include wall-clock elapsed time, any new log lines of interest |

### Phase 7 — Termination

| ID | Requirement |
|----|-------------|
| FR-035 | On epoch-completion signal: SSH to sender host and stop the sender script gracefully; then wait for SAGIPS to drain and exit (detect job leaving `RUNNING` state or stdout showing "SAGIPS srun completed") |
| FR-036 | On wall-time timeout: stop the sender the same way, log a warning that max time was reached |
| FR-037 | On unrecoverable error: stop the sender and immediately proceed to failure collection (Phase 8) |
| FR-038 | Record run duration and final outcome in `runs/<run-id>/run_params.json` |

### Phase 8 — Failure collection and reporting

| ID | Requirement |
|----|-------------|
| FR-039 | On any failure, fetch the last 100 lines of: SAGIPS stdout, SAGIPS stderr, and each ERSAP per-node log |
| FR-040 | Present a structured failure report to the user: phase where failure occurred, timestamp, detected error lines, and paths to full log files on NERSC CFS |
| FR-041 | Save the failure report to `runs/<run-id>/failure_report.txt` locally |
| FR-042 | Exit with a non-zero return code on any failure |

---

## Configuration Requirements

| ID | Requirement |
|----|-------------|
| CR-001 | Default configuration templates for ERSAP `services.yaml` and SAGIPS `sagips.yaml` are shipped with the package in `haidis-run/configs/`; a `default_run_profile.yaml` provides defaults for all job parameters |
| CR-002 | A run profile YAML specifies: Perlmutter login host, NERSC account, queue/QOS, SLURM time limit, node count, sender host, sender script path, default sender args, container images, readiness/queue-wait timeouts, poll interval, epoch target; plus `ersap_overrides` and `sagips_overrides` sub-sections |
| CR-003 | **ERSAP config**: the template is a `services.yaml` in the same format as `/global/cfs/cdirs/<account>/ersap-data/config/services.yaml`. The `ersap_overrides` section in the run profile is deep-merged onto the template. The merged result is written to `runs/<run-id>/ersap/config/services.yaml` locally and synced to the same path on NERSC CFS. The sbatch script mounts `<job_dir>/ersap/config` at `/user_data/config` inside the ERSAP container (overriding the default ersap-data config subdirectory). Commonly overridden keys: `configuration.services.hlink.batch_size`, `configuration.services.hlink.data_id_number`, `configuration.services.haidis.et_port` |
| CR-004 | **SAGIPS config**: SAGIPS uses Hydra; overrides are passed as CLI arguments to `perlmutter_cmd.sh` (e.g. `opt.n_epochs=100 dataloader.dataset.shm_name=my_shmem`). The `sagips_overrides` section in the run profile is a flat dict of Hydra key-value pairs that are appended to the command. The full set of overrides is written to `runs/<run-id>/sagips_overrides.txt` for auditability. Commonly overridden keys: `opt.n_epochs`, `opt.noise_dim`, `dataloader.dataset.shm_name`, `dataloader.dataset.sem_name`, `dataloader.dataset.selection` |
| CR-005 | The sbatch script (`haidis_slurm.sh`) must be updated to: (a) accept `ERSAP_CONFIG_DIR` env var and add a bind mount `-v $ERSAP_CONFIG_DIR:/user_data/config` to the ERSAP container command when set; (b) accept `SAGIPS_HYDRA_OVERRIDES` env var and append its value to the `perlmutter_cmd.sh` invocation |
| CR-006 | **Config consistency**: the following three names must be identical between the ERSAP `services.yaml` and the SAGIPS Hydra overrides — `haidis-run` validates all three before submission and fails with a clear error listing both values if any pair mismatches: (1) shared memory segment: ERSAP `configuration.services.hlink.shmem_name` ↔ SAGIPS `dataloader.dataset.shm_name`; (2) primary semaphore: ERSAP `configuration.services.hlink.sem_name` ↔ SAGIPS `dataloader.dataset.sem_name`; (3) ack semaphore: ERSAP `configuration.services.hlink.sem_ack_name` ↔ SAGIPS `dataloader.dataset.sem_ack_name` |
| CR-007 | Secrets (EJFAT URI, auth tokens) must never appear in snapshot files or be committed to git |

---

## Non-Functional Requirements

| ID | Requirement |
|----|-------------|
| NFR-001 | Python 3.9+ compatible (matches iri-run) |
| NFR-002 | Installable as a pip package via `pip install -e .` |
| NFR-003 | All SSH operations use the Fabric library (primary); Paramiko must not be called directly |
| NFR-004 | The IRI API backend must be implemented behind the same `JobBackend` abstraction used by the SSH backend, so the two are interchangeable at the CLI level |
| NFR-005 | The tool must be usable without Globus CLI installed when using the SSH backend |
| NFR-006 | No use of `/tmp` on NERSC for any output — all paths use CFS |
| NFR-007 | Log polling must tolerate transient SSH errors (retry up to 3 times before surfacing the error) |
| NFR-008 | All remote file operations on NERSC use SFTP (Fabric's `get`/`put`) rather than shell redirection |

---

## Out of Scope

- Provisioning the NERSC CFS working directory, sbatch scripts, or `ersap-data/`
- Managing or creating EJFAT load balancer reservations
- Installing or building container images on Perlmutter
- SAGIPS training algorithm changes (only the termination signal is needed)
- Multi-cluster or cross-facility operation
- A web UI or GUI
- Job cancellation if the user interrupts the controller mid-run (best-effort only)

---

## Open Questions

1. What is the exact hostname and username for the sender host? (Needed for the default run profile template)
2. What is the exact path and signature of the parameterized sender script on the sender host, and what arguments does it accept?
3. Should `haidis-run` cancel the SLURM job on fatal failure, or leave it running for manual inspection?
4. What is the `perlmutter_cmd.sh` script inside the SAGIPS container image doing — specifically, does it already support passing Hydra CLI overrides via an env var, or does it need to be modified?
5. For the epoch-completion signal in `dalitz_shmem_workflow.py`: should rank 0 emit it, or all ranks? What if a rank fails — should we detect partial completions?
