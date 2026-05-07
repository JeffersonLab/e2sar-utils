"""Shared IRI API client, auth, and helpers for all iri-* commands."""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

import requests

API_BASE = "https://api.iri.nersc.gov/api/v1"
RESOURCE = "perlmutter"
FS_RESOURCE = "cfs"
IRI_RESOURCE_SERVER = "ed3e577d-f7f3-4639-b96e-ff5a8445d699"
IRI_SCOPE = f"https://auth.globus.org/scopes/{IRI_RESOURCE_SERVER}/iri_api"
TASK_POLL_INTERVAL = 2
JOB_POLL_INTERVAL = 10
TASK_TIMEOUT = 60


# ---------------------------------------------------------------------------
# Authentication
# ---------------------------------------------------------------------------

def _find_globus_cli() -> str:
    """Locate the globus CLI executable."""
    path = shutil.which("globus")
    if path:
        return path
    local_bin = Path.home() / ".local" / "bin" / "globus"
    if local_bin.is_file():
        return str(local_bin)
    print(
        "Error: globus-cli not found. Install it with: pipx install globus-cli",
        file=sys.stderr,
    )
    sys.exit(1)


def _run_globus(globus: str, args: list[str]) -> None:
    """Run a globus CLI command, inheriting the terminal for interactive auth."""
    print(f"Running: globus {' '.join(args)}", file=sys.stderr)
    result = subprocess.run([globus] + args)
    if result.returncode != 0:
        print(f"Error: 'globus {args[0]}' failed (exit {result.returncode})", file=sys.stderr)
        sys.exit(1)


def _load_token_from_store() -> dict | None:
    """Try to read IRI token data from the globus-cli SQLite store."""
    try:
        from globus_sdk.tokenstorage import SQLiteTokenStorage
    except ImportError:
        from globus_sdk.token_storage.legacy import SQLiteAdapter as SQLiteTokenStorage

    db_path = Path.home() / ".globus" / "cli" / "storage.db"
    if not db_path.is_file():
        return None

    try:
        storage = SQLiteTokenStorage(str(db_path), namespace="userprofile/production")
        token_data = storage.get_token_data(IRI_RESOURCE_SERVER)
    except Exception:
        return None

    if not token_data:
        return None

    access_token = token_data.get("access_token", "")
    if not access_token:
        return None

    expires_at = token_data.get("expires_at_seconds", 0)
    if expires_at and time.time() >= expires_at:
        return None

    return token_data


def load_token() -> str:
    """Read the IRI API access token, running globus login/consent if needed."""
    globus = _find_globus_cli()

    token_data = _load_token_from_store()
    if token_data:
        return token_data["access_token"]

    db_path = Path.home() / ".globus" / "cli" / "storage.db"
    if not db_path.is_file():
        print("No Globus session found. Logging in...", file=sys.stderr)
        _run_globus(globus, ["login"])

    token_data = _load_token_from_store()
    if not token_data:
        print("Requesting IRI API access consent...", file=sys.stderr)
        _run_globus(globus, ["session", "consent", IRI_SCOPE])

    token_data = _load_token_from_store()
    if not token_data:
        print("Error: Still no IRI token after login/consent.", file=sys.stderr)
        print("Try manually: globus login --force && globus session consent", file=sys.stderr)
        print(f"  {IRI_SCOPE}", file=sys.stderr)
        sys.exit(1)

    return token_data["access_token"]


# ---------------------------------------------------------------------------
# API helpers
# ---------------------------------------------------------------------------

