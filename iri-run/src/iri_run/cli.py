"""CLI entry point for iri-run: submit SLURM jobs to NERSC via the IRI API."""

from __future__ import annotations

import argparse
import sys
import uuid

from iri_run.api import (
    RESOURCE,
    ensure_remote_dir,
    load_token,
    poll_job,
    submit_job,
)


def _default_output_dir(account: str) -> str:
    return f"/global/cfs/cdirs/{account}/iri-run"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="iri-run",
        description="Submit a SLURM job to NERSC Perlmutter via the IRI API.",
        epilog=(
            "Examples:\n"
            '  iri-run -A m1234 -- echo "hello from perlmutter"\n'
            "  iri-run -A m1234 -q debug -C gpu -t 10 -N 2 --wait -- python my_script.py\n"
            '  iri-run -A m1234 --env MY_VAR=foo -- bash -c "echo $MY_VAR"\n'
            "\n"
            "Authentication:\n"
            "  On first run, you will be prompted to log in via Globus."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("-A", "--account", default="amsc016", help="SLURM account/project (default: amsc016)")
    parser.add_argument("-q", "--qos", default="cron", help="Quality of service (default: cron)")
    parser.add_argument("-C", "--constraint", default="cron", help="Node constraint (default: cron)")
    parser.add_argument("-t", "--time", type=int, default=30, dest="time_minutes", help="Wall time in minutes (default: 30)")
    parser.add_argument("-N", "--nodes", type=int, default=1, help="Number of nodes (default: 1)")
    parser.add_argument("-n", "--ntasks", type=int, default=1, help="Number of tasks/processes (default: 1)")
    parser.add_argument("-p", "--partition", default=None, help="Partition name")
    parser.add_argument("-d", "--directory", default=None, help="Working directory on NERSC")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VAL", help="Environment variable (repeatable)")
    parser.add_argument("-w", "--wait", action="store_true", help="Poll for job completion and stream stdout/stderr")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="Command to run (after '--')")

    args = parser.parse_args(argv)

    if args.command and args.command[0] == "--":
        args.command = args.command[1:]

    if not args.command:
        parser.error("No command specified. Use -- COMMAND [ARGS...]")

    return args


def build_payload(args, job_id_hint: str = "") -> dict:
    executable = args.command[0]
    arguments = args.command[1:]
    hint = job_id_hint or "job"

    custom_attributes = {
        "qos": args.qos,
        "constraint": args.constraint,
    }
    if args.partition:
        custom_attributes["partition"] = args.partition

    output_dir = _default_output_dir(args.account)
    stdout_path = f"{output_dir}/{hint}.out"
    stderr_path = f"{output_dir}/{hint}.err"

    payload = {
        "name": f"iri-run-{hint}",
        "executable": executable,
        "arguments": arguments,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
        "resources": {
            "node_count": args.nodes,
            "process_count": args.ntasks,
        },
        "attributes": {
            "duration": args.time_minutes * 60,
            "account": args.account,
            "custom_attributes": custom_attributes,
        },
    }

    env = {}
    for entry in args.env:
        key, _, val = entry.partition("=")
        env[key] = val
    if env:
        payload["environment"] = env

    if args.directory:
        payload["directory"] = args.directory

    return payload


def main(argv=None):
    args = parse_args(argv)
    token = load_token()

    job_id_hint = uuid.uuid4().hex[:12]
    payload = build_payload(args, job_id_hint=job_id_hint)

    output_dir = _default_output_dir(args.account)
    ensure_remote_dir(token, output_dir)

    print(f"Submitting job to {RESOURCE}...", file=sys.stderr)
    job_id, state = submit_job(token, payload)
    print(f"Job submitted: {job_id} (state: {state})", file=sys.stderr)

    if not args.wait:
        print(job_id)
        return

    stdout_path = payload["stdout_path"]
    stderr_path = payload["stderr_path"]
    print(f"Streaming output (stdout: {stdout_path}, stderr: {stderr_path})...", file=sys.stderr)
    rc = poll_job(token, job_id, stdout_path, stderr_path)
    sys.exit(rc)


if __name__ == "__main__":
    main()
