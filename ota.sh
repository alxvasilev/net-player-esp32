#!/bin/bash
set -e

source `dirname "$0"`/esp-host.sh
RETRY="--retry 20 --retry-connrefused --connect-timeout 1 --retry-delay 0"

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
        ninja
    else
        echo "Waiting target to reconnect..."
        sleep 3
    fi
else
    # already in recovery
    if [ "$1" == "make" ]; then
        ninja
    fi
    echo "Target is already in recovery mode, not rebooting"
fi

ELF_FILE=$(ls ./*.elf)
if [ -z "$ELF_FILE" ]; then
    echo "No .elf file present in current directory"
    exit 1
fi
BIN_FILE="$(basename -s .elf $ELF_FILE).bin"
echo binfile=$BIN_FILE
if [ ! -f "$BIN_FILE" ]; then
    echo "No .bin file present in current directory"
    exit 1
fi

echo "Erasing flash and sending image $BIN_FILE ($((`stat --printf="%s" $BIN_FILE` / 1024))KB)..."
curl $RETRY -i -X POST ${ESP_HOST}:80/ota -H "Content-Type: application/octet-stream"   --data-binary "@$BIN_FILE" --progress-bar > /dev/null

if [ "$?" -eq 0 ]; then
    echo Success, reboting....
else
   echo Failed
   exit 1
fi

if [ "$1" == "make" ] && [ "$2" == "log" ]; then
    nc $ESPLINK_HOST 23
fi
