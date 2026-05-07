# iri-run: Context and Reference

## Overview

`iri-run` is a Python project providing CLI tools for submitting and monitoring
SLURM jobs on NERSC Perlmutter via the IRI (Integrated Research Infrastructure)
REST API. Two commands are provided:

- **`iri-bash`**: Simplified tool for running bash commands on NERSC interactive
  nodes via the "cron" QOS. Wraps commands in `bash -c "..."`.
- **`iri-run`**: Lower-level tool with full control over SLURM job parameters.

## DOE Integrated Research Infrastructure (IRI)

IRI is a DOE program to seamlessly connect experimental facilities with HPC
centers. It grew out of NERSC's Superfacility Project (2019-2022). Six pillars:

1. Quality of Service
2. Seamlessness
3. Programmability and Automation (APIs)
4. Orchestration
5. Portability (works across NERSC, ALCF, ESnet)
6. Security (Globus-based federated identity)

### Live Deployments

| Facility | URL |
|----------|-----|
| NERSC | `https://api.iri.nersc.gov` |
| ALCF | `https://api.alcf.anl.gov` |
| ESnet | `https://iri-dev.ppg.es.net` |

### Reference Implementation

https://github.com/doe-iri/iri-facility-api-python -- FastAPI-based Python
server (Python 3.12+) using an adapter pattern for facility-specific backends.

## NERSC IRI API

- **Base URL**: `https://api.iri.nersc.gov/api/v1`
- **OpenAPI spec**: `https://api.iri.nersc.gov/openapi.json`
- **Swagger UI**: `https://api.iri.nersc.gov/#/`
- **API version**: 1.0.0 (OpenAPI 3.1.0)
- **Warning**: "This API is not final and may change at any time."
- **Auth**: HTTP Bearer via Globus
- **Error format**: RFC 7807 `Problem` (type, status, title, detail, instance)

### All Endpoints (29 total, 6 groups)

#### Facility (3 endpoints, public)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/facility` | Get facility info |
| GET | `/facility/sites` | List sites (params: name, short_name, offset, limit, modified_since) |
| GET | `/facility/sites/{site_id}` | Get a specific site |

#### Account (8 endpoints, authenticated)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/account/capabilities` | List capabilities (params: name, offset, limit, modified_since) |
| GET | `/account/capabilities/{capability_id}` | Get a capability |
| GET | `/account/projects` | List user's projects |
| GET | `/account/projects/{project_id}` | Get a project |
| GET | `/account/projects/{project_id}/project_allocations` | List allocations for a project |
| GET | `.../{project_allocation_id}` | Get a specific allocation |
| GET | `.../{project_allocation_id}/user_allocations` | List user allocations |
| GET | `.../{project_allocation_id}/user_allocations/{user_allocation_id}` | Get a user allocation |

#### Status (6 endpoints, public)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/status/resources` | List resources (params: name, group, resource_type, current_status, capability, offset, limit) |
| GET | `/status/resources/{resource_id}` | Get a resource |
| GET | `/status/incidents` | List incidents (params: name, status, type, from, to, resource_id, offset, limit) |
| GET | `/status/incidents/{incident_id}` | Get an incident |
| GET | `/status/events` | List events (params: incident_id, resource_id, status, from, to, offset, limit) |
| GET | `/status/events/{event_id}` | Get an event |

#### Compute (5 endpoints, authenticated)

| Method | Path | Description |
|--------|------|-------------|
| POST | `/compute/job/{resource_id}` | Submit a job (body: JobSpec) |
| PUT | `/compute/job/{resource_id}/{job_id}` | Update a job (body: JobSpec) |
| GET | `/compute/status/{resource_id}/{job_id}` | Get job status (params: historical, include_spec) |
| POST | `/compute/status/{resource_id}` | List/filter jobs (params: offset, limit, historical, include_spec; body: filter object) |
| DELETE | `/compute/cancel/{resource_id}/{job_id}` | Cancel a job (returns 204) |

Pagination on job listing: `offset` (default 0), `limit` (default 100, max 1000).

#### Filesystem (18 endpoints, ALL async -- return TaskSubmitResponse)

