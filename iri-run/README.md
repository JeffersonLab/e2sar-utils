# iri-run

CLI tools for running commands on NERSC Perlmutter from anywhere, without SSH.

This project talks to the [NERSC IRI API](https://api.iri.nersc.gov/#/) to submit
and monitor batch jobs on Perlmutter. Authentication is handled automatically
via `globus-cli`.

## Installation

Requires Python 3.9+ and [globus-cli](https://docs.globus.org/cli/).

```bash
# Install globus-cli (if not already installed)
pipx install globus-cli

# Install iri-run
python3 -m venv e2sar-utils && . e2sar-utils/bin/activate
pip install -e .
```

## Authentication

Both tools read tokens from the `globus-cli` token store. On first run (or when
tokens expire), they will automatically invoke `globus login` and
`globus session consent` on your behalf -- just follow the prompts.

Access tokens expire after ~48 hours and are re-acquired automatically.

---

## iri-bash

Run bash commands on NERSC interactive nodes via the "cron" QOS. This is the
primary tool -- it hides the SLURM job mechanics and behaves as if you're
running a command in a remote shell.

### Usage

```bash
# Simple command
iri-bash echo hello world

# Quoted complex command
iri-bash 'for i in 1 2 3; do echo $i; done'

# From a script file
iri-bash -f my_script.sh

# From stdin (heredoc)
iri-bash <<'EOF'
module load python
python my_script.py
EOF

# Pipe a script
cat my_script.sh | iri-bash
```

By default, `iri-bash` waits for the job to complete and streams stdout/stderr
back to your terminal.

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `-A`, `--account` | `myproject` | SLURM account/project |
| `-t`, `--time` | `30` | Wall time in minutes |
| `--no-wait` | | Submit and exit immediately (prints job UUID) |
| `-f`, `--file` | | Read script from a local file |

### Fire-and-forget with reap

Use `--no-wait` to submit a job and get back a UUID immediately:

```bash
UUID=$(iri-bash --no-wait 'long-running-command')
echo "Submitted: $UUID"
```

Later, fetch the output and clean up:

```bash
iri-bash reap $UUID
```

The `reap` subcommand prints stdout/stderr to your terminal, then submits a
small cleanup job to remove the output directory on NERSC.

### How it works

Each invocation:
1. Generates a UUID for the job
2. Creates a working directory at `/global/cfs/cdirs/<account>/iri-bash/<uuid>/`
3. Submits `/bin/bash -c "<your script>"` as a SLURM job under the `cron` QOS
4. Streams stdout/stderr back (or prints the UUID if `--no-wait`)

---

## iri-run

Lower-level tool for submitting arbitrary SLURM jobs with full control over
job parameters. See `iri-run -h` for all options.

```bash
iri-run -A myproject -q debug -C gpu -t 10 -N 2 --wait -- python my_script.py
```

---

## Development

```bash
python3 -m venv e2sar-utils && . e2sar-utils/bin/activate
pip install -e ".[dev]"
pytest -v
```

## Project structure

```
iri-run/
├── pyproject.toml
├── src/
│   └── iri_run/
│       ├── __init__.py
│       ├── api.py       # Shared: auth, API client, polling
│       ├── bash.py      # iri-bash CLI
│       └── cli.py       # iri-run CLI
└── tests/
    ├── test_api.py              # API client tests
    ├── test_auth.py             # Globus auth tests
    ├── test_bash_main.py        # iri-bash end-to-end
    ├── test_bash_parse_args.py  # iri-bash argument parsing
    ├── test_bash_payload.py     # iri-bash payload construction
    ├── test_bash_resolve_script.py  # Script resolution (args/file/stdin)
    ├── test_build_payload.py    # iri-run payload construction
    ├── test_helpers.py          # Utility functions
    ├── test_main.py             # iri-run end-to-end
    ├── test_parse_args.py       # iri-run argument parsing
    └── test_poll_job.py         # Job polling and output streaming
```
