#!/bin/bash
if [ -z "$ESP_IP" ]; then
    ESP_IP="192.168.1.5"
fi
curl -i -X POST ${ESP_IP}:80/ota -H "Content-Type: application/octet-stream"   --data-binary "@build/netplayer.bin" --progress-bar > /dev/null

