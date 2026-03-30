#!/bin/bash
# SLURM batch script for HAIDIS on Perlmutter
#
# SLURM Options (can be overridden via sbatch):
#   -N 1              Number of nodes (override with -N on sbatch command line)
#   -q debug          Queue (debug or regular)
#   -t 00:30:00       Time limit
#   -A <allocation>   Project allocation
#
# Other Options:
#   --sagipsimage IMAGE  Container image (default: codecr.jlab.org/datascience/haidis-ips/nersc_base)
#   --ersapimage IMAGE   Container image (default: docker.io/gurjyan/haidis-dp:latest)
#   --e2sarimage IMAGE   Container image (default: docker.io/ibaldin/e2sar:0.3.1)
#   --gpus-per-node N    Number of GPUs per node (default: 4)
#
# Environment Variables:
#   EJFAT_URI         Required: EJFAT load balancer URI
#
# Example (single node):
#   EJFAT_URI="ejfat://..." sbatch -N 1 -A <project> haidis_slurm.sh
#
# Example (multi-node):
#   EJFAT_URI="ejfat://..." sbatch -N 4 -A <project> haidis_slurm.sh

##SBATCH -N 1              # commented out - pass -N on sbatch command line
#SBATCH --account=amsc016
#SBATCH --qos=debug
#SBATCH -t 00:30:00
#SBATCH -o runs/slurm-%j.out
#SBATCH -e runs/slurm-%j.err
#SBATCH --constraint=gpu
#SBATCH --gpus-per-node=4
#SBATCH --gpu-bind=none
#SBATCH --chdir=/global/cfs/cdirs/amsc016/haidis/

set -euo pipefail

#=============================================================================
# Parse command-line arguments
#=============================================================================

SAGIPSIMAGE="codecr.jlab.org/datascience/haidis-ips/nersc_base"
ERSAPIMAGE="docker.io/gurjyan/haidis-dp:latest"
E2SARIMAGE="docker.io/ibaldin/e2sar:0.3.1"
GPUS_PER_NODE=4

while [[ $# -gt 0 ]]; do
    case $1 in
        --sagipsimage)
            SAGIPSIMAGE="$2"
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
        --gpus-per-node)
            GPUS_PER_NODE="$2"
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

#=============================================================================
# Environment setup
#=============================================================================

TOTAL_RANKS=$((SLURM_NNODES * GPUS_PER_NODE))

echo "========================================="
echo "HAIDIS Test - SLURM Job $SLURM_JOB_ID running in $PWD"
echo "SAGIPS IMAGE: ${SAGIPSIMAGE}"
echo "ERSAP IMAGE:  ${ERSAPIMAGE}"
echo "E2SAR IMAGE:  ${E2SARIMAGE}"
echo "Nodes:        ${SLURM_NNODES}"
echo "GPUs/node:    ${GPUS_PER_NODE}"
echo "Total ranks:  ${TOTAL_RANKS}"
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
# so Hydra doesn't fail trying to create them inside the container
for node in "${NODE_ARRAY[@]}"; do
    mkdir -p "$SCRIPT_DIR/outputs/$node"
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
# ERSAP is launched via srun as a separate step, one task per node.
# Each node's ERSAP writes to the node-local IPC shared memory segment.
# This step runs in the background so Phase 3 can follow after a short delay.
#=============================================================================
echo "========================================="
echo "Phase 2: Starting ERSAP containers"
echo "========================================="

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
    -v ${SCRIPT_DIR}/ersap-data:/user_data \
    -e EJFAT_URI='${EJFAT_URI}' \
    -e RECV_IP=\$RECEIVER_IP \
    ${ERSAPIMAGE} > ${JOB_DIR}/ersap_\$(hostname).log 2>&1

echo "[\$(hostname)] ERSAP exited with code \$?"
EOF

chmod +x $JOB_DIR/ersap_launcher_${SLURM_JOB_ID}.sh

# Launch ERSAP on all nodes simultaneously, in the background
srun --ntasks=${SLURM_NNODES} \
     --ntasks-per-node=1 \
     --gpus-per-node=0 \
     --overlap \
     bash $JOB_DIR/ersap_launcher_${SLURM_JOB_ID}.sh &

ERSAP_SRUN_PID=$!
echo "ERSAP srun launched (PID $ERSAP_SRUN_PID), waiting for shmem to be populated..."
sleep 10

