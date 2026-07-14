#!/bin/bash
# SLURM batch script for HAIDIS CPU-only integration test on Perlmutter
#
# Runs ERSAP + gluex-reader.py on CPU nodes (no GPU required).
# Replaces SAGIPS with a lightweight Python reader that emits the same
# log signals, so haidis-run monitoring works unchanged.
#
# SLURM Options (can be overridden via sbatch):
#   -N 1              Number of nodes (override with -N on sbatch command line)
#   -q debug          Queue (debug or shared for CPU)
#   -t 00:30:00       Time limit
#   -A <allocation>   Project allocation
#
# Other Options:
#   --gluexreaderimage IMAGE  Container image (default: localhost/gluex-reader:dev)
#   --ersapimage IMAGE        Container image (default: docker.io/gurjyan/haidis-dp:latest)
#   --e2sarimage IMAGE        Container image (default: docker.io/ibaldin/e2sar:0.3.1)
#   --shmem-name NAME         Shared memory segment name (default: haidis_shmem)
#   --sem-name NAME           Semaphore name (default: haidis_sem)
#   --sem-ack-name NAME       Ack semaphore name (default: haidis_sem_ack)
#   --iterations N            Batches to read before exiting (default: no limit)
#
# Reader mode (mutually exclusive; default is --histogram):
#   --save FILE               Write raw CSV (x,y per event) to FILE in the job dir
#   --plot FILE               Override default histogram PNG path (default: histogram_<node>.png)
#   --bins N                  Histogram bin count (default: 50; ignored with --save)
#   --out-stats FILE          Save histogram .npz to FILE in the job dir (ignored with --save)
#   --flush-every N           Re-save plot/stats every N batches (default: 10; 0 = only on exit)
#   --filter-abs-max X        Discard events where abs(x) > X or abs(y) > X (default: no filter)
#
# Environment Variables:
#   EJFAT_URI                 Required: EJFAT load balancer URI
#   ERSAP_CONFIG_DIR          Optional: path to per-run ERSAP config dir; mounted over /user_data/config
#
# Example (single node):
#   EJFAT_URI="ejfat://..." sbatch -N 1 -A <project> haidis_cpu_test.sh
#
# Example (with per-run ERSAP config):
#   EJFAT_URI="ejfat://..." ERSAP_CONFIG_DIR=/path/to/ersap/config \
#       sbatch -N 1 -A <project> haidis_cpu_test.sh

##SBATCH -N 1              # commented out - pass -N on sbatch command line
#SBATCH --account=amsc016
#SBATCH --qos=debug
#SBATCH -t 00:30:00
#SBATCH -o runs/slurm-%j.out
#SBATCH -e runs/slurm-%j.err
#SBATCH --constraint=cpu
#SBATCH --chdir=/global/cfs/cdirs/amsc016/haidis/

set -euo pipefail

#=============================================================================
# Parse command-line arguments
#=============================================================================

GLUEXREADERIMAGE="localhost/gluex-reader:dev"
ERSAPIMAGE="docker.io/gurjyan/haidis-dp:latest"
E2SARIMAGE="docker.io/ibaldin/e2sar:0.3.1"
SHMEM_NAME="haidis_shmem"
SEM_NAME="haidis_sem"
SEM_ACK_NAME="haidis_sem_ack"
ITERATIONS_ARG=""
SAVE_FILE=""
PLOT_FILE=""
BINS_ARG=""
OUT_STATS_FILE=""
FLUSH_EVERY_ARG=""
FILTER_ABS_MAX_ARG=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --gluexreaderimage)
            GLUEXREADERIMAGE="$2"
            shift 2
            ;;
        --ersapimage)
            ERSAPIMAGE="$2"
            shift 2
            ;;
        --e2sarimage)
            E2SARIMAGE="$2"
            shift 2
            ;;
        --shmem-name)
            SHMEM_NAME="$2"
            shift 2
            ;;
        --sem-name)
            SEM_NAME="$2"
            shift 2
            ;;
        --sem-ack-name)
            SEM_ACK_NAME="$2"
            shift 2
            ;;
        --iterations)
            ITERATIONS_ARG="--iterations $2"
            shift 2
            ;;
        --save)
            SAVE_FILE="$2"
            shift 2
            ;;
        --plot)
            PLOT_FILE="$2"
            shift 2
            ;;
        --bins)
            BINS_ARG="--bins $2"
            shift 2
            ;;
        --out-stats)
            OUT_STATS_FILE="$2"
            shift 2
            ;;
        --flush-every)
            FLUSH_EVERY_ARG="--flush-every $2"
            shift 2
            ;;
        --filter-abs-max)
            FILTER_ABS_MAX_ARG="--filter-abs-max $2"
            shift 2
            ;;
        --help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Validate and build reader args passed into the launcher heredoc.
