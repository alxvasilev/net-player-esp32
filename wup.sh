#!/bin/bash
source ./esp-host.sh
curl -i -X POST ${ESP_HOST}:80/file/spiffs/$1 -H "Content-Type: text/html"   --data-binary "@www/$1"