| Method | Path | Description | Key params |
|--------|------|-------------|------------|
| GET | `/filesystem/checksum/{resource_id}` | Checksum a file | path |
| PUT | `/filesystem/chmod/{resource_id}` | Change permissions | body: {path, mode} |
| PUT | `/filesystem/chown/{resource_id}` | Change ownership | body: {path, owner, group} |
| POST | `/filesystem/compress/{resource_id}` | Compress files | body: {path, target_path, match_pattern, dereference, compression} |
| POST | `/filesystem/cp/{resource_id}` | Copy file/dir | body: {path, target_path, dereference} |
| GET | `/filesystem/download/{resource_id}` | Download a file | path |
| POST | `/filesystem/extract/{resource_id}` | Extract archive | body: {path, target_path, compression} |
| GET | `/filesystem/file/{resource_id}` | Get file type info | path |
| GET | `/filesystem/head/{resource_id}` | Read from start | path, bytes, lines, skipTrailing |
| GET | `/filesystem/ls/{resource_id}` | List directory | path, showHidden, numericUid, recursive, dereference |
| POST | `/filesystem/mkdir/{resource_id}` | Create directory | body: {path, parent} |
| POST | `/filesystem/mv/{resource_id}` | Move/rename | body: {path, target_path} |
| DELETE | `/filesystem/rm/{resource_id}` | Delete file/dir | path |
| GET | `/filesystem/stat/{resource_id}` | Stat a file | path, dereference |
| POST | `/filesystem/symlink/{resource_id}` | Create symlink | body: {path, link_path} |
| GET | `/filesystem/tail/{resource_id}` | Read from end | path, bytes, lines, skipHeading |
| POST | `/filesystem/upload/{resource_id}` | Upload a file | path (query), body: multipart |
| GET | `/filesystem/view/{resource_id}` | Read file chunk | path, offset (default 0), size (default/max 5242880) |

#### Task (3 endpoints, authenticated)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/task` | List all tasks |
| GET | `/task/{task_id}` | Get task status/result |
| DELETE | `/task/{task_id}` | Delete a task |

The resource ID for Perlmutter is `perlmutter`.

### Schemas

#### Job

```
Job {
  id: string                    # SLURM job ID
  status: JobStatus | null
  job_spec: JobSpec | null      # only if include_spec=true
}

JobStatus {
  state: JobState               # required
  time: number | null           # epoch seconds
  message: string | null
  exit_code: integer | null
  meta_data: object | null      # backend-specific
}

JobState enum: new, queued, held, active, completed, failed, canceled
```

#### JobSpec (PSI/J format)

```
JobSpec {
  name: string | null
  executable: string | null
  arguments: string[]
  directory: string | null
  inherit_environment: bool     # default: true
  environment: object           # key-value pairs
  stdin_path: string | null
  stdout_path: string | null
  stderr_path: string | null
  resources: ResourceSpec | null
  attributes: JobAttributes | null
  launcher: string | null       # "mpirun", "srun", "single"
  container: Container | null   # for containerized execution
  pre_launch: string | null     # inline script run before job
  post_launch: string | null    # inline script run after job
}

ResourceSpec {
  node_count: int | null        # min: 1
  process_count: int | null     # min: 1
  processes_per_node: int | null
  cpu_cores_per_process: int | null
  gpu_cores_per_process: int | null
  exclusive_node_use: bool      # default: true
  memory: int | null            # bytes
}

JobAttributes {
  duration: int | null          # wall time in seconds, min: 1
  queue_name: string | null
  account: string | null
  reservation_id: string | null
  custom_attributes: object     # scheduler-specific, prefixed with "slurm."
}

Container {
  image: string                 # e.g. "docker.io/library/ubuntu:latest"
  volume_mounts: VolumeMount[]
}

VolumeMount {
  source: string                # host path
  target: string                # container path
  read_only: bool               # default: true
}
```

SLURM-specific custom attributes use the `slurm.` prefix (e.g., `slurm.qos`,
`slurm.constraint`, `slurm.partition`). Any SLURM long-form option can be passed
this way -- they translate to `#SBATCH --<key>=<value>` directives.

#### Task

```
Task {
  id: string
  status: TaskStatus            # default: "pending"
  result: object | null         # operation output (varies per endpoint)
  command: TaskCommand | null
}

TaskCommand {
  router: string
  command: string
  args: object
}

TaskSubmitResponse {
  task_id: string
  task_uri: string              # full URL to poll
}

TaskStatus enum: pending, active, completed, failed, canceled
```

#### Other Enums

- `AllocationUnit`: node_hours, bytes, inodes
- `CompressionType`: none, bzip2, gzip, xz
- `IncidentType`: planned, unplanned, reservation
- `Resolution`: unresolved, cancelled, completed, extended, pending
- `ResourceType`: website, service, compute, system, storage, network, unknown
- `Status`: up, down, degraded, unknown

### Error Responses

All errors use RFC 7807 `Problem` format:

```json
{
  "type": "about:blank",
  "status": 401,
  "title": "Unauthorized",
  "detail": "Token expired",
  "instance": "/api/v1/compute/job/perlmutter"
}
```

