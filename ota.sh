#!/bin/bash
source ./esp-ip.sh
curl -i -X POST ${ESP_IP}:80/ota -H "Content-Type: application/octet-stream"   --data-binary "@build/netplayer.bin" --progress-bar > /dev/null

