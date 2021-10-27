#!/bin/bash
source ./esp-ip.sh
curl -i -X POST ${ESP_IP}:80/slist/import -H "Content-Type: text/json"   --data-binary "@$1"
