#!/bin/bash

# kill_all.sh
# Stops ET, ersap-et-receiver, ersap-shell, and DPE processes

set -euo pipefail

echo "=========================================="
echo "Stopping E2SAR EJFAT Processes"
echo "=========================================="

GRACE_PERIOD=5

stop_process() {
    local PROC_NAME="$1"

    PIDS=$(pgrep -f "${PROC_NAME}" || true)

    if [ -z "${PIDS}" ]; then
        echo "✓ No running ${PROC_NAME} processes found"
        return
    fi

    echo "Found ${PROC_NAME} PIDs: ${PIDS}"
    echo "Sending SIGTERM to ${PROC_NAME}..."
    pkill -15 -f "${PROC_NAME}" || true

    echo "Waiting ${GRACE_PERIOD}s for ${PROC_NAME} to exit..."
    sleep ${GRACE_PERIOD}

    STILL_RUNNING=$(pgrep -f "${PROC_NAME}" || true)
    if [ -n "${STILL_RUNNING}" ]; then
        echo "⚠ ${PROC_NAME} still running. Sending SIGKILL..."
        pkill -9 -f "${PROC_NAME}" || true
    else
        echo "✓ ${PROC_NAME} stopped gracefully"
    fi
}

echo "------------------------------------------"
echo "Stopping ERSAP (ersap-shell)..."
stop_process "ersap-shell"

echo "------------------------------------------"
echo "Stopping ersap-et-receiver..."
stop_process "ersap-et-receiver"

echo "------------------------------------------"
echo "Stopping ET system (et_start)..."
stop_process "et_start"

echo "------------------------------------------"
echo "Stopping ERSAP DPEs using kill-dpes..."
if [ -n "${ERSAP_HOME:-}" ] && [ -x "${ERSAP_HOME}/bin/kill-dpes" ]; then
    "${ERSAP_HOME}/bin/kill-dpes" || true
    echo "✓ kill-dpes executed"
else
    echo "WARNING: ${ERSAP_HOME}/bin/kill-dpes not found or not executable"
fi

echo "=========================================="
echo "All E2SAR EJFAT processes stopped"
echo "=========================================="
