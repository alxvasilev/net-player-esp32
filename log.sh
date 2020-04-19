#!/bin/sh
if [ -z "$ESP_IP" ]; then
    ESP_IP=192.168.1.5
fi
curl ${ESP_IP}:80/log