# \$(hostname) in the default plot path is intentionally unescaped here so it
# lands as a literal $(hostname) in the generated launcher script and expands
# on each compute node at runtime.
if [[ -n "$SAVE_FILE" && -n "$PLOT_FILE" ]]; then
    echo "ERROR: --save and --plot are mutually exclusive"
    exit 1
fi

if [[ -n "$SAVE_FILE" ]]; then
    READER_ARGS="--save /app/outputs/${SAVE_FILE} ${ITERATIONS_ARG}"
else
    if [[ -n "$PLOT_FILE" ]]; then
        READER_ARGS="--histogram --plot /app/outputs/${PLOT_FILE}"
    else
        READER_ARGS="--histogram --plot /app/outputs/histogram_\$(hostname).png"
    fi
    [[ -n "$BINS_ARG" ]]        && READER_ARGS="${READER_ARGS} ${BINS_ARG}"
    [[ -n "$OUT_STATS_FILE" ]]  && READER_ARGS="${READER_ARGS} --out-stats /app/outputs/${OUT_STATS_FILE}"
    [[ -n "$FLUSH_EVERY_ARG" ]] && READER_ARGS="${READER_ARGS} ${FLUSH_EVERY_ARG}"
    READER_ARGS="${READER_ARGS} ${ITERATIONS_ARG}"
fi
[[ -n "$FILTER_ABS_MAX_ARG" ]] && READER_ARGS="${READER_ARGS} ${FILTER_ABS_MAX_ARG}"

#=============================================================================
# Environment setup
#=============================================================================

echo "========================================="
echo "HAIDIS CPU Test - SLURM Job $SLURM_JOB_ID running in $PWD"
echo "GLUEX READER IMAGE: ${GLUEXREADERIMAGE}"
echo "ERSAP IMAGE:        ${ERSAPIMAGE}"
echo "E2SAR IMAGE:        ${E2SARIMAGE}"
echo "Nodes:              ${SLURM_NNODES}"
echo "Shmem name:         ${SHMEM_NAME}"
echo "Sem name:           ${SEM_NAME}"
echo "Sem ack name:       ${SEM_ACK_NAME}"
echo "Reader args:        ${READER_ARGS}"
echo "========================================="
echo "Start time: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""

# Get time limit in seconds
get_slurm_timelimit_seconds() {
    local time_str=$(scontrol show job $SLURM_JOB_ID | grep -oP 'TimeLimit=\K[^ ]+')
    local seconds=0

    if [[ $time_str =~ ^([0-9]+)-([0-9]+):([0-9]+):([0-9]+)$ ]]; then
        seconds=$(( ${BASH_REMATCH[1]} * 86400 + ${BASH_REMATCH[2]} * 3600 + ${BASH_REMATCH[3]} * 60 + ${BASH_REMATCH[4]} ))
    elif [[ $time_str =~ ^([0-9]+):([0-9]+):([0-9]+)$ ]]; then
        seconds=$(( ${BASH_REMATCH[1]} * 3600 + ${BASH_REMATCH[2]} * 60 + ${BASH_REMATCH[3]} ))
    elif [[ $time_str =~ ^([0-9]+):([0-9]+)$ ]]; then
        seconds=$(( ${BASH_REMATCH[1]} * 60 + ${BASH_REMATCH[2]} ))
    fi

    echo $seconds
}

