#!/bin/bash

# Entrypoint script for E2SAR EJFAT Tools Docker container
# This script:
# 1. Validates required EJFAT_URI environment variable
# 2. Sources ROOT and environment configuration
# 3. Starts ET system in background
# 4. Starts ersap-et-receiver to bridge E2SAR data to ET
# 5. Starts ERSAP in foreground

set -euo pipefail

echo "=========================================="
echo "E2SAR EJFAT Tools Container Starting"
echo "=========================================="

# Step 1: Validate EJFAT_URI environment variable
if [ -z "${EJFAT_URI:-}" ]; then
    echo "ERROR: EJFAT_URI environment variable is not set!"
    echo "Please run the container with: docker run -e EJFAT_URI=<your-uri> ..."
    exit 1
fi

echo "✓ EJFAT_URI: ${EJFAT_URI}"

# Step 2: Source ROOT environment
echo "----------------------------------------"
echo "Setting up ROOT environment..."
ROOT_SETUP="${ROOT_INSTALL}/root/bin/thisroot.sh"
if [ -f "${ROOT_SETUP}" ]; then
    # Use '.' instead of 'source' for POSIX compatibility, but we're using bash anyway
    source "${ROOT_SETUP}"
    echo "✓ Sourced: ${ROOT_SETUP}"
else
    echo "ERROR: ROOT setup script not found at ${ROOT_SETUP}"
    exit 1
fi

# Step 3: Source custom environment (env.sh)
# Note: env.sh tries to source ROOT's thisroot.sh with a relative path,
# but we've already sourced it above, so we'll skip that line
echo "----------------------------------------"
echo "Setting up CODA and ERSAP environment..."
cd "${WORKDIR}"

# Source env.sh, but handle the ROOT sourcing issue
# We'll create a temporary modified version that skips the ROOT sourcing line
ENV_FILE="${WORKDIR}/env.sh"
if [ -f "${ENV_FILE}" ]; then
    # Export variables that env.sh sets
    export CODA="${WORKDIR}/coda"
    export CODA_BIN="${CODA}/${MACHINE}/bin"
    export CODA_LIB="${CODA}/${MACHINE}/lib"
    export ERSAP_HOME="${WORKDIR}/ersap"
    export ERSAP_USER_DATA="${WORKDIR}/ersap-data"
    export PATH="${CODA_BIN}:${CODA}/common/bin:${ERSAP_HOME}/bin:${PATH}"
    export LD_LIBRARY_PATH="${CODA_LIB}:${LD_LIBRARY_PATH:-}"

    echo "✓ CODA: ${CODA}"
    echo "✓ CODA_BIN: ${CODA_BIN}"
    echo "✓ CODA_LIB: ${CODA_LIB}"
    echo "✓ ERSAP_HOME: ${ERSAP_HOME}"
    echo "✓ ERSAP_USER_DATA: ${ERSAP_USER_DATA}"
    echo "✓ LD_LIBRARY_PATH: ${LD_LIBRARY_PATH}"
else
    echo "WARNING: ${ENV_FILE} not found, environment may not be complete"
fi

# Step 4: Start ET system in background
echo "----------------------------------------"
echo "Starting ET system..."
ET_START_CMD="et_start -f /tmp/et_sys -v -d -n 1000 -s 2097152 -p 23911"
echo "Command: ${ET_START_CMD}"

# Check if et_start is available
if ! command -v et_start &> /dev/null; then
    echo "ERROR: et_start not found in PATH"
    echo "PATH: ${PATH}"
    exit 1
fi

# Start ET in background
${ET_START_CMD} &
ET_PID=$!
echo "✓ ET started with PID: ${ET_PID}"

# Give ET a moment to initialize
sleep 2

# Check if ET is still running
if ! kill -0 ${ET_PID} 2>/dev/null; then
    echo "ERROR: ET failed to start or exited immediately"
    exit 1
fi

echo "✓ ET is running"

# Step 5: Start ersap-et-receiver to bridge E2SAR data to ET system
echo "----------------------------------------"
echo "Starting ersap-et-receiver..."

