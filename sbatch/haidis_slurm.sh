#!/bin/bash
# Minimal SLURM batch script for E2SAR sender/receiver tests on Perlmutter
#
# SLURM Options (can be overridden via sbatch):
#   -N 1              Number of nodes 
#   -q debug          Queue (debug or regular)
#   -t 00:30:00       Time limit
#   -A <allocation>   Project allocation
#
# Other Options (passed to haidis_slurm.sh):
#   --sagipsimage IMAGE  Container image (default: codecr.jlab.org/datascience/haidis-ips/nersc_base)
#   --ersapimage IMAGE   Container image (default: docker.io/gurjyan/haidis-dp:latest)
#   --e2sarimage IMAGE   Container image (default: docker.io/ibaldin/e2sar:0.3.1)
#
# Environment Variables:
#   EJFAT_URI         Required: EJFAT load balancer URI
#
# Example:
#   EJFAT_URI="ejfat://..." sbatch -A <project> haidis_slurm.sh --ersapimage haidis-dp:latest 

#SBATCH -N 1
##SBATCH -C cpu
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

echo "========================================="
echo "HAIDIS Test - SLURM Job $SLURM_JOB_ID running in $PWD"
echo "SAGIPS IMAGE: ${SAGIPSIMAGE}"
echo "ERSAP IMAGE:  ${ERSAPIMAGE}"
echo "E2SAR IMAGE:  ${E2SARIMAGE}"
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

# containers will timeout 30 seconds before to give them time to shutdown
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

# Get script directory where various configs reside
SCRIPT_DIR="$PWD"
echo "Script directory: $SCRIPT_DIR"

# Create runs directory if it doesn't exist
RUNS_DIR="${SCRIPT_DIR}/runs"
mkdir -p "$RUNS_DIR"
echo "Runs directory: $RUNS_DIR"

# Create job-specific working directory for logs
JOB_DIR="${RUNS_DIR}/slurm_job_${SLURM_JOB_ID}"
mkdir -p "$JOB_DIR"
cd "$JOB_DIR"
echo "Working directory: $JOB_DIR"
echo ""

# SLURM_JOB_NODELIST format examples: "nid[001-002]", "nid001,nid002"
NODE_ARRAY=($(scontrol show hostname $SLURM_JOB_NODELIST))
echo "Nodes: $NODE_ARRAY"

#=============================================================================
# Phase 1: Check load balancer status
#=============================================================================
export EJFAT_URI
echo "========================================="
echo "Phase 1: Validate EJFAT LB Reservation: "
echo "========================================="

# Try to run lbadm --status to check if the reservation is valid
if podman-hpc run -e EJFAT_URI="$EJFAT_URI" --rm --network host $E2SARIMAGE lbadm -4 -v --status &>/dev/null; then
    echo "Existing reservation is valid"
else
    echo "Existing reservation is invalid, exiting. Please create a loadbalancer reservation using lbadm"
    exit 1
fi

#=============================================================================
# Phase 2: Start container pairs
#=============================================================================

# generate launcher script (note EOF not quoted to allow expansion from outer script)
cat > $JOB_DIR/node_launcher_${SLURM_JOB_ID}.sh << EOF
#!/bin/bash

DATA_IPv4=$(echo "$EJFAT_URI" | grep -oP 'data=\K([0-9]{1,3}\.){3}[0-9]{1,3}')
DATA_IPv6=$(echo "$EJFAT_URI" | grep -oP 'data=\[\K[0-9a-fA-F:]+(?=\])')

# Find source IP for route to LB dataplane
RECEIVER_IP=\$(ip route get "\$DATA_IPv4" | head -1 | sed 's/^.*src//' | awk '{print \$1}')

if [[ -z "\$RECEIVER_IP" ]]; then
    echo "ERROR: Failed to detect receiver IP to use with ERSAP"
    exit 1
fi

echo "Receiver IP: \$RECEIVER_IP"
echo "ERSAP Config in: $SCRIPT_DIR/ersap-data/config/"
echo "SAGIPS Config in: $SCRIPT_DIR/sagips.yaml"

timeout ${CONTAINER_TIMEOUT} podman-hpc run \
    --network=host --ipc=host --rm --group-add keep-groups \
    -v $SCRIPT_DIR/ersap-data:/user_data \
    -e EJFAT_URI='$EJFAT_URI' \
    -e RECV_IP=\$RECEIVER_IP \
    $ERSAPIMAGE > $JOB_DIR/ersap_\$(hostname).log 2>&1 &

ERSAP_PID=\$!

sleep 5

timeout ${CONTAINER_TIMEOUT} podman-hpc run \
    -i --rm --group-add keep-groups \
    --ipc=host \
    --security-opt=label=disable \
    --gpus all \
    -v $SCRIPT_DIR/outputs:/app/outputs:Z \
    -v $SCRIPT_DIR/sagips.yaml:/app/src/haidis_ips/cfg/sagips.yaml:ro \
    $SAGIPSIMAGE \
    /opt/mpich/install/bin/mpirun -n 4 \
      bash -c 'CUDA_VISIBLE_DEVICES=\$MPI_LOCALRANKID uv run /app/src/haidis_ips/dalitz_shmem_workflow.py -cn sagips' \
    > $JOB_DIR/sagips_\$(hostname).log 2>&1 &

SAGIPS_PID=\$!

wait \$ERSAP_PID
wait \$SAGIPS_PID

EOF

chmod +x $JOB_DIR/node_launcher_${SLURM_JOB_ID}.sh
echo "========================================="
echo "Launcher Script in $JOB_DIR/node_launcher_${SLURM_JOB_ID}.sh"
echo "========================================="

# Run the launcher script once per node
# --ntasks=${SLURM_NNODES} ensures one task per node
# --ntasks-per-node=1 ensures exactly one per node
srun --ntasks=${SLURM_NNODES} \
     --ntasks-per-node=1 \
     --gpus-per-node=4 \
     --gpu-bind=none \
     bash $JOB_DIR/node_launcher_${SLURM_JOB_ID}.sh > launcher.log 2>&1

echo "All node pairs completed"

#=============================================================================
# Summary and Log Collection
#=============================================================================

echo "========================================="
echo "Test Summary"
echo "========================================="
echo "Job ID:        $SLURM_JOB_ID"
echo "Job directory: $JOB_DIR"
echo ""

echo "Logs available at:"
echo "  - SBatch logs:             $RUNS_DIR/slurm-<job id>.out/.err"
echo "  - Container Launcher log:  $JOB_DIR/launcher.log"
echo "  - ERSAP logs:              $JOB_DIR/ersap_<node>.log"
echo "  - SAGIPS logs:             $JOB_DIR/sagips_<node>.log"
echo ""

echo "========================================="
echo "End time: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================="

# Exit with sender's exit code (most important for test success)
exit $CONTAINERS_EXIT_CODE