SLURM_TIMEOUT=$(get_slurm_timelimit_seconds)
CONTAINER_TIMEOUT=$((SLURM_TIMEOUT - 30))
echo "Container timeout set to: ${CONTAINER_TIMEOUT}s"

# Validate EJFAT_URI
if [[ -z "${EJFAT_URI:-}" ]]; then
    echo "ERROR: EJFAT_URI is required"
    echo "Set via: EJFAT_URI='ejfat://...' sbatch $0"
    exit 1
fi

echo "EJFAT_URI: $EJFAT_URI"
echo "Job nodes: $SLURM_JOB_NODELIST"
echo ""

SCRIPT_DIR="$PWD"
echo "Script directory: $SCRIPT_DIR"

RUNS_DIR="${SCRIPT_DIR}/runs"
mkdir -p "$RUNS_DIR"

JOB_DIR="${RUNS_DIR}/slurm_job_${SLURM_JOB_ID}"
mkdir -p "$JOB_DIR"
echo "Job directory: $JOB_DIR"

NODE_ARRAY=($(scontrol show hostname $SLURM_JOB_NODELIST))
echo "Nodes: ${NODE_ARRAY[@]}"
echo ""

# Pre-create per-node output directories on the shared filesystem
for node in "${NODE_ARRAY[@]}"; do
    mkdir -p "$JOB_DIR/$node"
done
echo "Created per-node output directories"

# Extract data plane IP from EJFAT_URI once, in the batch script context
DATA_IPv4=$(echo "$EJFAT_URI" | grep -oP 'data=\K([0-9]{1,3}\.){3}[0-9]{1,3}')

#=============================================================================
# Phase 1: Check load balancer status
#=============================================================================
export EJFAT_URI
echo "========================================="
echo "Phase 1: Validate EJFAT LB Reservation"
echo "========================================="

if podman-hpc run -e EJFAT_URI="$EJFAT_URI" --rm --network host $E2SARIMAGE lbadm -4 -v --status &>/dev/null; then
    echo "Existing reservation is valid"
else
    echo "Existing reservation is invalid, exiting. Please create a loadbalancer reservation using lbadm"
    exit 1
fi

#=============================================================================
# Phase 2: Launch ERSAP containers - one per node, in parallel
#
# Identical to haidis_slurm.sh minus GPU srun flags.
# ERSAP_CONFIG_DIR, if set, is mounted over /user_data/config so a per-run
# services.yaml can override the default without duplicating the full data tree.
#=============================================================================
echo "========================================="
echo "Phase 2: Starting ERSAP containers"
echo "========================================="

# Build optional ERSAP config bind mount (expands at heredoc write time)
ERSAP_CONFIG_MOUNT=""
if [[ -n "${ERSAP_CONFIG_DIR:-}" ]]; then
    ERSAP_CONFIG_MOUNT="-v ${ERSAP_CONFIG_DIR}:/user_data/config"
fi

# Generate the ERSAP launcher script
cat > $JOB_DIR/ersap_launcher_${SLURM_JOB_ID}.sh << EOF
#!/bin/bash
# Runs once per node via srun --ntasks-per-node=1

RECEIVER_IP=\$(ip route get "${DATA_IPv4}" | head -1 | sed 's/^.*src//' | awk '{print \$1}')

if [[ -z "\$RECEIVER_IP" ]]; then
    echo "ERROR: [\$(hostname)] Failed to detect receiver IP"
    exit 1
fi

echo "[\$(hostname)] ERSAP starting, receiver IP: \$RECEIVER_IP"