HTTP codes used: 400, 401, 403, 404, 405, 409, 422, 500, 501, 503, 504.

### Async Filesystem Operations

All 18 filesystem endpoints are asynchronous. They return a `TaskSubmitResponse`:
```json
{"task_id": "abc123", "task_uri": "https://api.iri.nersc.gov/api/v1/task/abc123"}
```

Poll `GET /task/{task_id}` until `status` is `completed`, `failed`, or
`canceled`. The `result` field of a completed task contains the operation output
(structure varies per operation -- not formally documented in the OpenAPI spec).

### File View (for streaming output)

`GET /filesystem/view/{resource_id}?path=...&offset=0&size=5242880`

- `offset` (int, default 0): byte offset into the file
- `size` (int, default/max 5242880): bytes to read
- Supports progressive reading by incrementing offset

### File Deletion

`DELETE /filesystem/rm/{resource_id}?path=...`

This is an async operation (returns TaskSubmitResponse). Can be used to clean up
job output directories.

## Authentication

### Globus OAuth2 Flow

The IRI API uses Globus Auth for authentication:
- **Resource Server ID**: `ed3e577d-f7f3-4639-b96e-ff5a8445d699`
- **Scope**: `https://auth.globus.org/scopes/ed3e577d-f7f3-4639-b96e-ff5a8445d699/iri_api`
- **Auth header**: `Authorization: Bearer <access_token>`
- **Token lifetime**: ~48 hours for access tokens; refresh tokens last 6 months of inactivity

### Using globus-cli

```bash
# One-time setup
pipx install globus-cli
globus login
globus session consent https://auth.globus.org/scopes/ed3e577d-f7f3-4639-b96e-ff5a8445d699/iri_api
```

The `iri-run` tools automate these steps -- on first run, `load_token()` detects
missing tokens and runs `globus login` / `globus session consent` on the user's
behalf.

### Token Storage

Tokens are stored in the globus-cli's SQLite database at
`~/.globus/cli/storage.db`.

**Critical**: The namespace is `"userprofile/production"` -- you MUST pass this
to `SQLiteAdapter`/`SQLiteTokenStorage` or you'll get an empty result.

```python
# globus-sdk v4.x (legacy adapter, schema version 1)
from globus_sdk.token_storage.legacy import SQLiteAdapter as SQLiteTokenStorage

store = SQLiteTokenStorage(str(db_path), namespace="userprofile/production")
token_data = store.get_token_data("ed3e577d-f7f3-4639-b96e-ff5a8445d699")
# Returns dict with access_token, refresh_token, expires_at_seconds, etc.
```

### Known Issue: 401 "Invalid token"

As of 2026-04-02, tokens obtained via globus-cli for the IRI scope are rejected
by the IRI API with 401 "Invalid token". This may indicate that:
1. The user's NERSC account needs to be enrolled for IRI API access
2. The IRI API requires tokens from a specific registered Globus app

