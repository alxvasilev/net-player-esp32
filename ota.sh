#!/bin/bash
set -e
source ./esp-host.sh
RETRY="--retry 20 --retry-connrefused --connect-timeout 1 --retry-delay 1"

ISRECOVERY=`curl -s -S $RETRY -o /dev/null -w "%{http_code}" http://${ESP_HOST}:80/isrecovery`
if [ "$ISRECOVERY" != "200" ]; then
    echo -n "Rebooting to recovery..."
    RECOVERY_URL="http://${ESP_HOST}:80/reboot?recovery=1"
    if [ "$1" == "make" ]; then
        RECOVERY_URL+="&flags=1"
	fi
	curl -s -S $RETRY "$RECOVERY_URL"
    echo
    if [ "$1" == "make" ]; then
        make -j
    else
        echo "Waiting target to reconnect..."
        sleep 3
    fi
else
    # already in recovery
    if [ "$1" == "make" ]; then
        make -j
    fi
    echo "Target is already in recovery mode, not rebooting"
fi

echo "Erasing flash and sending image..."
curl $RETRY -i -X POST ${ESP_HOST}:80/ota -H "Content-Type: application/octet-stream"   --data-binary "@build/netplayer.bin" --progress-bar > /dev/null

if [ "$?" -eq 0 ]; then
    echo Success, reboting....
else
   echo Failed
   exit 1
fi

if [ "$1" == "make" ] && [ "$2" == "log" ]; then
    nc $ESPLINK_HOST 23
fi