timeout ${CONTAINER_TIMEOUT} podman-hpc run \
    --network=host --ipc=host --rm --group-add keep-groups \
    --ulimit nofile=65536:65536 \
    -v ${SCRIPT_DIR}/ersap-data:/user_data \
    ${ERSAP_CONFIG_MOUNT} \
    -e EJFAT_URI='${EJFAT_URI}' \
    -e RECV_IP=\$RECEIVER_IP \
    ${ERSAPIMAGE} > ${JOB_DIR}/ersap_\$(hostname).log 2>&1

echo "[\$(hostname)] ERSAP exited with code \$?"
EOF

chmod +x $JOB_DIR/ersap_launcher_${SLURM_JOB_ID}.sh

# Launch ERSAP on all nodes simultaneously, in the background
srun --ntasks=${SLURM_NNODES} \
     --ntasks-per-node=1 \
     --overlap \
     bash $JOB_DIR/ersap_launcher_${SLURM_JOB_ID}.sh &

ERSAP_SRUN_PID=$!
echo "ERSAP srun launched (PID $ERSAP_SRUN_PID), waiting for shmem to be populated..."
sleep 10

#=============================================================================
# Phase 3: Launch gluex-reader.py - one per node via srun
#
# Replaces SAGIPS. Single process per node (no MPI, no GPU).
# Emits the same log signals as SAGIPS so haidis-run monitoring is unchanged:
#   - "Waiting for data (sample 1)"      → readiness signal
#   - "HAIDIS TRAINING COMPLETE: ..."    → completion signal
#=============================================================================
echo "========================================="
echo "Phase 3: Starting gluex reader (${SLURM_NNODES} nodes)"
echo "========================================="

# Generate the gluex-reader launcher script (same pattern as ERSAP launcher so
# $(hostname) is evaluated on each compute node, not on the head node)
cat > $JOB_DIR/reader_launcher_${SLURM_JOB_ID}.sh << EOF
#!/bin/bash
# Runs once per node via srun --ntasks-per-node=1

echo "[\$(hostname)] gluex-reader starting"

podman-hpc run --rm \
    --ipc=host \
    --security-opt=label=disable \
    --group-add keep-groups \
    -v ${JOB_DIR}:/app/outputs \
    ${GLUEXREADERIMAGE} \
    --shmem-name ${SHMEM_NAME} \
    --sem-name ${SEM_NAME} \
    --sem-ack-name ${SEM_ACK_NAME} \
    ${READER_ARGS}

READER_EXIT=\$?
echo "[\$(hostname)] gluex-reader exited with code \$READER_EXIT"
exit \$READER_EXIT
EOF

chmod +x $JOB_DIR/reader_launcher_${SLURM_JOB_ID}.sh

srun --ntasks=${SLURM_NNODES} \
     --ntasks-per-node=1 \
     --overlap \
     bash $JOB_DIR/reader_launcher_${SLURM_JOB_ID}.sh

READER_EXIT=$?
echo "gluex-reader srun completed with exit code $READER_EXIT"

#=============================================================================
# Cleanup: shut down ERSAP containers
#=============================================================================
echo "========================================="
echo "Shutting down ERSAP containers"
echo "========================================="

kill $ERSAP_SRUN_PID 2>/dev/null || true
wait $ERSAP_SRUN_PID 2>/dev/null || true
echo "ERSAP containers stopped"

#=============================================================================
# Summary
#=============================================================================
echo "========================================="
echo "Test Summary"
echo "========================================="
echo "Job ID:        $SLURM_JOB_ID"
echo "Nodes:         $SLURM_NNODES"
echo "Job directory: $JOB_DIR"
echo ""
echo "Logs available at:"
echo "  - SBatch logs:          $RUNS_DIR/slurm-${SLURM_JOB_ID}.out/.err"
echo "  - ERSAP logs:           $JOB_DIR/ersap_<node>.log"
echo "  - Reader stdout:        $RUNS_DIR/slurm-${SLURM_JOB_ID}.out"
echo "  - Reader output files:  $JOB_DIR/"
echo ""
echo "End time: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================="

exit $READER_EXIT
