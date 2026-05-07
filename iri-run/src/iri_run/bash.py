"""CLI entry point for iri-bash: run bash commands on NERSC via the IRI API.

Jobs run under the "cron" QOS on interactive nodes. The user's command is
executed as ``/bin/bash -c "<script>"``, behaving as if it were run in an
interactive shell on NERSC.
"""

from __future__ import annotations

import argparse
import sys
import uuid

import requests

from iri_run.api import (
    API_BASE,
    RESOURCE,
    auth_headers,
    load_token,
    poll_job,
    read_file_chunk,
    remove_remote_path,
    submit_job,
)

DEFAULT_ACCOUNT = "myproject"
DEFAULT_TIME_MINUTES = 30
CFS_BASE = "/global/cfs/cdirs"


def _job_dir(account: str, job_uuid: str) -> str:
    return f"{CFS_BASE}/{account}/iri-bash/{job_uuid}"


def _parse_run_args(parser: argparse.ArgumentParser) -> None:
    """Add arguments for the run (default) subcommand."""
    parser.add_argument(
        "-A", "--account", default=DEFAULT_ACCOUNT,
        help=f"SLURM account/project (default: {DEFAULT_ACCOUNT})",
    )
    parser.add_argument(
        "-t", "--time", type=int, default=DEFAULT_TIME_MINUTES, dest="time_minutes",
        help=f"Wall time in minutes (default: {DEFAULT_TIME_MINUTES})",
    )
    parser.add_argument(
        "--no-wait", action="store_true",
        help="Submit and exit immediately (print job UUID instead of waiting)",
    )
    parser.add_argument(
        "--debug", action="store_true",
        help="Print HTTP request and response details",
    )
    parser.add_argument(
        "-f", "--file", default=None, type=argparse.FileType("r"), dest="script_file",
        help="Read script from file instead of command-line arguments",
    )
    parser.add_argument(
        "command", nargs=argparse.REMAINDER,
        help="Command to run (joined into a single bash -c argument)",
    )


def _parse_reap_args(parser: argparse.ArgumentParser) -> None:
    """Add arguments for the reap subcommand."""
    parser.add_argument(
        "-A", "--account", default=DEFAULT_ACCOUNT,
        help=f"SLURM account/project (default: {DEFAULT_ACCOUNT})",
    )
    parser.add_argument(
        "uuid", help="Job UUID to reap (printed by --no-wait)",
    )


def _build_run_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="iri-bash",
        description="Run bash commands on NERSC Perlmutter via the IRI API.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            '  iri-bash echo hello world\n'
            '  iri-bash \'for i in 1 2 3; do echo $i; done\'\n'
            '  iri-bash -f my_script.sh\n'
            '  iri-bash <<\'EOF\'\n'
            '  module load python\n'
            '  python my_script.py\n'
            '  EOF\n'
            '  iri-bash --no-wait echo hello     # prints UUID\n'
            '  iri-bash reap <uuid>              # fetch output and clean up\n'
        ),
    )
    _parse_run_args(parser)
    return parser


def _build_reap_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="iri-bash reap",
        description="Fetch output and clean up a completed iri-bash job.",
    )
    _parse_reap_args(parser)
    return parser


def parse_args(argv=None):
    if argv is None:
        argv = sys.argv[1:]

    # Detect subcommands before argparse sees them
    if argv and argv[0] == "reap":
        parser = _build_reap_parser()
        args = parser.parse_args(argv[1:])
        args.subcommand = "reap"
        return args

    if argv and argv[0] == "status":
        parser = argparse.ArgumentParser(
            prog="iri-bash status",
            description="Check the status of a SLURM job.",
        )
        parser.add_argument("job_id", help="SLURM job ID")
        args = parser.parse_args(argv[1:])
        args.subcommand = "status"
        return args

    # Default: run mode
    parser = _build_run_parser()
    args = parser.parse_args(argv)
    args.subcommand = None

    if args.command and args.command[0] == "--":
        args.command = args.command[1:]

    return args