A NERSC developer's gist (https://gist.github.com/dingp/347b99840d9b3ff2553ee53f47f0bf07)
uses native app client ID `fae5c579-490a-4d76-b6eb-d78f65caeb63`, but tokens
from this client also produce the same 401 error. This needs to be resolved
with NERSC support (https://help.nersc.gov/).

## NERSC Perlmutter

### QOS Types

#### GPU QOS

| QOS | Max Nodes | Max Wall | Charge Factor |
|-----|-----------|----------|---------------|
| regular | Unlimited | 48h | 1 |
| debug | 8 | 30min | 1 |
| interactive | 4 | 4h | 1 |
| shared | 0.5 | 48h | 1 |
| shared_interactive | 0.5 | 4h | 1 |
| jupyter | 4 | 6h | 1 |
| preempt | 128 | 48h | 0.25 |
| premium | -- | 48h | 2-4 |
| overrun | -- | 48h | 0 |

#### Login Node QOS

| QOS | Max Resources | Max Wall | Charge |
|-----|---------------|----------|--------|
| xfer | 1 login node | 48h | Free |
| **cron** | 1/128 login node | 24h | Free |
| **workflow** | 1/4 login node | 90 days | Free |

### Cron QOS Details

**Important**: `cron` is both a QOS name and a constraint value. The standard
NERSC pattern uses `-q workflow -C cron` with `scrontab`. However, when
submitting via the IRI API, we use `slurm.qos: cron` which runs jobs on
login/interactive nodes without requiring scrontab.

Key characteristics:
- Runs on login nodes (not compute farm nodes)
- Uses 1/128 of a login node
- Max wall time: 24 hours per job
- Free (no allocation charge)
- Useful for orchestration, automation, and launching further jobs

### Filesystems

| Filesystem | Path | Purpose | Persistence |
|------------|------|---------|-------------|
| Home | `/global/homes/<initial>/<username>` (`$HOME`) | Scripts, config | Permanent, snapshotted |
| CFS | `/global/cfs/cdirs/<project>/` | Shared project data | Permanent, snapshotted |
| Scratch | `/pscratch/sd/<initial>/<username>` (`$SCRATCH`) | Temporary job I/O | 8-week purge |
| Common | `/global/common/` | Software stacks | Read-only on compute |

### CFS and Project Accounts

Every NERSC project has a CFS directory at `/global/cfs/cdirs/<project_name>/`.
The project name matches the SLURM account name (e.g., `myproject` ->
`/global/cfs/cdirs/myproject/`).

**Important**: Do NOT use `/tmp` for output files -- it is local to each node
and may differ among machines behind an API fanout.

### Perlmutter Resources

From `GET /status/resources`, the Perlmutter group includes:
- `compute` -- compute nodes
- `login` -- login nodes
- `jobs` -- SLURM
- `realtime` -- urgent/realtime jobs
- `scratch` -- scratch filesystem
- `shifter` -- container runtime

## PSI/J (Portable Submission Interface for Jobs)

PSI/J is a job management abstraction API for portability across HPC schedulers
(SLURM, PBS, LSF, etc.). Developed by ExaWorks. The IRI API adopts PSI/J's
`JobSpec` format. Spec: https://exaworks.org/psij-python/docs/v/0.9.11/.generated/tree.html#jobspec

### IRI Extensions to PSI/J

- `container` field for containerized execution (image + volume_mounts)
- `pre_launch` / `post_launch` as inline script strings (not file paths)
- `attributes.account` instead of `attributes.project_name`
- `resources.memory` field (in bytes)
- `resources.exclusive_node_use` defaults to `true` (PSI/J defaults to `false`)

## Project Context (haidis / e2sar-utils)

The sibling project `../e2sar-utils` uses these NERSC conventions:
- **Account**: `<your-account>` (default CPU/login), `<gpu-account>` (GPU interactive)
- **Working dir**: `/global/cfs/cdirs/<your-account>/haidis/`
- **Job logs**: `/global/cfs/cdirs/<your-account>/haidis/runs/slurm-<ID>.out/.err`
- **sbatch script**: `/global/cfs/cdirs/<your-account>/haidis/sbatch/haidis_slurm.sh`
- **QOS**: `debug` (30-min limit), `cron`, `regular`
- **Constraint**: `gpu` (for GPU nodes), `cpu`
- **Container runtime**: `podman-hpc`

## iri-bash Output Convention

Each `iri-bash` invocation creates a unique working directory:
`/global/cfs/cdirs/{account}/iri-bash/{uuid}/`

Files in that directory:
- `stdout` -- job stdout
- `stderr` -- job stderr

The `iri-bash reap <uuid>` command fetches these files and submits a cleanup job
to remove the directory.

## Development Environment

- **Machine**: RHEL 8 (Linux 4.18.0)
- **System Python**: 3.6.8 (do not use -- too old)
- **Usable Python**: 3.9 (installed via `dnf module install python39`)
- **Package management**: `pipx` for CLI tools, `pip` in venvs for libraries — always create a venv first
- **globus-cli**: installed via `pipx install globus-cli`
- **globus-sdk version**: 4.5.0 (uses legacy `SQLiteAdapter` with schema version 1)

Setup:

```bash
python3 -m venv e2sar-utils && . e2sar-utils/bin/activate
pip install -e .           # or pip install -e ".[dev]" for dev deps
```

## Dependencies

From `pyproject.toml`:
- `requests>=2.28`
- `globus-sdk>=3.0`
- Python >= 3.9
- Build: `setuptools>=68.0`
- Dev: `pytest>=7.0`

CLI entry points:
- `iri-run = "iri_run.cli:main"`
- `iri-bash = "iri_run.bash:main"`

## Existing Python Libraries

| Package | What it does | Auth? |
|---------|-------------|-------|
| `nersc-iri` (PyPI) | Auto-generated OpenAPI client for the IRI API. Very new (2026-03). | No -- needs token passed in |
| `sfapi-client` (PyPI) | Client for the **old** Superfacility API (`api.nersc.gov`). Wrong API. | Yes, but old NERSC OIDC, not Globus |
| `globus-sdk` (PyPI) | Generic Globus OAuth2 machinery. No IRI-specific helpers. | Yes |

We use `requests` + `globus-sdk` (for token storage only) over `nersc-iri`
because the auto-generated client is too new and doesn't have a proper GitHub repo.
