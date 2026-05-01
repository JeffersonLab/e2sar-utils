#!/bin/bash

# To make things run faster, increase the PARALLEL setting - that is the number of file reader child processes. Between 5-10 makes sense

# To let it exit gracefully you can do `touch /tmp/stop-e2sar-loop`. If you need to kill it urgently use
# $ touch /tmp/stop-e2sar-loop
# $ pkill -KILL start-gluex-sender.sh
# $ podman stop e2sar-root
# but check `htop` to make sure all the children quit before restarting


MTU=9000
DATAID=1
RATE=-1.0
DATAVOL=/nvme/haidis/gluex/eta3pi_trees/data2017
LOOPFILE="/tmp/stop-e2sar-loop"
SINGLETONFILE="/tmp/e2sar-loop-running"
E2SAR_ROOT_VER="0.1.3a7"
PARALLEL=1
BUFFER_SIZE=1

if [ -e $SINGLETONFILE ]; then
	echo "Singleton file $SINGLETONFILE is present, another instance may be running"
	exit -1
fi
touch ${SINGLETONFILE}

while [ ! -f "$LOOPFILE" ];  do
	echo "**** Run 'touch ${LOOPFILE}' to stop"
	podman run --rm -v ${DATAVOL}:/data --network=host --name=e2sar-root --security-opt label=disable docker.io/ibaldin/e2sar-utils:${E2SAR_ROOT_VER} e2sar-root --gluex -s --tree myTree -u 'ejfats://token@ejfat-lb.es.net:18048/lb/60?sync=192.188.29.6:19054&data=192.188.29.54&data=[2001:400:a300::54]' --withcp --mtu  ${MTU} --dataid ${DATAID} --rate ${RATE} --dir /data --parallel ${PARALLEL} --bufsize-mb 1
done

rm ${LOOPFILE}
rm ${SINGLETONFILE}

