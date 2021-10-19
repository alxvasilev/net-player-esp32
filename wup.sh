#!/bin/bash
source ./esp-ip.sh
curl -i -X POST ${ESP_IP}:80/file/spiffs/$1 -H "Content-Type: text/html"   --data-binary "@www/$1"
