if [ -z "$ESP_HOST" ]; then
    export ESP_HOST="netplayer.local"
    echo "ESP device host is $ESP_HOST"
else
    echo "ESP device host is $ESP_HOST (from env variable)"
fi

if [ -z "$ESPLINK_HOST" ]; then
    export ESPLINK_HOST="192.168.1.78"
fi
