#!/bin/bash
source ./esp-host.sh
curl -i -X POST ${ESP_HOST}:80/slist/import -H "Content-Type: text/json"   --data-binary "@$1"
