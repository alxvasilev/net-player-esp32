#!/bin/bash
curl -i -X POST 192.168.1.9:80/ota -H "Content-Type: application/octet-stream"   --data-binary "@build/netplayer.bin" --progress-bar > /dev/null