#=============================================================================
# Phase 3: Launch SAGIPS via a single srun spanning all nodes
#
# srun acts as the MPI launcher via PMI2, spawning GPUS_PER_NODE tasks per
# node. Each task runs one container instance with one Python rank. Slurm
# propagates MPI rank information through PMI2 so mpi4py sees the correct
# global rank and world size across all nodes, enabling cross-node gradient
# aggregation via MPI Allreduce.
#
# SLURM_LOCALID (0..GPUS_PER_NODE-1) is used for CUDA_VISIBLE_DEVICES so
# each rank is pinned to its own GPU. SLURM_NODEID is used to route output
# to the per-node directory.
#=============================================================================
echo "========================================="
echo "Phase 3: Starting SAGIPS ($TOTAL_RANKS total ranks)"
echo "========================================="

# Generate the per-rank SAGIPS wrapper script
# This is invoked once per rank by srun, with Slurm env vars set per rank
cat > $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh << 'WRAPPER_EOF'
#!/bin/bash
# This script is invoked once per rank by srun.
# Slurm sets SLURM_LOCALID (local rank within node) and SLURM_NODEID.
# PMI2 environment is set by srun for mpi4py rank detection.

echo "[$(hostname)] SAGIPS rank wrapper starting: SLURM_LOCALID=$SLURM_LOCALID SLURM_NODEID=$SLURM_NODEID"

podman-hpc run --rm \
    --ipc=host \
    --network=host \
    --security-opt=label=disable \
    --gpus all \
    -e CUDA_VISIBLE_DEVICES=$SLURM_LOCALID \
    -e PMI_FD=$PMI_FD \
    -e PMI_RANK=$PMI_RANK \
    -e PMI_SIZE=$PMI_SIZE \
    -e SLURM_LOCALID=$SLURM_LOCALID \
    -e SLURM_NODEID=$SLURM_NODEID \
    -e SLURM_PROCID=$SLURM_PROCID \
    -e SLURM_GTIDS=$SLURM_GTIDS \
    -v SCRIPT_DIR_PLACEHOLDER/outputs/$(hostname):/app/outputs \
    -v SCRIPT_DIR_PLACEHOLDER/sagips.yaml:/app/src/haidis_ips/cfg/sagips.yaml:ro \
    SAGIPSIMAGE_PLACEHOLDER \
    /opt/mpich/install/bin/mpirun -n 1 \
      python /app/src/haidis_ips/dalitz_shmem_workflow.py \
        -cn sagips \
        hydra.run.dir=/app/outputs/hydra_rank${SLURM_LOCALID}

EXIT_CODE=$?
echo "[$(hostname)] SAGIPS rank $SLURM_LOCALID exited with code $EXIT_CODE"
exit $EXIT_CODE
WRAPPER_EOF

# Substitute the values that need to come from the batch script context
sed -i "s|SCRIPT_DIR_PLACEHOLDER|${SCRIPT_DIR}|g" $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh
sed -i "s|SAGIPSIMAGE_PLACEHOLDER|${SAGIPSIMAGE}|g" $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh
chmod +x $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh

echo "SAGIPS rank wrapper: $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh"

# Single srun spanning all nodes, one task per GPU.
# srun uses PMI2 to provide MPI rank information to each task,
# enabling mpi4py inside each container to form a single global communicator.
srun --ntasks=${TOTAL_RANKS} \
     --ntasks-per-node=${GPUS_PER_NODE} \
     --gpus-per-node=${GPUS_PER_NODE} \
     --gpu-bind=none \
     --mpi=pmi2 \
     --overlap \
     bash $JOB_DIR/sagips_rank_wrapper_${SLURM_JOB_ID}.sh \
     > $JOB_DIR/sagips.log 2>&1

SAGIPS_EXIT=$?
echo "SAGIPS srun completed with exit code $SAGIPS_EXIT"

#=============================================================================
# Cleanup: shut down ERSAP containers
#=============================================================================
echo "========================================="
echo "Shutting down ERSAP containers"
echo "========================================="

# Kill the ERSAP srun - this will terminate ERSAP containers on all nodes
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
echo "Total ranks:   $TOTAL_RANKS"
echo "Job directory: $JOB_DIR"
echo ""
echo "Logs available at:"
echo "  - SBatch logs:          $RUNS_DIR/slurm-${SLURM_JOB_ID}.out/.err"
echo "  - ERSAP logs:           $JOB_DIR/ersap_<node>.log"
echo "  - SAGIPS combined log:  $JOB_DIR/sagips.log"
echo "  - Per-node outputs:     $SCRIPT_DIR/outputs/<node>/"
echo ""
echo "End time: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================="

exit $SAGIPS_EXIT