# Determine the receiver IP address
# IMPORTANT: Docker networking constraints
# - Bridge mode: Container MUST bind to 0.0.0.0 (cannot bind to host IP from inside container)
# - Host mode: Can bind to actual host IPs
#
# Priority:
# 1. Use RECV_IP environment variable if set
# 2. Detect Docker bridge networking and use 0.0.0.0 (bind to all interfaces)
# 3. Try to detect primary routable IP via default route (host networking)

if [ -n "${RECV_IP:-}" ]; then
    echo "✓ Using RECV_IP from environment: ${RECV_IP}"
else
    # Detect if we're in Docker bridge networking
    # Check for Docker bridge IP (172.17.x.x, 172.18.x.x) or /.dockerenv file
    IS_DOCKER_BRIDGE=false

    if [ -f /.dockerenv ]; then
        # We're definitely in a container
        # Check if we have a Docker bridge IP (172.17.x.x, etc.)
        if hostname -I 2>/dev/null | grep -qE '172\.(1[67]|18)\.[0-9]+\.[0-9]+'; then
            IS_DOCKER_BRIDGE=true
        fi
    fi

    if [ "$IS_DOCKER_BRIDGE" = true ]; then
        # In Docker bridge mode: bind to 0.0.0.0 to accept traffic on all interfaces
        RECV_IP="0.0.0.0"
        echo "✓ Detected Docker bridge networking, binding to 0.0.0.0 (all interfaces)"
        echo "  Note: Ensure you map ports with -p 10000:10000/udp when running container"
    else
        # Not in bridge mode (or in host networking): try to find the actual IP
        # This asks: "What source IP would I use to reach the internet?"
        RECV_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K\S+' || true)

        # If that didn't work, try hostname -I
        if [ -z "${RECV_IP}" ]; then
            RECV_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
        fi

        if [ -z "${RECV_IP}" ]; then
            echo "ERROR: Could not determine receiver IP address"
            echo ""
            echo "Please set RECV_IP environment variable:"
            echo "  docker run -e RECV_IP=<ip-address> -e EJFAT_URI=<uri> ..."
            echo ""
            echo "Use 0.0.0.0 for Docker bridge mode, or specific IP for host networking"
            exit 1
        fi

        echo "✓ Auto-detected receiver IP: ${RECV_IP}"
    fi
fi

RECEIVER_CMD="ersap-et-receiver -u ${EJFAT_URI} --withcp -v --recv-ip ${RECV_IP} --recv-port 10000 --recv-threads 8 --et-file /tmp/et_sys"
echo "Command: ${RECEIVER_CMD}"
echo "✓ Receiver IP: ${RECV_IP}"

# Check if ersap-et-receiver is available
if ! command -v ersap-et-receiver &> /dev/null; then
    echo "ERROR: ersap-et-receiver not found in PATH"
    echo "PATH: ${PATH}"
    exit 1
fi

# Start receiver in background
${RECEIVER_CMD} &
RECEIVER_PID=$!
echo "✓ ersap-et-receiver started with PID: ${RECEIVER_PID}"

# Give receiver a moment to initialize
sleep 1

# Check if receiver is still running
if ! kill -0 ${RECEIVER_PID} 2>/dev/null; then
    echo "ERROR: ersap-et-receiver failed to start or exited immediately"
    exit 1
fi

echo "✓ ersap-et-receiver is running"

# Step 6: Start ERSAP in foreground (as PID 1 for proper signal handling)
echo "----------------------------------------"
echo "Starting ERSAP..."
ERSAP_SCRIPT="${ERSAP_USER_DATA}/haidis.ersap"
ERSAP_CMD="ersap-shell ${ERSAP_SCRIPT}"
echo "Command: ${ERSAP_CMD}"

# Check if ersap-shell is available
if ! command -v ersap-shell &> /dev/null; then
    echo "ERROR: ersap-shell not found in PATH"
    echo "PATH: ${PATH}"
    exit 1
fi

# Check if ERSAP script exists
if [ ! -f "${ERSAP_SCRIPT}" ]; then
    echo "ERROR: ERSAP script not found at ${ERSAP_SCRIPT}"
    exit 1
fi

echo "=========================================="
echo "All systems initialized. Starting ERSAP..."
echo "=========================================="

# Use exec to replace this shell with ERSAP, making it PID 1
# This ensures proper signal handling (SIGTERM, etc.)
exec ersap-shell "${ERSAP_SCRIPT}"
