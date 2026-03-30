---
name: haidis-launch
description: Launch the HAIDIS workflow (haidis_slurm.sh) on Perlmutter via sbatch, monitor job status, stream logs, and verify successful completion. Use when the user wants to run, submit, or monitor the HAIDIS/ERSAP/SAGIPS job on Perlmutter.
---

# HAIDIS Slurm Job Launcher

Launch the HAIDIS workflow on Perlmutter, monitor it through completion, and report results.

## Step 0: Check Perlmutter Status

If you have already run `/perlmutter-status` today and received PASS, skip this step. Otherwise invoke it now. Stop on FAIL; proceed with a warning on WARN.

## Step 1: Gather Inputs

Ask the user for the following if not already provided:

1. **EJFAT URI** — `ejfat://token@host:port/lb/N?data=ip:port`
2. **Number of nodes** — integer (e.g. 2)

Do not proceed until both are provided.

## Step 2: Submit the Job

```bash
ssh perlmutter.nersc.gov "cd /global/cfs/cdirs/amsc016/haidis && EJFAT_URI='<EJFAT_URI>' sbatch -N <NUM_NODES> sbatch/haidis_slurm.sh"
```

Extract the job ID from `Submitted batch job <job_id>`. Tell the user: "Job submitted: ID **\<job_id\>**"

## Step 3: Wait for Job to Start Running

Run this single SSH call; it blocks until the job starts (or fails), then exits:

```bash
ssh perlmutter.nersc.gov "while true; do STATE=\$(squeue --job <job_id> --format='%T %R' --noheader); case \"\${STATE%% *}\" in R) echo \"RUNNING \$(squeue --job <job_id> --format='%N' --noheader)\"; exit 0;; F|TO|CA) echo \"FAILED: \$STATE\"; exit 1;; '') echo 'Job ended before R'; exit 1;; *) echo \"Waiting: \$STATE\"; sleep 10;; esac; done"
```

On exit 0, save the node list from the `RUNNING <nodes>` output and proceed to Step 4. On exit 1, report the failure and stop. Report each `Waiting:` line to the user while blocking.

## Step 4: Monitor Logs While Running

Poll every 15–30s until both containers are ready (see Step 4a) or the job leaves `R` state:

```bash
ssh perlmutter.nersc.gov "tail -n 80 /global/cfs/cdirs/amsc016/haidis/runs/slurm-<job_id>.out; tail -n 40 /global/cfs/cdirs/amsc016/haidis/runs/slurm-<job_id>.err; tail -n 40 /global/cfs/cdirs/amsc016/haidis/runs/slurm_job_<job_id>/ersap_*.log 2>/dev/null"
```

ERSAP logs only appear ~30s after the job enters `R` state. Report notable events (phase completions, errors, warnings) as they appear.

## Step 4a: Prompt User to Send Data

Watch the logs for these two readiness signals:

- **ERSAP ready** (one per node): `All services configured on <node>` in `ersap_*.log`
- **SAGIPS ready**: `Waiting for data (sample 1)` in stdout

Once both signals are present for all nodes, **stop polling and tell the user:**

> Both ERSAP and SAGIPS are ready and waiting for data. Please send data now using `root-reader` or your preferred sender pointed at the EJFAT URI.

Resume polling (Step 4) once the user confirms data is being sent.

## Step 5: Verify Completion

Do a final full read of all logs:

```bash
ssh perlmutter.nersc.gov "cat /global/cfs/cdirs/amsc016/haidis/runs/slurm-<job_id>.{out,err}; cat /global/cfs/cdirs/amsc016/haidis/runs/slurm_job_<job_id>/ersap_*.log 2>/dev/null"
```

**Success:** Ranks 0 through N-1 all appear with `Training completed`, where N = nodes × 4.

**Scan for errors:** `ERROR`, `Exception`, `Traceback`, `Segmentation fault`, `BIND MOUNT FAILED`, `lbadm` phase failures, ERSAP exit before completion, missing ranks.

Note: `SIGNAL Killed` in stderr after a zero exit code is expected cleanup behavior, not a failure.

## Step 6: Report Results

Report: job ID, node list, duration, ranks completed (X/N with list), any errors found, and log paths:
- Stdout/stderr: `/global/cfs/cdirs/amsc016/haidis/runs/slurm-<job_id>.out/.err`
- ERSAP: `/global/cfs/cdirs/amsc016/haidis/runs/slurm_job_<job_id>/ersap_<node>.log`
- Per-node outputs: `/global/cfs/cdirs/amsc016/haidis/outputs/<node>/`

## Shell Command Constraints

A pre-execution hook rejects commands containing single-quoted strings that start with a dash (e.g. `'---'`), flagging them as potential bypasses. Avoid using `echo '--- LABEL ---'` style separators in SSH commands. Use `echo "=== LABEL ==="` (double-quoted, no leading dash) instead.

## Notes

sbatch script: `/global/cfs/cdirs/amsc016/haidis/sbatch/haidis_slurm.sh` (default queue: `debug`, 30-min limit). Containers: ERSAP `docker.io/gurjyan/haidis-dp:latest`, SAGIPS `codecr.jlab.org/datascience/haidis-ips/nersc_base`, E2SAR `docker.io/ibaldin/e2sar:0.3.1`. Do NOT use `--signal=` in Slurm options (known Perlmutter issue).
