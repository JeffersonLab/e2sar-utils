#!/bin/bash

# This script starts sending GlueX data into one of the two load balancer
# Production load balancer (--lbid 1) is the default, while
# testing load balancer (--lbid 2) can also optionaly be used.
# REMEMBER that both sender and receiver have to be using the same LB.

# Usage: ./start_gluex_sender.sh [options]

# Options:
#  --lbid       Which load balancer URL to use (1 is default, production/stable, 2 is for testing only)
#  --parallel   How many parallel file readers to start (1 is the default)
#  --eventsize  How big EJFAT events should be in MB (1MB is the default)

# To make things run faster, increase the PARALLEL setting - that is the number of file reader child processes. Between 5-10 makes sense

# To let it exit gracefully you can do `touch /tmp/stop-e2sar-loop`.
# If you need to kill it urgently use ./stop-gluex-sender [--lbid 1 or 2]]


MTU=9000
DATAID=1
RATE=-1.0
DATAVOL=/nvme/haidis/gluex/eta3pi_trees/data2017
CONTAINERPREFIX="e2sar-root"
LOOPPREFIX="/tmp/stop-e2sar-loop"
SINGLETONPREFIX="/tmp/e2sar-loop-running"
E2SAR_ROOT_VER="latest"
EJFATURIS=('ejfats://a6fd0e1f4cf64948ba7d5d30e1b604c4@ejfat-lb.es.net:18048/lb/60?sync=192.188.29.6:19054&data=192.188.29.54&data=[2001:400:a300::54]'
        'ejfats://fb14841f350d45fda2b730233deff0a8@ejfat-lb.es.net:18008/lb/334?sync=192.188.29.6:19010&data=192.188.29.10&data=[2001:400:a300::10]')

BUFFER_SIZE=1
PARALLEL=1
LBID=1

usage() {
    head -20 "$0" | tail -18
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --lbid)
            LBID="$2"
            shift 2
            ;;
        --parallel)
            PARALLEL="$2"
            shift 2
            ;;
        --eventsize)
            BUFFER_SIZE="$2"
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

SINGLETONFILE=${SINGLETONPREFIX}-${LBID}
LOOPFILE=${LOOPPREFIX}-${LBID}
CONTAINERNAME=${CONTAINERPREFIX}-${LBID}

if [ "$LBID" -lt 1 ] || [ "$LBID" -gt 2 ]; then
        echo "Invalid LB ID, only 1 or 2 allowed"
        exit 1
fi

EJFAT_URI=${EJFATURIS[$(($LBID -1))]}

if [ -e $SINGLETONFILE ]; then
        echo "Singleton file $SINGLETONFILE is present, another instance may be running"
        exit -1
fi
echo $$ > ${SINGLETONFILE}

echo Using the following EJFAT_URI: ${EJFAT_URI}

while [ ! -f "$LOOPFILE" ];  do
        echo "**** Run 'touch ${LOOPFILE}' to stop"
        podman run --rm -v ${DATAVOL}:/data --network=host --name=${CONTAINERNAME} --security-opt label=disable docker.io/ibaldin/e2sar-utils:${E2SAR_ROOT_VER} e2sar-root --gluex -s --tree myTree -u ${EJFAT_URI} --withcp --mtu  ${MTU} --dataid ${DATAID} --rate ${RATE} --dir /data --parallel ${PARALLEL} --bufsize-mb ${BUFFER_SIZE}
done

rm ${LOOPFILE}
rm ${SINGLETONFILE}