def auth_headers(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def wait_for_task(token: str, task_id: str) -> dict | None:
    """Poll a task until it completes. Returns the result dict or None on failure."""
    deadline = time.monotonic() + TASK_TIMEOUT
    while time.monotonic() < deadline:
        time.sleep(TASK_POLL_INTERVAL)
        try:
            resp = requests.get(
                f"{API_BASE}/task/{task_id}",
                headers=auth_headers(token),
            )
        except requests.RequestException:
            continue
        if not resp.ok:
            continue
        data = resp.json()
        status = data.get("status", "")
        if status == "completed":
            return data.get("result")
        if status in ("failed", "canceled"):
            return None
    return None


def read_file_chunk(token: str, path: str, offset: int, size: int = 1048576) -> str | None:
    """Submit a filesystem view request and wait for the result."""
    try:
        resp = requests.get(
            f"{API_BASE}/filesystem/view/{FS_RESOURCE}",
            headers=auth_headers(token),
            params={"path": path, "offset": offset, "size": size},
        )
    except requests.RequestException:
        return None
    if not resp.ok:
        return None

    data = resp.json()
    task_id = data.get("task_id")
    if not task_id:
        return None

    result = wait_for_task(token, task_id)
    if result is None:
        return None

    if isinstance(result, str):
        return result
    if isinstance(result, dict):
        # The view response nests content inside: {"output": {"content": "..."}}
        output = result.get("output")
        if isinstance(output, dict):
            return output.get("content", "")
        if isinstance(output, str):
            return output
        return result.get("content", "")
    return None


def ensure_remote_dir(token: str, path: str) -> None:
    """Create a directory on NERSC via the filesystem API (best-effort)."""
    try:
        resp = requests.post(
            f"{API_BASE}/filesystem/mkdir/{FS_RESOURCE}",
            headers={**auth_headers(token), "Content-Type": "application/json"},
            json={"path": path, "parent": True},
        )
        if resp.ok:
            task_id = resp.json().get("task_id")
            if task_id:
                wait_for_task(token, task_id)
    except requests.RequestException:
        pass


def remove_remote_path(token: str, path: str) -> None:
    """Delete a file or directory on NERSC via the filesystem API (best-effort)."""
    try:
        resp = requests.delete(
            f"{API_BASE}/filesystem/rm/{FS_RESOURCE}",
            headers=auth_headers(token),
            params={"path": path},
        )
        if resp.ok:
            task_id = resp.json().get("task_id")
            if task_id:
                wait_for_task(token, task_id)
    except requests.RequestException:
        pass


def _dump_request(prepared: requests.PreparedRequest) -> None:
    """Pretty-print an HTTP request to stderr, redacting the token."""
    print(f">>> {prepared.method} {prepared.path_url} HTTP/1.1", file=sys.stderr)
    for k, v in prepared.headers.items():
        if k.lower() == "authorization":
            v = v[:27] + "..."  # "Bearer " (7) + 20 chars of token
        print(f">>> {k}: {v}", file=sys.stderr)
    if prepared.body:
        try:
            import json as _json
            body = _json.loads(prepared.body)
            print(f">>>\n{_json.dumps(body, indent=2)}", file=sys.stderr)
        except (ValueError, TypeError):
            print(f">>>\n{prepared.body}", file=sys.stderr)
    print(file=sys.stderr)


def _dump_response(resp: requests.Response) -> None:
    """Pretty-print an HTTP response to stderr."""
    print(f"<<< HTTP {resp.status_code} {resp.reason}", file=sys.stderr)
    for k, v in resp.headers.items():
        print(f"<<< {k}: {v}", file=sys.stderr)
    try:
        import json as _json
        print(f"<<<\n{_json.dumps(resp.json(), indent=2)}", file=sys.stderr)
    except (ValueError, TypeError):
        print(f"<<<\n{resp.text}", file=sys.stderr)
    print(file=sys.stderr)


def submit_job(token: str, payload: dict, debug: bool = False) -> tuple[str, str]:
    """Submit a job and return (job_id, state)."""
    url = f"{API_BASE}/compute/job/{RESOURCE}"
    req = requests.Request(
        "POST", url,
        headers=auth_headers(token),
        json=payload,
    )
    prepared = req.prepare()

    if debug:
        _dump_request(prepared)

    resp = requests.Session().send(prepared)

    if debug:
        _dump_response(resp)

    if not resp.ok:
        if not debug:
            print(f"Error: API returned HTTP {resp.status_code}", file=sys.stderr)
            try:
                print(resp.json(), file=sys.stderr)
            except ValueError:
                print(resp.text, file=sys.stderr)
        sys.exit(1)

    data = resp.json()
    job_id = data.get("id")
    status = data.get("status") or {}
    state = status.get("state") if isinstance(status, dict) else None

    if not job_id:
        print("Error: No job id in response:", file=sys.stderr)
        print(data, file=sys.stderr)
        sys.exit(1)

    return job_id, state


def poll_job(token: str, job_id: str, stdout_path: str, stderr_path: str) -> None:
    """Poll job status and stream stdout/stderr until completion."""
    import signal

    prev_state = None
    stdout_offset = 0
    stderr_offset = 0

    def on_interrupt(sig, frame):
        print(f"\nInterrupted. Job {job_id} may still be running.", file=sys.stderr)
        sys.exit(130)

    signal.signal(signal.SIGINT, on_interrupt)

    def stream_output():
        nonlocal stdout_offset, stderr_offset

        chunk = read_file_chunk(token, stdout_path, stdout_offset)
        if chunk:
            sys.stdout.write(chunk)
            sys.stdout.flush()
            stdout_offset += len(chunk.encode("utf-8"))

        chunk = read_file_chunk(token, stderr_path, stderr_offset)
        if chunk:
            sys.stderr.write(chunk)
            sys.stderr.flush()
            stderr_offset += len(chunk.encode("utf-8"))

    while True:
        time.sleep(JOB_POLL_INTERVAL)

        try:
            resp = requests.get(
                f"{API_BASE}/compute/status/{RESOURCE}/{job_id}",
                headers=auth_headers(token),
            )
        except requests.RequestException as exc:
            print(f"Warning: Status poll failed: {exc}", file=sys.stderr)
            continue

        if not resp.ok:
            print(f"Warning: Status poll returned HTTP {resp.status_code}", file=sys.stderr)
            continue

        data = resp.json()
        status = data.get("status") or {}
        if isinstance(status, dict):
            state = status.get("state", "unknown")
            exit_code = status.get("exit_code")
        else:
            state = data.get("state", "unknown")
            exit_code = data.get("exit_code")

        if state != prev_state:
            print(f"Job {job_id}: {state}", file=sys.stderr)
            prev_state = state

        stream_output()

        if state == "completed":
            stream_output()
            print(f"Job completed (exit code: {exit_code})", file=sys.stderr)
            return 0 if str(exit_code) == "0" else 1
        elif state in ("failed", "canceled"):
            stream_output()
            print(f"Job {state}", file=sys.stderr)
            return 1
