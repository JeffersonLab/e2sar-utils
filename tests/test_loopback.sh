#!/bin/bash
#
# Loopback test for e2sar-root sender/receiver
#
# This script tests the complete data pipeline:
# 1. Starts a receiver listening on loopback interface
# 2. Runs the sender with parallel file processing
# 3. Verifies all buffers were received
# 4. Reports success/failure
#
# Usage: ./tests/test_loopback.sh [options]
#
# Options:
#   --timeout N     Receiver timeout in seconds (default: 30)
#   --bufsize N     Batch size in MB (default: 1)
#   --mtu N         MTU size (default: 9000)
#   --files N       Number of times to process the test file (default: 2)
#   --help          Show this help message
#

set -e

# Default configuration
TIMEOUT=30
BUFSIZE_MB=1
MTU=9000
NUM_FILES=2
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
EXECUTABLE="$BUILD_DIR/src/e2sar-root"
TEST_DATA="$PROJECT_ROOT/dalitz_toy_data_0/dalitz_root_file_0.root"
TREE_NAME="dalitz_root_tree"
OUTPUT_DIR=$(mktemp -d)
RECV_LOG="$OUTPUT_DIR/receiver.log"
SEND_LOG="$OUTPUT_DIR/sender.log"

# EJFAT URI for loopback testing (no control plane)
EJFAT_URI='ejfat://token@127.0.0.1:18347/lb/0?sync=127.0.0.1&data=127.0.0.1'

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    head -20 "$0" | tail -17
    exit 0
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."

    # Kill receiver if still running
    if [[ -n "$RECV_PID" ]] && kill -0 "$RECV_PID" 2>/dev/null; then
        kill -INT "$RECV_PID" 2>/dev/null || true
        wait "$RECV_PID" 2>/dev/null || true
    fi

    # Remove event files
    rm -f "$OUTPUT_DIR"/event_*.dat 2>/dev/null || true

    # Keep logs on failure, remove on success
    if [[ "$TEST_PASSED" == "true" ]]; then
        rm -rf "$OUTPUT_DIR"
    else
        log_warn "Logs preserved in: $OUTPUT_DIR"
    fi
}

trap cleanup EXIT

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --bufsize)
            BUFSIZE_MB="$2"
            shift 2
            ;;
        --mtu)
            MTU="$2"
            shift 2
            ;;
        --files)
            NUM_FILES="$2"
            shift 2
            ;;
        --help)
            usage
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            ;;
    esac
done

# Verify prerequisites
log_info "Checking prerequisites..."

if [[ ! -x "$EXECUTABLE" ]]; then
    log_error "Executable not found: $EXECUTABLE"
    log_error "Please build the project first: meson compile -C build/"
    exit 1
fi

if [[ ! -f "$TEST_DATA" ]]; then
    log_error "Test data not found: $TEST_DATA"
    exit 1
fi

# Build file list for sender
FILE_ARGS=""
for ((i=0; i<NUM_FILES; i++)); do
    FILE_ARGS="$FILE_ARGS $TEST_DATA"
done

log_info "Test configuration:"
echo "  Executable:  $EXECUTABLE"
echo "  Test data:   $TEST_DATA"
echo "  Files:       $NUM_FILES"
echo "  Batch size:  $BUFSIZE_MB MB"
echo "  MTU:         $MTU"
echo "  Timeout:     $TIMEOUT seconds"
echo "  Output dir:  $OUTPUT_DIR"
echo ""

# Start receiver in background
log_info "Starting receiver..."
cd "$OUTPUT_DIR"

"$EXECUTABLE" -r \
    -u "$EJFAT_URI" \
    --recv-ip 127.0.0.1 \
    -o "event_{:08d}.dat" \
    > "$RECV_LOG" 2>&1 &

RECV_PID=$!
log_info "Receiver started with PID: $RECV_PID"

# Wait for receiver to initialize
sleep 2

# Verify receiver is running
if ! kill -0 "$RECV_PID" 2>/dev/null; then
    log_error "Receiver failed to start. Log:"
    cat "$RECV_LOG"
    exit 1
fi

# Start sender
log_info "Starting sender with $NUM_FILES file(s) in parallel..."
cd "$PROJECT_ROOT"

"$EXECUTABLE" -s \
    -u "$EJFAT_URI" \
    --tree "$TREE_NAME" \
    --bufsize-mb "$BUFSIZE_MB" \
    --mtu "$MTU" \
    $FILE_ARGS \
    > "$SEND_LOG" 2>&1

SEND_EXIT=$?

if [[ $SEND_EXIT -ne 0 ]]; then
    log_error "Sender failed with exit code: $SEND_EXIT"
    log_error "Sender log:"
    cat "$SEND_LOG"
    exit 1
fi

log_info "Sender completed successfully"

# Extract sender statistics
BUFFERS_SENT=$(grep "Total buffers submitted:" "$SEND_LOG" | awk '{print $NF}')
SEND_ERRORS=$(grep "Send errors:" "$SEND_LOG" | awk '{print $NF}')

log_info "Sender stats: $BUFFERS_SENT buffers sent, $SEND_ERRORS errors"

# Wait for receiver to process remaining data (with timeout)
log_info "Waiting up to $TIMEOUT seconds for receiver to finish processing..."

WAIT_START=$(date +%s)
EXPECTED_FILES=$BUFFERS_SENT

while true; do
    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - WAIT_START))

    if [[ $ELAPSED -ge $TIMEOUT ]]; then
        log_warn "Timeout reached after $TIMEOUT seconds"
        break
    fi

    # Count received files
    RECEIVED_FILES=$(ls "$OUTPUT_DIR"/event_*.dat 2>/dev/null | wc -l | tr -d ' ')

    if [[ "$RECEIVED_FILES" -ge "$EXPECTED_FILES" ]]; then
        log_info "All $EXPECTED_FILES files received"
        break
    fi

    # Progress update every 5 seconds
    if [[ $((ELAPSED % 5)) -eq 0 ]] && [[ $ELAPSED -gt 0 ]]; then
        log_info "Progress: $RECEIVED_FILES / $EXPECTED_FILES files received ($ELAPSED seconds elapsed)"
    fi

    sleep 1
done

# Stop receiver gracefully
log_info "Stopping receiver..."
kill -INT "$RECV_PID" 2>/dev/null || true
wait "$RECV_PID" 2>/dev/null || true

# Count final results
RECEIVED_FILES=$(ls "$OUTPUT_DIR"/event_*.dat 2>/dev/null | wc -l | tr -d ' ')

echo ""
log_info "========== Test Results =========="
echo "  Buffers sent:     $BUFFERS_SENT"
echo "  Files received:   $RECEIVED_FILES"
echo "  Send errors:      $SEND_ERRORS"

# Verify results
if [[ "$RECEIVED_FILES" -eq "$BUFFERS_SENT" ]] && [[ "$SEND_ERRORS" -eq "0" ]]; then
    echo ""
    log_info "${GREEN}TEST PASSED${NC} - All buffers received successfully"
    TEST_PASSED=true
    exit 0
else
    echo ""
    log_error "TEST FAILED"
    if [[ "$RECEIVED_FILES" -ne "$BUFFERS_SENT" ]]; then
        log_error "  Expected $BUFFERS_SENT files, got $RECEIVED_FILES"
    fi
    if [[ "$SEND_ERRORS" -ne "0" ]]; then
        log_error "  $SEND_ERRORS send errors occurred"
    fi

    log_warn "Receiver log tail:"
    tail -20 "$RECV_LOG"

    TEST_PASSED=false
    exit 1
fi
