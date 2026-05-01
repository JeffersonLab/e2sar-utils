#!/bin/bash

touch /tmp/stop-e2sar-loop
pkill -KILL start-gluex-sender.sh
podman stop e2sar-root
rm /tmp/e2sar-loop-running /tmp/stop-e2sar-loop

