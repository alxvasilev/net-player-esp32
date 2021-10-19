if [ -z "$ESP_IP" ]; then
    export ESP_IP="192.168.1.15"
    echo "ESP device IP is $ESP_IP"
else
    echo "ESP device IP is $ESP_IP (from env variable)"
fi
