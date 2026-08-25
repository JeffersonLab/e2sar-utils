#!/bin/bash

# This script attempts to stop (not always successfully) a GlueX file sender

# Usage: ./stop_gluex_sender.sh [options]

# Options:
#  --lbid       Which load balancer URL to use (1 is default, production/stable, 2 is for testing only)

# Always be sure to stop the right sender (1 or 2)

SINGLETONPREFIX="/tmp/e2sar-loop-running"
LOOPPREFIX="/tmp/stop-e2sar-loop"
CONTAINERPREFIX="e2sar-root"
LBID=1

usage() {
    head -10 "$0" | tail -9
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --lbid)
            LBID="$2"
            shift 2
            ;;
        --help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

LOOPFILE=${LOOPPREFIX}-${LBID}
SINGLETONFILE=${SINGLETONPREFIX}-${LBID}
CONTAINERNAME=${CONTAINERPREFIX}-${LBID}

touch ${LOOPFILE}

# Kill the start script via the PID stored in the singleton file.
if [[ -f "${SINGLETONFILE}" ]]; then
    START_PID=$(cat "${SINGLETONFILE}" 2>/dev/null)
    [[ -n "${START_PID}" ]] && kill -9 "${START_PID}" 2>/dev/null || true
fi

podman stop --time 10 ${CONTAINERNAME} 2>/dev/null || \
    podman kill ${CONTAINERNAME} 2>/dev/null || true
rm -f ${SINGLETONFILE} ${LOOPFILE}