def _resolve_script(args) -> str:
    """Determine the bash script to execute from args, file, or stdin."""
    if args.script_file is not None:
        script = args.script_file.read()
        args.script_file.close()
        return script

    if args.command:
        return " ".join(args.command)

    # No args and no file — try stdin
    if not sys.stdin.isatty():
        return sys.stdin.read()

    print("Error: No command specified.", file=sys.stderr)
    print("Usage: iri-bash COMMAND  |  iri-bash -f FILE  |  ... | iri-bash", file=sys.stderr)
    sys.exit(1)


def _build_run_payload(args, job_uuid: str, script: str) -> dict:
    directory = _job_dir(args.account, job_uuid)
    return {
        "name": f"iri-bash-{job_uuid}",
        "executable": "/bin/bash",
        "arguments": ["-c", script],
        "directory": directory,
        "stdout_path": f"{directory}/stdout",
        "stderr_path": f"{directory}/stderr",
        "resources": {
            "process_count": 1,
        },
        "attributes": {
            "duration": args.time_minutes * 60,
            "queue_name": "cron",
            "account": args.account,
            "custom_attributes": {
                "constraint": "cron",
            },
        },
    }



def _cmd_run(args) -> None:
    """Execute the run (default) subcommand."""
    script = _resolve_script(args)
    token = load_token()
    job_uuid = uuid.uuid4().hex[:12]

    payload = _build_run_payload(args, job_uuid, script)

    print(f"Submitting to {RESOURCE}...", file=sys.stderr)
    debug = getattr(args, "debug", False)
    job_id, state = submit_job(token, payload, debug=debug)
    print(f"Job submitted: {job_id} (state: {state})", file=sys.stderr)

    if args.no_wait:
        print(job_uuid)
        return

    directory = _job_dir(args.account, job_uuid)
    rc = poll_job(token, job_id, f"{directory}/stdout", f"{directory}/stderr")

    # Clean up the job output directory after successful streaming
    if rc == 0:
        remove_remote_path(token, directory)

    sys.exit(rc)


def _cmd_reap(args) -> None:
    """Fetch stdout/stderr from a completed job, then clean up."""
    token = load_token()
    directory = _job_dir(args.account, args.uuid)
    stdout_path = f"{directory}/stdout"
    stderr_path = f"{directory}/stderr"

    # Fetch stdout
    print(f"Fetching output from {directory}...", file=sys.stderr)
    offset = 0
    while True:
        chunk = read_file_chunk(token, stdout_path, offset)
        if not chunk:
            break
        sys.stdout.write(chunk)
        sys.stdout.flush()
        offset += len(chunk.encode("utf-8"))

    # Fetch stderr
    offset = 0
    while True:
        chunk = read_file_chunk(token, stderr_path, offset)
        if not chunk:
            break
        sys.stderr.write(chunk)
        sys.stderr.flush()
        offset += len(chunk.encode("utf-8"))

    # Clean up via filesystem API
    print(f"Cleaning up {directory}...", file=sys.stderr)
    remove_remote_path(token, directory)


def _cmd_status(args) -> None:
    """Check the status of a SLURM job."""
    token = load_token()
    resp = requests.get(
        f"{API_BASE}/compute/status/{RESOURCE}/{args.job_id}",
        headers=auth_headers(token),
    )
    if not resp.ok:
        print(f"Error: HTTP {resp.status_code}", file=sys.stderr)
        print(resp.text, file=sys.stderr)
        sys.exit(1)

    import json
    print(json.dumps(resp.json(), indent=2))


def main(argv=None):
    args = parse_args(argv)

    if args.subcommand == "reap":
        _cmd_reap(args)
    elif args.subcommand == "status":
        _cmd_status(args)
    else:
        _cmd_run(args)


if __name__ == "__main__":
    main()
